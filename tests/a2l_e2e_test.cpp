// =============================================================================
// a2l_e2e_test.cpp —— 批次10 工作包 10.3：A2L → XCP 端到端验收
//
// 覆盖 R4 文档头部的首个验收里程碑（前半）：
//   1) IF_DATA XCP 自动建链：Slave 绑定 A2L 声明端口，Transport 端点
//      完全取自 XcpInfo()（host 经集成层解析，见 ToTransportAddress 注释）；
//   2) A2L 与 Slave 运行时一致性：GetSessionParameters → CompareWithRuntime
//      （B-16：0 个 Error；已知差异恰为 MAX_CTO/MAX_DTO 两条 Warning）；
//   3) 变量名 → 地址 → SHORT_UPLOAD 读值：符号表寻址（B-1）驱动真实读，
//      raw → ToPhysical（B-8/B-14），含 RL 推导后的 CHARACTERISTIC；
//   4) 错误路径：未预置内存的地址 → 结构化 XcpException（不静默）。
//
// 装配胶水只在本测试内（不新增生产公开 API，AGENTS.md 约束）。
// **写路径（DOWNLOAD）不在本批**：libxcp 核心尚无写内存接口（协议层仅有
// 命令码枚举），扩展属核心命令层，需用户明确指示，随 10.4（DAQ 命令组）
// 一并处理 —— 详见批次10记录 §5。
// =============================================================================

#include <gtest/gtest.h>

#include <cctype>
#include <cstdint>
#include <memory>
#include <string>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/command_executor.hpp"  // CommandTimeouts
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "udp_test_slave.hpp"

#ifndef A2L_GOLDEN_DIR
#error "A2L_GOLDEN_DIR 缺失（tests/CMakeLists.txt 注入）"
#endif

