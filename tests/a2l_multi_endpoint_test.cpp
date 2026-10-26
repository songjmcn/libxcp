// =============================================================================
// a2l_multi_endpoint_test.cpp —— 多实例传输层端点（历史遗漏项 L-C，ABI v10）
//
// 背景：ABI v9 的 `IfDataXcpDto::endpoint_*`
// 只承载**首个**实例，`XCP_ON_TCP_IP`
// 出现两次时第二个端点的事实（端口/主机/对齐/版本）在 SDK 边界即被丢弃，于是
// 消费方的"多实例 fail closed"永远看不到第二个实例，形同死代码。
//
// v10 新增 `IfDataXcpDto::endpoints`（逐实例向量），桥接层为每个实例各发布一个
// `TransportEndpoint`。本文件守住三件事：
//   1. 同一声明里两个 XCP_ON_TCP_IP / 两个 XCP_ON_UDP_IP 都要出现，顺序与 A2L
//   一致；
//   2. 每个端点的端口/主机/对齐/声明位/TL 版本都取自**自己**那一块，不得串味；
//   3. `instance_count` 报总数，供消费方判定"是否存在多实例歧义"。
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "libxcp/a2l/a2l_bridge.hpp"

namespace {

using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::IfDataXcpInfo;
using calmcar::xcp::a2l::LoadOptions;
using calmcar::xcp::a2l::TransportEndpoint;

/// @brief 一个 IF_DATA 块里同时声明两个 TCP 端点与两个 UDP 端点
constexpr std::string_view kMultiEndpointA2l = R"ASAP2(ASAP2_VERSION 1 71
/begin PROJECT multiep ""
/begin MODULE m ""
/begin MOD_COMMON ""
  BYTE_ORDER MSB_LAST
  DATA_SIZE 8
  ALIGNMENT_BYTE 1
  ALIGNMENT_WORD 2
  ALIGNMENT_LONG 4
/end MOD_COMMON
/begin IF_DATA XCP
  /begin PROTOCOL_LAYER 0x104 1 1 5 5 5 1 1 8 64 BYTE_ORDER_MSB_LAST ADDRESS_GRANULARITY_BYTE /end PROTOCOL_LAYER
  /begin XCP_ON_UDP_IP 0x104 5555
    ADDRESS "192.168.0.30"
  /end XCP_ON_UDP_IP
  /begin XCP_ON_UDP_IP 0x104 5556
    ADDRESS "192.168.0.31"
    PACKET_ALIGNMENT PACKET_ALIGNMENT_32
  /end XCP_ON_UDP_IP
  /begin XCP_ON_TCP_IP 0x104 5557
    HOST_NAME "ecu-a.local"
    ADDRESS "192.168.0.20"
    PACKET_ALIGNMENT PACKET_ALIGNMENT_8
  /end XCP_ON_TCP_IP
  /begin XCP_ON_TCP_IP 0x104 5558
    HOST_NAME "ecu-b.local"
    ADDRESS "192.168.0.21"
    PACKET_ALIGNMENT PACKET_ALIGNMENT_16
  /end XCP_ON_TCP_IP
/end IF_DATA
/end MODULE
/end PROJECT
)ASAP2";

/**
 * @brief 把内联 A2L 写到临时目录并加载
 * @param test_name 用例名（用于互不冲突的文件名）
 * @return 桥接对象；失败返回 nullptr 并记录断言
 */
std::unique_ptr<A2lBridge> LoadMultiEndpointA2l(const std::string& test_name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "libxcp_multi_endpoint";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / (test_name + ".a2l");
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            ADD_FAILURE() << "无法写入测试 A2L：" << file;
            return nullptr;
        }
        output.write(kMultiEndpointA2l.data(),
                     static_cast<std::streamsize>(kMultiEndpointA2l.size()));
    }

    LoadOptions options;
    options.require_if_data_xcp = true;
    auto loaded = A2lBridge::Load(file.string(), options);
    if (!loaded.HasValue()) {
        ADD_FAILURE() << "A2L 加载失败：" << loaded.ErrorInfo().message;
        return nullptr;
    }
    return std::move(loaded.Value());
}

/**
 * @brief 收集指定类别的端点（保持 A2L 声明顺序）
 * @param info IF_DATA XCP 汇总
 * @param kind 传输类别
 * @return 指向各端点的指针
 */
std::vector<const TransportEndpoint*> CollectEndpoints(
    const IfDataXcpInfo& info, TransportEndpoint::Kind kind) {
    std::vector<const TransportEndpoint*> matched;
    for (const auto& endpoint : info.transports) {
        if (endpoint.kind == kind) {
            matched.push_back(&endpoint);
        }
    }
    return matched;
}

