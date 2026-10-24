// =============================================================================
// a2l_calibration_access_test.cpp —— CALIBRATION_ACCESS 可写性判定（方案 A）
//
// 背景：ASAM MCD-2 MC V1.4 起用 CALIBRATION_ACCESS 取代 READ_ONLY 表达可写性
// （规范 §4.7.28：NO_CALIBRATION 只能读、OFFLINE_CALIBRATION 不可在线标定）。
// 修复前 a2l-sdk 只认 READ_ONLY，导致 NO_CALIBRATION 的标定量仍被判为可写。
//
// 本文件用一份内联的极小 A2L 覆盖六种 CHARACTERISTIC 声明与四种 INSTANCE 声明，
// 逐条断言桥接层发布的 read_write 位：
//   - 未声明 / 显式 CALIBRATION → 沿用原判据（可写）
//   - NO_CALIBRATION / OFFLINE_CALIBRATION / NOT_IN_MCD_SYSTEM → 不可写
//   - READ_ONLY / 未声明 READ_WRITE（INSTANCE）→ 不可写
// 同时守一条兼容性约束：XCPlite 系生成器不输出 CALIBRATION_ACCESS，
// 故演示 A2L 的行为必须保持不变（由本文件的 c_plain / i_plain 两条断言覆盖）。
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "libxcp/a2l/a2l_bridge.hpp"

namespace {

using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::LoadOptions;
using calmcar::xcp::a2l::SymbolInfo;

/// 测试用 A2L 文本：每种可写性声明出现一次，符号名自带语义，便于失败定位。
constexpr std::string_view kA2lText = R"ASAP2(ASAP2_VERSION 1 71
/begin PROJECT calaccess ""
/begin MODULE m ""
/begin MOD_COMMON ""
  BYTE_ORDER MSB_LAST
  DATA_SIZE 8
  ALIGNMENT_BYTE 1
  ALIGNMENT_WORD 2
  ALIGNMENT_LONG 4
  ALIGNMENT_INT64 8
/end MOD_COMMON
/begin CHARACTERISTIC c_plain "无访问关键字" VALUE 0x1000 RL 0 NO_COMPU_METHOD 0 255 /end CHARACTERISTIC
/begin CHARACTERISTIC c_readonly "显式只读" VALUE 0x1001 RL 0 NO_COMPU_METHOD 0 255 READ_ONLY /end CHARACTERISTIC
/begin CHARACTERISTIC c_cal "显式可标定" VALUE 0x1002 RL 0 NO_COMPU_METHOD 0 255 CALIBRATION_ACCESS CALIBRATION /end CHARACTERISTIC
/begin CHARACTERISTIC c_nocal "不可标定" VALUE 0x1003 RL 0 NO_COMPU_METHOD 0 255 CALIBRATION_ACCESS NO_CALIBRATION /end CHARACTERISTIC
/begin CHARACTERISTIC c_offline "离线标定" VALUE 0x1004 RL 0 NO_COMPU_METHOD 0 255 CALIBRATION_ACCESS OFFLINE_CALIBRATION /end CHARACTERISTIC
/begin CHARACTERISTIC c_notmcd "不在MCD系统内" VALUE 0x1005 RL 0 NO_COMPU_METHOD 0 255 CALIBRATION_ACCESS NOT_IN_MCD_SYSTEM /end CHARACTERISTIC
/begin TYPEDEF_MEASUREMENT M_leaf "结构体叶子分量" UBYTE NO_COMPU_METHOD 0 0 0 0 /end TYPEDEF_MEASUREMENT
/begin TYPEDEF_STRUCTURE Ts "结构体类型" 0x4
  /begin STRUCTURE_COMPONENT v M_leaf 0x0 /end STRUCTURE_COMPONENT
/end TYPEDEF_STRUCTURE
/begin INSTANCE i_plain "无访问关键字" Ts 0x2000 READ_WRITE /end INSTANCE
/begin INSTANCE i_nocal "不可标定" Ts 0x2010 READ_WRITE CALIBRATION_ACCESS NO_CALIBRATION /end INSTANCE
/begin INSTANCE i_offline "离线标定" Ts 0x2020 READ_WRITE CALIBRATION_ACCESS OFFLINE_CALIBRATION /end INSTANCE
/begin INSTANCE i_nowrite "未声明可写" Ts 0x2030 /end INSTANCE
/end MODULE
/end PROJECT
)ASAP2";

/**
 * @brief 把内联 A2L 落到临时目录并加载成桥接对象。
 * @param test_name 用例名，用于生成互不冲突的临时文件名。
 * @return 加载成功的桥接对象；失败时用断言终止当前用例。
 */