namespace {

using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::AddressGranularity;
using calmcar::xcp::a2l::ByteOrder;
using calmcar::xcp::a2l::Bytes;
using calmcar::xcp::a2l::IA2lDatabase;
using calmcar::xcp::a2l::LoadOptions;
using calmcar::xcp::a2l::PhysicalValue;
using calmcar::xcp::a2l::RuntimeXcpParams;
using calmcar::xcp::a2l::Severity;
using calmcar::xcp::a2l::SymbolInfo;
namespace test = calmcar::xcp::test;

/// @brief golden_mask 的 IF_DATA 声明端口（fixture 的 Slave 必须绑定同端口）
constexpr std::uint16_t kA2lPort = 0x15B9;

/**
 * @brief A2L 主机名 → Transport 可用地址（集成胶水，见批次10记录 §5）
 * @details `udp_transport.cpp:253` 的 parseIpv4 只接受 IPv4 字面量，
 * 且远端过滤用字面量比较（:465）——名称解析属后续集成层职责，本批只做
 * 确定性的本地映射（localhost → 127.0.0.1），非回环名原样透传并由连接报错暴露。
 */
std::string ToTransportAddress(const std::string& host) {
    if (host.empty()) {
        return host;
    }
    std::string lower = host;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower == "localhost" ? "127.0.0.1" : host;
}

/**
 * @brief XCP 版本字节 → A2L PROTOCOL_LAYER 版本（uint16 风格）
 * @details XCP CONNECT RES 的 PROTOCOL_VERSION 高/低 nibble = major/minor
 * （0x10 = 1.0）；A2L 侧 0x0100 = major 1 / minor 0。
 */
std::uint16_t VersionByteToA2l(std::uint8_t version_byte) {
    const auto major = static_cast<std::uint16_t>((version_byte >> 4) & 0x0F);
    const auto minor = static_cast<std::uint16_t>(version_byte & 0x0F);
    return static_cast<std::uint16_t>((major << 8) | minor);
}

/// @brief golden 样本路径（build tree，fixture 生成）
std::string Golden(const char* name) {
    return std::string(A2L_GOLDEN_DIR) + "/" + name;
}

/**
 * @brief 端到端夹具：固定端口 Slave（预置内存）+ A2L 装载 + A2L 端点自动建链
 * @details 每个用例独立重建（Slave 端口 0x15B9 被占用时构造抛异常，用例
 * 直接失败并给出明确信息——端口冲突即环境问题，不做静默回退）。
 */
class A2lE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        // 1) Slave：绑定 A2L 声明端口 + 预置 ECU 模拟内存（地址来自符号表）
        slave_ = std::make_unique<test::UdpTestSlave>(kA2lPort);
        slave_->Start();
        const Bytes mask_raw{0x00, 0x0F};  // M_MASK：0x0F00 → 掩码提取 15
        slave_->SetMemory(0x3000, mask_raw);
        const Bytes byte_val{0x2A};  // M_UBYTE → IDENTICAL 42
        slave_->SetMemory(0x3020, byte_val);
        const Bytes arr8{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
        slave_->SetMemory(0x3048, arr8);  // M_ARR8（B-1 基址）
        const Bytes ch_val{0x34, 0x12};   // C_VALUE：UWORD 0x1234（RL 推导）
        slave_->SetMemory(0x3100, ch_val);

        // 2) A2L 装载（默认 require_if_data_xcp=true，golden_mask 含 IF_DATA）
        auto loaded = A2lBridge::Load(Golden("golden_mask.a2l"));
        ASSERT_TRUE(loaded.HasValue())
            << "Load 失败: "
            << calmcar::xcp::a2l::ToString(loaded.ErrorInfo().code) << " / "
            << loaded.ErrorInfo().message;
        bridge_ = std::move(loaded).Value();
        ASSERT_NE(bridge_->Database(), nullptr);
        ASSERT_NE(bridge_->XcpInfo(), nullptr);
        ASSERT_EQ(bridge_->XcpInfo()->transports.size(), 1u);
        ASSERT_EQ(bridge_->XcpInfo()->transports[0].kind,
                  calmcar::xcp::a2l::TransportEndpoint::Kind::UdpIp);
        ASSERT_EQ(bridge_->XcpInfo()->transports[0].remote_port, kA2lPort);

        // 3) 自动建链：Transport 端点全部取自 A2L（XcpInfo）快照
        calmcar::xcp::UdpTransportConfig cfg;
        cfg.remote_host =
            ToTransportAddress(bridge_->XcpInfo()->transports[0].remote_host);
        cfg.remote_port = bridge_->XcpInfo()->transports[0].remote_port;
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;
        cfg.receive_poll_interval_ms = 20;
        master_ = std::make_unique<calmcar::xcp::XcpMaster>(
            std::make_unique<calmcar::xcp::UdpTransport>(cfg),
            calmcar::xcp::CommandTimeouts{}, nullptr);
        master_->Connect();
    }

    void TearDown() override {
        // 顺序：master 先关（发 DISCONNECT），再销毁 bridge，最后停 Slave
        master_.reset();
        bridge_.reset();
        slave_.reset();
    }

    std::unique_ptr<test::UdpTestSlave> slave_;
    std::unique_ptr<A2lBridge> bridge_;
    std::unique_ptr<calmcar::xcp::XcpMaster> master_;
};

// ---------------------------------------------------------------------------
// 1) IF_DATA XCP 自动建链 + 会话参数
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, AutoLinkFromA2lEndpointConnects) {
    EXPECT_TRUE(master_->IsConnected());
    EXPECT_TRUE(slave_->IsConnected());
    EXPECT_EQ(master_->GetSessionState(),
              calmcar::xcp::SessionState::Connected);

    const calmcar::xcp::SessionParameters params =
        master_->GetSessionParameters();
    // CONNECT 协商结果（B-16 运行时真值来源）
    EXPECT_EQ(params.connect.max_cto, 8);
    EXPECT_EQ(params.connect.max_dto, 8);
    EXPECT_EQ(params.connect.byte_order, calmcar::xcp::ByteOrder::Intel);
    EXPECT_EQ(params.connect.address_granularity,
              calmcar::xcp::AddressGranularity::Byte);
    EXPECT_EQ(params.connect.protocol_layer_version, 0x10);  // XCP 1.0 字节
    EXPECT_TRUE(calmcar::xcp::HasResource(params.connect.resource_mask,
                                          calmcar::xcp::Resource::Daq));
    // Connect 自动附带 GET_COMM_MODE_INFO 查询
    ASSERT_TRUE(params.comm_mode_info.has_value());
    EXPECT_TRUE(params.short_upload_available);
    // 至少处理 CONNECT + GET_COMM_MODE_INFO + GET_STATUS
    EXPECT_GE(slave_->CommandCount(), 3u);
}