TEST(A2lMultiEndpointTest, ExposesEveryDeclaredTcpEndpoint) {
    auto bridge = LoadMultiEndpointA2l("tcp_two");
    ASSERT_NE(bridge, nullptr);
    const IfDataXcpInfo* info = bridge->XcpInfo();
    ASSERT_NE(info, nullptr);

    const auto tcps = CollectEndpoints(*info, TransportEndpoint::Kind::TcpIp);
    ASSERT_EQ(tcps.size(), 2U) << "两个 XCP_ON_TCP_IP 都必须进入数据模型";

    // 顺序与 A2L 声明顺序一致，且每个端点的字段取自自己那一块。
    EXPECT_EQ(tcps[0]->remote_port, 5557);
    EXPECT_EQ(tcps[0]->remote_host, "ecu-a.local");
    EXPECT_EQ(tcps[0]->packet_alignment, 8U);
    EXPECT_TRUE(tcps[0]->packet_alignment_declared);
    EXPECT_EQ(tcps[0]->version, 0x104);

    EXPECT_EQ(tcps[1]->remote_port, 5558);
    EXPECT_EQ(tcps[1]->remote_host, "ecu-b.local");
    EXPECT_EQ(tcps[1]->packet_alignment, 16U);
    EXPECT_TRUE(tcps[1]->packet_alignment_declared);
    EXPECT_EQ(tcps[1]->version, 0x104);

    // 两个端点都必须报出"同类共 2 个"，否则消费方无法察觉歧义。
    EXPECT_EQ(tcps[0]->instance_count, 2U);
    EXPECT_EQ(tcps[1]->instance_count, 2U);
}

TEST(A2lMultiEndpointTest, ExposesEveryDeclaredUdpEndpoint) {
    auto bridge = LoadMultiEndpointA2l("udp_two");
    ASSERT_NE(bridge, nullptr);
    const IfDataXcpInfo* info = bridge->XcpInfo();
    ASSERT_NE(info, nullptr);

    const auto udps = CollectEndpoints(*info, TransportEndpoint::Kind::UdpIp);
    ASSERT_EQ(udps.size(), 2U) << "两个 XCP_ON_UDP_IP 都必须进入数据模型";

    // 第一个端点未声明 PACKET_ALIGNMENT：对齐只是 AML 默认值，不是证据。
    EXPECT_EQ(udps[0]->remote_port, 5555);
    EXPECT_EQ(udps[0]->remote_host, "192.168.0.30");
    EXPECT_FALSE(udps[0]->packet_alignment_declared)
        << "未声明 PACKET_ALIGNMENT 时不得给出声明位";

    // 第二个端点显式声明 32 bit，且不得被第一个端点的取值污染。
    EXPECT_EQ(udps[1]->remote_port, 5556);
    EXPECT_EQ(udps[1]->remote_host, "192.168.0.31");
    EXPECT_EQ(udps[1]->packet_alignment, 32U);
    EXPECT_TRUE(udps[1]->packet_alignment_declared);
}

TEST(A2lMultiEndpointTest, FirstEndpointStaysUnambiguousForSingleTargetUse) {
    auto bridge = LoadMultiEndpointA2l("first_endpoint");
    ASSERT_NE(bridge, nullptr);
    const IfDataXcpInfo* info = bridge->XcpInfo();
    ASSERT_NE(info, nullptr);

    // 单目标消费方（如 CLI 未显式指定端点时）需要能安全地取"第一个"，同时
    // 通过 instance_count 知道自己看到的不是全部——这两点必须同时成立，
    // 否则"多个实例"要么被静默当成唯一端点，要么根本无法使用。
    const auto tcps = CollectEndpoints(*info, TransportEndpoint::Kind::TcpIp);
    ASSERT_EQ(tcps.size(), 2U);
    EXPECT_EQ(tcps[0]->remote_port, 5557);
    EXPECT_EQ(tcps[0]->instance_count, 2U);

    // 顺序约定：`transport` 选中的类别在前（本 A2L 两种都有，选中的是 TCP），
    // 另一类别随后，同类内部按 A2L 声明顺序。这样"取第一个"的旧消费方行为
    // 与 v9 一致，而新消费方仍能看到全部四个端点。
    EXPECT_EQ(info->transports.front().kind, TransportEndpoint::Kind::TcpIp);
    EXPECT_EQ(info->transports.size(), 4U);
}

}  // namespace