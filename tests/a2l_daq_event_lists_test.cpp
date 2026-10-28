// =============================================================================
// a2l_daq_event_lists_test.cpp —— DAQ_EVENT 四张事件表的导出（ABI v11）
//
// 背景：XCP 协议层 §4.1.5 规定「MEASUREMENT 未声明 DEFAULT_EVENT_LIST 时，应以
// CONSISTENCY_EVENT_LIST 作为默认事件通道分配」（shall）。v10 的 DTO 只有
// fixed/default/available 三张表，消费方无从得知这条兜底分配；同时
// CHARACTERISTIC 分支（BuildCharacteristicSymbol）此前**完全不导出**
// DAQ_EVENT， 导致带事件的标定量在 DTO 里恒显示「未声明事件」。
//
// 本文件用一份内联 A2L 覆盖四种声明形态，逐条断言桥接层发布的四张表：
//   - `FIXED_EVENT_LIST` 形式（与 VARIABLE 互斥）：只有第一张表非空
//   - `VARIABLE` 形式且四表齐全：default/available/consistency 各自独立、不串表
//   - 仅声明 CONSISTENCY 的 MEASUREMENT：只有第四张表非空
//   - 带事件的 CHARACTERISTIC：v11 起与 MEASUREMENT 同口径导出
// 注意 A2L 语法层约束：`FIXED_EVENT_LIST` 与 `VARIABLE`（其下才有
// AVAILABLE/DEFAULT/CONSISTENCY）**互斥**，见
// thirdparty/a2llib/src/xcp/xcpdataparser.y:689
// `daq_event_union: fixed_event_list | variable;`，故不存在"一个符号同时声明
// fixed 与 default"的合法 A2L。
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libxcp/a2l/a2l_bridge.hpp"

namespace {

using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::LoadOptions;
using calmcar::xcp::a2l::SymbolInfo;

/// 测试用 A2L：三种事件声明形态各一份，符号名自带语义便于失败定位。
constexpr std::string_view kA2lText = R"ASAP2(ASAP2_VERSION 1 71
/begin PROJECT daqeventlists ""
/begin MODULE m ""
/begin MOD_COMMON ""
  BYTE_ORDER MSB_LAST
  DATA_SIZE 8
  ALIGNMENT_BYTE 1
  ALIGNMENT_WORD 2
  ALIGNMENT_LONG 4
  ALIGNMENT_INT64 8
/end MOD_COMMON
/begin COMPU_METHOD CM "identity" IDENTICAL "%6.2" ""
/end COMPU_METHOD
/begin MEASUREMENT M_FIXED "固定事件形式" UBYTE CM 0 0 0 255
  ECU_ADDRESS 0x1000
  /begin IF_DATA XCP
    /begin DAQ_EVENT FIXED_EVENT_LIST EVENT 0x0 /end DAQ_EVENT
  /end IF_DATA
/end MEASUREMENT
/begin MEASUREMENT M_VARIABLE "可变量形式四表齐全" UBYTE CM 0 0 0 255
  ECU_ADDRESS 0x1003
  /begin IF_DATA XCP
    /begin DAQ_EVENT VARIABLE
      /begin AVAILABLE_EVENT_LIST EVENT 0x0 EVENT 0x1 EVENT 0x2 /end AVAILABLE_EVENT_LIST
      /begin DEFAULT_EVENT_LIST EVENT 0x1 /end DEFAULT_EVENT_LIST
      /begin CONSISTENCY_EVENT_LIST EVENT 0x2 /end CONSISTENCY_EVENT_LIST
    /end DAQ_EVENT
  /end IF_DATA
/end MEASUREMENT
/begin MEASUREMENT M_CONS_ONLY "仅声明一致性事件" UBYTE CM 0 0 0 255
  ECU_ADDRESS 0x1001
  /begin IF_DATA XCP
    /begin DAQ_EVENT VARIABLE
      /begin CONSISTENCY_EVENT_LIST EVENT 0x4 /end CONSISTENCY_EVENT_LIST
    /end DAQ_EVENT
  /end IF_DATA
/end MEASUREMENT
/begin CHARACTERISTIC C_EVT "带事件的标定量" VALUE 0x1002 UBYTE 0 CM 0 255
  /begin IF_DATA XCP
    /begin DAQ_EVENT VARIABLE
      /begin AVAILABLE_EVENT_LIST EVENT 0x5 EVENT 0x6 EVENT 0x7 /end AVAILABLE_EVENT_LIST
      /begin DEFAULT_EVENT_LIST EVENT 0x6 /end DEFAULT_EVENT_LIST
      /begin CONSISTENCY_EVENT_LIST EVENT 0x7 /end CONSISTENCY_EVENT_LIST
    /end DAQ_EVENT
  /end IF_DATA
/end CHARACTERISTIC
/end MODULE
/end PROJECT
)ASAP2";