// ---------------------------------------------------------------------------
// 2) A2L ↔ Slave 运行时一致性（B-16：运行时为真值，Error 阻断受影响功能）
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, RuntimeConsistencyMatchesA2l) {
    const calmcar::xcp::SessionParameters params =
        master_->GetSessionParameters();
    const auto& c = params.connect;

    RuntimeXcpParams rt;
    rt.protocol_version = VersionByteToA2l(c.protocol_layer_version);
    rt.max_cto = c.max_cto;
    rt.max_dto = c.max_dto;
    rt.byte_order = (c.byte_order == calmcar::xcp::ByteOrder::Intel)
                        ? ByteOrder::MsbLast
                        : ByteOrder::MsbFirst;
    switch (c.address_granularity) {
        case calmcar::xcp::AddressGranularity::Word:
            rt.address_granularity = AddressGranularity::Word;
            break;
        case calmcar::xcp::AddressGranularity::DWord:
            rt.address_granularity = AddressGranularity::Dword;
            break;
        case calmcar::xcp::AddressGranularity::Byte:
        default:
            rt.address_granularity = AddressGranularity::Byte;
            break;
    }
    rt.has_daq =
        calmcar::xcp::HasResource(c.resource_mask, calmcar::xcp::Resource::Daq);

    auto diff = bridge_->CompareWithRuntime(rt);
    ASSERT_TRUE(diff.HasValue());
    // golden_mask: A2L 声明 MAX_CTO=0x10/MAX_DTO=0x20，Slave 实际 8/8 →
    // 恰好 2 条 Warning（B-16：容量取两端保守值），version/BO/AG/DAQ 全一致。
    EXPECT_EQ(diff.Value().size(), 2u);
    int errors = 0;
    for (const auto& d : diff.Value()) {
        if (d.severity == Severity::Error) {
            ++errors;
        }
        EXPECT_NE(d.parameter_name, std::string("ADDRESS_GRANULARITY"));
        EXPECT_NE(d.parameter_name, std::string("BYTE_ORDER"));
    }
    EXPECT_EQ(errors, 0) << "运行时协商参数一致时不得出现 Error 项（B-16）";
    bool has_cto = false;
    bool has_dto = false;
    for (const auto& d : diff.Value()) {
        EXPECT_EQ(d.severity, Severity::Warning) << d.parameter_name;
        has_cto = has_cto || d.parameter_name == "MAX_CTO";
        has_dto = has_dto || d.parameter_name == "MAX_DTO";
    }
    EXPECT_TRUE(has_cto && has_dto);

    // 反向：runtime major 不兼容 → 必须升为 Error（运行时为真值的意义）
    RuntimeXcpParams major_bad = rt;
    major_bad.protocol_version = 0x0200;
    auto bad = bridge_->CompareWithRuntime(major_bad);
    ASSERT_TRUE(bad.HasValue());
    int major_errors = 0;
    for (const auto& d : bad.Value()) {
        if (d.parameter_name == "PROTOCOL_VERSION_MAJOR") {
            EXPECT_EQ(d.severity, Severity::Error);
            ++major_errors;
        }
    }
    EXPECT_EQ(major_errors, 1);
}

