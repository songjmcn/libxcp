// =============================================================================
// a2l_if_data_conflict_test.cpp —— XCP vs XCPplus 的"对齐声明状态"差异（L-B2）
//
// 背景（code-plan/历史遗漏项_详细方案.md §2）：`DiffIfData` 原先只比对
// `PACKET_ALIGNMENT` 的**数值**。于是下面这种组合被静默放过：
//   XCP    块：未声明 PACKET_ALIGNMENT（AML 默认 8 bit，不构成证据）
//   XCPplus块：显式 PACKET_ALIGNMENT_8
// 两者数值相同（都是 8），但语义不同——前者"ECU 没给对齐证据"，后者
// "明确要求 8 bit"。本文件守住修复后的契约：
//   1. 数值相同但声明状态不同 → 追加一行 `PACKET_ALIGNMENT_DECLARED`；
//   2. 两侧声明状态一致 → 不得出现该行（避免噪声）。
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include "libxcp/a2l/a2l_bridge.hpp"

namespace {

using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::IfDataXcpInfo;
using calmcar::xcp::a2l::LoadOptions;

/// @brief XCP 块未声明对齐、XCPplus 块显式 8 bit 的最小模块
constexpr std::string_view kUndeclaredVersusEightBit =
    R"ASAP2(ASAP2_VERSION 1 71
/begin PROJECT alignconf ""
/begin MODULE m ""
/begin MOD_COMMON ""
  BYTE_ORDER MSB_LAST
  DATA_SIZE 8
  ALIGNMENT_BYTE 1
  ALIGNMENT_WORD 2
  ALIGNMENT_LONG 4
/end MOD_COMMON
/begin IF_DATA XCP
  /begin PROTOCOL_LAYER 0x100 1 1 5 5 5 1 1 8 64 BYTE_ORDER_MSB_LAST ADDRESS_GRANULARITY_BYTE /end PROTOCOL_LAYER
  /begin XCP_ON_UDP_IP 0x100 5555
    HOST_NAME "localhost"
  /end XCP_ON_UDP_IP
/end IF_DATA
/begin IF_DATA XCPplus 11
  /begin PROTOCOL_LAYER 0x100 1 1 5 5 5 1 1 8 64 BYTE_ORDER_MSB_LAST ADDRESS_GRANULARITY_BYTE /end PROTOCOL_LAYER
  /begin XCP_ON_UDP_IP 0x100 5555
    HOST_NAME "localhost"
    PACKET_ALIGNMENT PACKET_ALIGNMENT_8
  /end XCP_ON_UDP_IP
/end IF_DATA
/end MODULE
/end PROJECT
)ASAP2";

/// @brief 两侧都显式声明同一个位宽的最小模块（不得产生声明状态差异）
constexpr std::string_view kBothDeclaredEightBit = R"ASAP2(ASAP2_VERSION 1 71
/begin PROJECT alignconf2 ""
/begin MODULE m ""
/begin MOD_COMMON ""
  BYTE_ORDER MSB_LAST
  DATA_SIZE 8
  ALIGNMENT_BYTE 1
  ALIGNMENT_WORD 2
  ALIGNMENT_LONG 4
/end MOD_COMMON
/begin IF_DATA XCP
  /begin PROTOCOL_LAYER 0x100 1 1 5 5 5 1 1 8 64 BYTE_ORDER_MSB_LAST ADDRESS_GRANULARITY_BYTE /end PROTOCOL_LAYER
  /begin XCP_ON_UDP_IP 0x100 5555
    HOST_NAME "localhost"
    PACKET_ALIGNMENT PACKET_ALIGNMENT_8
  /end XCP_ON_UDP_IP
/end IF_DATA
/begin IF_DATA XCPplus 11
  /begin PROTOCOL_LAYER 0x100 1 1 5 5 5 1 1 8 64 BYTE_ORDER_MSB_LAST ADDRESS_GRANULARITY_BYTE /end PROTOCOL_LAYER
  /begin XCP_ON_UDP_IP 0x100 5555
    HOST_NAME "localhost"
    PACKET_ALIGNMENT PACKET_ALIGNMENT_8
  /end XCP_ON_UDP_IP
/end IF_DATA
/end MODULE
/end PROJECT
)ASAP2";

/**
 * @brief 把内联 A2L 写到临时目录并加载
 * @param test_name 用例名（用于互不冲突的文件名）
 * @param text A2L 文本
 * @return 桥接对象；失败时返回 nullptr 并记录断言失败
 */
std::unique_ptr<A2lBridge> LoadInlineA2l(const std::string& test_name,
                                         std::string_view text) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "libxcp_if_data_conflict";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / (test_name + ".a2l");
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            ADD_FAILURE() << "无法写入测试 A2L：" << file;
            return nullptr;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
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
 * @brief 在差异列表里查找指定参数名
 * @param info IF_DATA XCP 汇总
 * @param parameter 参数名
 * @return 命中返回指针，否则 nullptr
 */
const calmcar::xcp::a2l::XcpPlusConflict* FindConflict(
    const IfDataXcpInfo& info, std::string_view parameter) {
    for (const auto& conflict : info.plus_conflicts) {
        if (conflict.parameter == parameter) {
            return &conflict;
        }
    }
    return nullptr;
}

TEST(A2lIfDataConflictTest, ReportsDeclaredStateMismatchWhenValuesMatch) {
    auto bridge =
        LoadInlineA2l("undeclared_vs_eight", kUndeclaredVersusEightBit);
    ASSERT_NE(bridge, nullptr);
    const IfDataXcpInfo* info = bridge->XcpInfo();
    ASSERT_NE(info, nullptr);

    // 数值相同 → 数值行不得出现
    EXPECT_EQ(FindConflict(*info, "PACKET_ALIGNMENT"), nullptr)
        << "两侧数值都是 8 bit，不应产生数值差异行";

    const auto* declared = FindConflict(*info, "PACKET_ALIGNMENT_DECLARED");
    ASSERT_NE(declared, nullptr)
        << "XCP 未声明、XCPplus 显式 8 bit：数值相同但证据强度不同，必须报";
    EXPECT_EQ(declared->xcp_value, "未声明");
    EXPECT_EQ(declared->xcpplus_value, "已声明");
}

TEST(A2lIfDataConflictTest, KeepsDeclaredStateQuietWhenBothAgree) {
    auto bridge = LoadInlineA2l("both_eight", kBothDeclaredEightBit);
    ASSERT_NE(bridge, nullptr);
    const IfDataXcpInfo* info = bridge->XcpInfo();
    ASSERT_NE(info, nullptr);

    // 两侧都显式 8 bit：数值与声明状态都一致，不得有任何对齐相关差异
    EXPECT_EQ(FindConflict(*info, "PACKET_ALIGNMENT"), nullptr);
    EXPECT_EQ(FindConflict(*info, "PACKET_ALIGNMENT_DECLARED"), nullptr)
        << "声明状态一致时不得追加噪声行";
}

}  // namespace