std::unique_ptr<A2lBridge> LoadInlineA2l(const std::string& test_name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "libxcp_calibration_access";
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
    // 这份语料刻意不带 IF_DATA XCP：本用例只验证可写性判定，不验证 XCP 段。
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
        ADD_FAILURE() << "找不到符号 " << qualified
                      << "：" << found.ErrorInfo().message;
        return SymbolInfo{};
    }
    return found.Value();
}

/**
 * @brief 断言某符号的 read_write 位。
 * @param bridge 已加载的桥接对象。
 * @param qualified 全限定名。
 * @param expected 期望的可写性。
 */
void ExpectReadWrite(A2lBridge& bridge, std::string_view qualified,
                     bool expected) {
    const SymbolInfo symbol = FindSymbol(bridge, qualified);
    EXPECT_EQ(symbol.read_write, expected) << "符号 " << qualified;
}

// ---------------------------------------------------------------------------
// CHARACTERISTIC：CALIBRATION_ACCESS 参与可写性判定
// ---------------------------------------------------------------------------

/// 未声明任何访问关键字：沿用原判据（可写），保证 XCPlite 语料行为不变。
TEST(A2lCalibrationAccessTest, CharacteristicWithoutKeywordStaysWritable) {
    auto bridge = LoadInlineA2l("characteristic_without_keyword");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::c_plain", true);
}

/// READ_ONLY 仍然有效。
TEST(A2lCalibrationAccessTest, CharacteristicReadOnlyStaysReadOnly) {
    auto bridge = LoadInlineA2l("characteristic_read_only");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::c_readonly", false);
}

/// 显式 CALIBRATION 不额外禁止写入。
TEST(A2lCalibrationAccessTest, CharacteristicExplicitCalibrationIsWritable) {
    auto bridge = LoadInlineA2l("characteristic_explicit_calibration");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::c_cal", true);
}

/// NO_CALIBRATION：规范要求"不可改、只能读"→ 不可写（修复前误判为可写）。
TEST(A2lCalibrationAccessTest, CharacteristicNoCalibrationIsRejected) {
    auto bridge = LoadInlineA2l("characteristic_no_calibration");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::c_nocal", false);
}

/// OFFLINE_CALIBRATION：可刷写但不可在线标定 → 不可写。
TEST(A2lCalibrationAccessTest, CharacteristicOfflineCalibrationIsRejected) {
    auto bridge = LoadInlineA2l("characteristic_offline_calibration");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::c_offline", false);
}

/// NOT_IN_MCD_SYSTEM：MCD 系统不可读写 → 不可写。
TEST(A2lCalibrationAccessTest, CharacteristicNotInMcdIsRejected) {
    auto bridge = LoadInlineA2l("characteristic_not_in_mcd");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::c_notmcd", false);
}

// ---------------------------------------------------------------------------
// INSTANCE：CALIBRATION_ACCESS 与 READ_WRITE 同时生效
//
// 注意断言目标：INSTANCE 自身在快照里是"类型节点"（kind=Type，size=0，
// read_write 恒 false），真正承载数据的是展开后的叶子（i_xxx.v）。
// 因此这里逐条断言叶子，避免误把容器节点当成数据符号。
// ---------------------------------------------------------------------------

/// READ_WRITE 且未声明 CALIBRATION_ACCESS：可写（XCPlite 生成物的形态）。
TEST(A2lCalibrationAccessTest, InstanceWithReadWriteStaysWritable) {
    auto bridge = LoadInlineA2l("instance_with_read_write");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::i_plain.v", true);
}

/// READ_WRITE + NO_CALIBRATION：CALIBRATION_ACCESS 优先禁止。
TEST(A2lCalibrationAccessTest, InstanceWithNoCalibrationIsRejected) {
    auto bridge = LoadInlineA2l("instance_no_calibration");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::i_nocal.v", false);
}

/// READ_WRITE + OFFLINE_CALIBRATION：不可在线标定。
TEST(A2lCalibrationAccessTest, InstanceWithOfflineCalibrationIsRejected) {
    auto bridge = LoadInlineA2l("instance_offline_calibration");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::i_offline.v", false);
}

/// 未声明 READ_WRITE：即使没有 CALIBRATION_ACCESS 也不可写（ASAP2 缺省只读）。
TEST(A2lCalibrationAccessTest, InstanceWithoutReadWriteIsRejected) {
    auto bridge = LoadInlineA2l("instance_without_read_write");
    ASSERT_NE(bridge, nullptr);
    ExpectReadWrite(*bridge, "m::i_nowrite.v", false);
}

}  // namespace