/**
 * @brief 把内联 A2L 落到临时目录并加载成桥接对象。
 * @param test_name 用例名，用于生成互不冲突的临时文件名。
 * @return 加载成功的桥接对象；失败时用断言记录并返回空指针。
 */
std::unique_ptr<A2lBridge> LoadInlineA2l(const std::string& test_name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "libxcp_daq_event_lists";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / (test_name + ".a2l");
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            // 该辅助函数返回非 void，不能用 ASSERT_*（GTest 限制）
            ADD_FAILURE() << "无法写入测试 A2L：" << file;
            return nullptr;
        }
        output.write(kA2lText.data(),
                     static_cast<std::streamsize>(kA2lText.size()));
    }

    LoadOptions options;
    // 语料只声明 DAQ_EVENT，不含 DAQ 公共参数块；本次只验证事件表导出。
    options.require_if_data_xcp = false;
    auto loaded = A2lBridge::Load(file.string(), options);
    if (!loaded.HasValue()) {
        ADD_FAILURE() << "A2L 加载失败：" << loaded.ErrorInfo().message;
        return nullptr;
    }
    return std::move(loaded.Value());
}

/**
 * @brief 查询一个符号并返回其快照；查不到直接判失败。
 * @param bridge 已加载的桥接对象。
 * @param qualified 全限定名（module::name）。
 * @return 符号快照。
 */
SymbolInfo FindSymbol(A2lBridge& bridge, std::string_view qualified) {
    const auto found = bridge.Database()->Find(qualified);
    if (!found.HasValue()) {
        ADD_FAILURE() << "找不到符号 " << qualified << "："
                      << found.ErrorInfo().message;
        return SymbolInfo{};
    }
    return found.Value();
}

/// FIXED_EVENT_LIST 形式：只有第一张表非空，其余三张必须为空。
TEST(A2lDaqEventListsTest, ExportsFixedEventListForm) {
    auto bridge = LoadInlineA2l("fixed_form");
    if (!bridge) return;
    const auto symbol = FindSymbol(*bridge, "m::M_FIXED");

    EXPECT_EQ(symbol.daq_event_fixed, (std::vector<std::uint16_t>{0}));
    EXPECT_TRUE(symbol.daq_event_default.empty());
    EXPECT_TRUE(symbol.daq_event_available.empty());
    EXPECT_TRUE(symbol.daq_consistency_event.empty());
}

/// VARIABLE 形式且四表齐全：各表独立，不串表。
TEST(A2lDaqEventListsTest, ExportsVariableEventListsSeparately) {
    auto bridge = LoadInlineA2l("variable_form");
    if (!bridge) return;
    const auto symbol = FindSymbol(*bridge, "m::M_VARIABLE");

    // FIXED 与 VARIABLE 互斥：这份语料没有 fixed。
    EXPECT_TRUE(symbol.daq_event_fixed.empty());
    EXPECT_EQ(symbol.daq_event_default, (std::vector<std::uint16_t>{1}));
    EXPECT_EQ(symbol.daq_event_available,
              (std::vector<std::uint16_t>{0, 1, 2}));
    EXPECT_EQ(symbol.daq_consistency_event, (std::vector<std::uint16_t>{2}));
}

/// 只声明 CONSISTENCY 的 MEASUREMENT：前三张为空，第四张有值。
/// 这是 v11 修复的核心场景——修复前该符号在 DTO 里显示「未声明任何事件」。
TEST(A2lDaqEventListsTest, ExportsConsistencyListWhenOthersAbsent) {
    auto bridge = LoadInlineA2l("consistency_only");
    if (!bridge) return;
    const auto symbol = FindSymbol(*bridge, "m::M_CONS_ONLY");

    EXPECT_TRUE(symbol.daq_event_fixed.empty());
    EXPECT_TRUE(symbol.daq_event_default.empty());
    EXPECT_TRUE(symbol.daq_event_available.empty());
    EXPECT_EQ(symbol.daq_consistency_event, (std::vector<std::uint16_t>{4}));
}

/// CHARACTERISTIC 的事件表：v11 起与 MEASUREMENT 同口径导出。
/// 行为中性——它能否进 DAQ 由主站策略决定（只接受 MEASUREMENT），此处只锁导出。
TEST(A2lDaqEventListsTest, ExportsCharacteristicEventLists) {
    auto bridge = LoadInlineA2l("characteristic_events");
    if (!bridge) return;
    const auto symbol = FindSymbol(*bridge, "m::C_EVT");

    // 语料走 VARIABLE 形式，故 fixed 为空（语法互斥，见文件头说明）。
    EXPECT_TRUE(symbol.daq_event_fixed.empty());
    EXPECT_EQ(symbol.daq_event_default, (std::vector<std::uint16_t>{6}));
    EXPECT_EQ(symbol.daq_event_available,
              (std::vector<std::uint16_t>{5, 6, 7}));
    EXPECT_EQ(symbol.daq_consistency_event, (std::vector<std::uint16_t>{7}));
}

}  // namespace