// ---------------------------------------------------------------------------
// 3) 变量名 → 地址 → SHORT_UPLOAD 读值（真实 Socket 往返）
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, ReadScalarsThroughA2lAddress) {
    const IA2lDatabase* db = bridge_->Database();
    ASSERT_NE(db, nullptr);

    // 标量 UBYTE：地址/扩展直通 slave，raw → IDENTICAL 物理值
    auto ub = db->Find("MASK_ECU::M_UBYTE");
    ASSERT_TRUE(ub.HasValue());
    EXPECT_EQ(ub.Value().xcp_address, 0x3020u);
    const Bytes ub_raw = master_->ReadMemoryBytes(
        ub.Value().xcp_address, ub.Value().address_extension,
        ub.Value().element_size_bytes);
    ASSERT_EQ(ub_raw.size(), 1u);
    EXPECT_EQ(ub_raw[0], 0x2A);
    auto ub_phys = db->ToPhysical("MASK_ECU::M_UBYTE", ub_raw);
    ASSERT_TRUE(ub_phys.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(ub_phys.Value()));
    EXPECT_EQ(std::get<std::int64_t>(ub_phys.Value()), 42);

    // 掩码 UWORD：BIT_MASK 值提取（0x0F00 → 15）
    auto masked = db->Find("MASK_ECU::M_MASK");
    ASSERT_TRUE(masked.HasValue());
    EXPECT_EQ(masked.Value().bit_mask, 0x0F00u);
    const Bytes mask_raw = master_->ReadMemoryBytes(
        masked.Value().xcp_address, masked.Value().address_extension, 2);
    auto mask_phys = db->ToPhysical("MASK_ECU::M_MASK", mask_raw);
    ASSERT_TRUE(mask_phys.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(mask_phys.Value()));
    EXPECT_EQ(std::get<std::int64_t>(mask_phys.Value()), 15);

    // 数组 B-1：整段 raw 原样读回（8 字节），元素地址按 AG 推进
    auto arr = db->Find("MASK_ECU::M_ARR8");
    ASSERT_TRUE(arr.HasValue());
    auto byte_size = db->ByteSizeOf("MASK_ECU::M_ARR8");
    ASSERT_TRUE(byte_size.HasValue());
    EXPECT_EQ(byte_size.Value(), 8u);
    const Bytes arr_raw = master_->ReadMemoryBytes(
        arr.Value().xcp_address, arr.Value().address_extension, 8);
    const Bytes arr_expect{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    EXPECT_EQ(arr_raw, arr_expect);
    auto elem3 = calmcar::xcp::a2l::ComputeElementAddress(
        arr.Value(), 3, AddressGranularity::Byte);
    ASSERT_TRUE(elem3.HasValue());
    EXPECT_EQ(elem3.Value(), 0x3048u + 3);  // 0-based：第 4 个元素在 +3
    const Bytes elem3_raw = master_->ReadMemoryBytes(
        elem3.Value(), arr.Value().address_extension, 1);
    ASSERT_EQ(elem3_raw.size(), 1u);
    EXPECT_EQ(elem3_raw[0], 0x03);

    // CHARACTERISTIC：RECORD_LAYOUT 推导出元素类型后走同一读路径（B-4）
    auto ch = db->Find("MASK_ECU::C_VALUE");
    ASSERT_TRUE(ch.HasValue());
    EXPECT_EQ(ch.Value().data_type, calmcar::xcp::a2l::AsamDataType::UWord);
    EXPECT_EQ(ch.Value().element_size_bytes, 2);
    EXPECT_TRUE(ch.Value().read_write);
    EXPECT_EQ(ch.Value().address_extension, 0x12);
    const Bytes ch_raw = master_->ReadMemoryBytes(
        ch.Value().xcp_address, ch.Value().address_extension,
        ch.Value().element_size_bytes);
    ASSERT_EQ(ch_raw.size(), 2u);
    auto ch_phys = db->ToPhysical("MASK_ECU::C_VALUE", ch_raw);
    ASSERT_TRUE(ch_phys.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(ch_phys.Value()));
    EXPECT_EQ(std::get<std::int64_t>(ch_phys.Value()), 0x1234);
}

// ---------------------------------------------------------------------------
// 4) 未预置内存地址 → 结构化失败（不静默返回零填充数据）
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, ReadUnconfiguredAddressThrows) {
    EXPECT_THROW((void)master_->ReadMemoryBytes(0x9F9F, 0x00, 4),
                 calmcar::xcp::XcpException);
}

}  // namespace
