/**
 * @file xcp_master_calibration_view_test.cpp
 * @brief `tools/xcp_master/calibration_view.hpp` 的单元测试。
 *
 * 依据 `code-plan/xcp_master_标定页与F4自动登记_实施方案.md` §6.1 编写。
 *
 * @par 测试策略
 *   标定视图的行文本被抽成 `CalibrationViewFormatter` 抽象接口，目的就是让
 *   "一行到底长什么样"可以在不起 FTXUI、不连 XCP 的前提下被钉住。每个用例
 *   只改动行数据的一个维度（样本有无、数组、单位、旧值、失效符号），断言文本
 *   里出现/不出现关键标记；再用一个记录型实现验证 `CliApp` 侧依赖的调用契约。
 */

#include "tools/xcp_master/calibration_view.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using calmcar::xcp::tools::BuildCalibrationViewLabels;
using calmcar::xcp::tools::CalibrationViewFormatter;
using calmcar::xcp::tools::CalibrationViewRow;
using calmcar::xcp::tools::ClampCalibrationSelection;
using calmcar::xcp::tools::DefaultCalibrationViewFormatter;
using calmcar::xcp::tools::EmptyCalibrationViewHint;
using calmcar::xcp::tools::NextViewIndex;
using calmcar::xcp::tools::ObservationSnapshot;

/**
 * @brief 构造一个有效标定行的基线
 * @return CHARACTERISTIC/VALUE 类型的标定行
 */
CalibrationViewRow MakeRow() {
    CalibrationViewRow row;
    row.name = "DemoM::Gain";
    row.symbol_valid = true;
    row.kind_name = "CHARACTERISTIC/VALUE";
    row.base_type = "FLOAT32_IEEE";
    row.data_shape = "标量";
    row.unit = "V";
    return row;
}

/**
 * @brief 断言文本包含指定子串
 * @param text 被检查文本
 * @param needle 期望出现的子串
 */
void ExpectContains(const std::string& text, const std::string& needle) {
    EXPECT_NE(text.find(needle), std::string::npos)
        << "文本为: " << text << "；期望包含: " << needle;
}

/**
 * @brief 默认格式化器的各维显示规则。
 */
class DefaultCalibrationViewFormatterTest : public ::testing::Test {
protected:
    DefaultCalibrationViewFormatter formatter_;
};

/** @brief 无样本时显示破折号占位，而不是空。 */
TEST_F(DefaultCalibrationViewFormatterTest, ShowsPlaceholderWithoutSample) {
    const auto text = formatter_.Format(MakeRow(), false);
    ExpectContains(text, "DemoM::Gain");
    ExpectContains(text, "CHARACTERISTIC/VALUE");
    ExpectContains(text, "—");
}

/** @brief 有标量样本时输出值与单位。 */
TEST_F(DefaultCalibrationViewFormatterTest, ShowsScalarValueWithUnit) {
    auto row = MakeRow();
    row.sample.has_value = true;
    row.sample.value = "1.25";
    const auto text = formatter_.Format(row, false);
    ExpectContains(text, "1.25 V");
}

/** @brief 数组样本只显示元素个数，不拼接全部元素。 */
TEST_F(DefaultCalibrationViewFormatterTest, ShowsArrayElementCount) {
    auto row = MakeRow();
    row.sample.has_value = true;
    row.sample.is_array = true;
    row.sample.array_values = {"1", "2", "3"};
    const auto text = formatter_.Format(row, false);
    ExpectContains(text, "[3 elements]");
}

/** @brief 旧值样本必须带显式标记，避免误读成实时值。 */
TEST_F(DefaultCalibrationViewFormatterTest, MarksStaleSample) {
    auto row = MakeRow();
    row.sample.has_value = true;
    row.sample.value = "0.5";
    row.sample.stale = true;
    ExpectContains(formatter_.Format(row, false), "（旧值）");
}

/** @brief 已停止采样仍显示值，但带「已停止」。 */
TEST_F(DefaultCalibrationViewFormatterTest, MarksStoppedSample) {
    auto row = MakeRow();
    row.sample.has_value = true;
    row.sample.value = "0.5";
    row.sample.stopped = true;
    ExpectContains(formatter_.Format(row, false), "（已停止）");
}

/** @brief 宽屏显示时间戳，窄屏省略以省列宽。 */
TEST_F(DefaultCalibrationViewFormatterTest, ShowsTimestampOnlyWhenWide) {
    auto row = MakeRow();
    row.sample.has_value = true;
    row.sample.value = "0.5";
    row.sample.timestamp = "12:00:01.250";
    ExpectContains(formatter_.Format(row, false), "12:00:01.250");
    EXPECT_EQ(formatter_.Format(row, true).find("12:00:01.250"),
              std::string::npos);
}

/** @brief 没有样本但有状态文本时显示状态，例如「读取失败」。 */
TEST_F(DefaultCalibrationViewFormatterTest, ShowsStatusWithoutValue) {
    auto row = MakeRow();
    row.sample.status = "读取失败";
    ExpectContains(formatter_.Format(row, false), "读取失败");
}

/** @brief A2L 候选失效的行仍显示名称，并提示可用 Del 移出。 */
TEST_F(DefaultCalibrationViewFormatterTest, MarksInvalidSymbol) {
    auto row = MakeRow();
    row.symbol_valid = false;
    const auto text = formatter_.Format(row, false);
    ExpectContains(text, "DemoM::Gain");
    ExpectContains(text, "A2L候选已失效");
    ExpectContains(text, "Del");
}

/** @brief 无单位时不产生多余空格。 */
TEST_F(DefaultCalibrationViewFormatterTest, OmitsUnitWhenEmpty) {
    auto row = MakeRow();
    row.unit.clear();
    row.sample.has_value = true;
    row.sample.value = "0.5";
    const auto text = formatter_.Format(row, false);
    ExpectContains(text, "0.5");
    EXPECT_EQ(text.find("0.5 "), std::string::npos) << "文本为: " << text;
}

/**
 * @brief 列表构造：空名单与多行的映射规则。
 */
class CalibrationViewLabelsTest : public ::testing::Test {
protected:
    DefaultCalibrationViewFormatter formatter_;
};

/** @brief 空名单也必须产出一行提示，否则菜单无焦点可落。 */
TEST(CalibrationViewLabelsTest, EmptyListYieldsHintLine) {
    const auto labels =
        BuildCalibrationViewLabels({}, DefaultCalibrationViewFormatter(), false);
    ASSERT_EQ(labels.size(), 1U);
    EXPECT_EQ(labels.front(), EmptyCalibrationViewHint());
}

/** @brief 有行时按顺序一一映射，不插入额外提示行。 */
TEST(CalibrationViewLabelsTest, MapsRowsInOrder) {
    std::vector<CalibrationViewRow> rows;
    auto first = MakeRow();
    first.name = "DemoM::A";
    auto second = MakeRow();
    second.name = "DemoM::B";
    rows.push_back(first);
    rows.push_back(second);
    const auto labels =
        BuildCalibrationViewLabels(rows, DefaultCalibrationViewFormatter(), false);
    ASSERT_EQ(labels.size(), 2U);
    ExpectContains(labels[0], "DemoM::A");
    ExpectContains(labels[1], "DemoM::B");
}

/**
 * @brief 可以替换成派生实现，验证虚函数契约可被 `CliApp` 复用。
 */
class RecordingFormatter final : public CalibrationViewFormatter {
public:
    /**
     * @brief 记录调用并返回固定文本
     * @param row 行数据
     * @param compact 紧凑模式
     * @return 固定字符串
     */
    [[nodiscard]] std::string Format(const CalibrationViewRow& row,
                                     bool compact) const override {
        ++calls;
        last_name = row.name;
        last_compact = compact;
        return "recorded:" + row.name;
    }

    mutable int calls{0};    ///< 被调用次数。
    mutable std::string last_name; ///< 最近一次的行名。
    mutable bool last_compact{false}; ///< 最近一次的紧凑标志。
};

/** @brief 替换实现后，列表构造只走虚接口，不依赖默认实现。 */
TEST(CalibrationViewFormatterContractTest, DerivedFormatterIsUsed) {
    RecordingFormatter formatter;
    std::vector<CalibrationViewRow> rows{MakeRow()};
    const auto labels = BuildCalibrationViewLabels(rows, formatter, true);
    ASSERT_EQ(labels.size(), 1U);
    EXPECT_EQ(labels.front(), "recorded:DemoM::Gain");
    EXPECT_EQ(formatter.calls, 1);
    EXPECT_EQ(formatter.last_name, "DemoM::Gain");
    EXPECT_TRUE(formatter.last_compact);
}

/** @brief 选中行夹紧：越界回落到最后一行，空列表回到零。 */
TEST(CalibrationSelectionTest, ClampsSelectionIntoRange) {
    EXPECT_EQ(ClampCalibrationSelection(0, 0U), 0);
    EXPECT_EQ(ClampCalibrationSelection(5, 0U), 0);
    EXPECT_EQ(ClampCalibrationSelection(-3, 4U), 0);
    EXPECT_EQ(ClampCalibrationSelection(2, 4U), 2);
    EXPECT_EQ(ClampCalibrationSelection(9, 4U), 3);
}

/** @brief 四视图下 F2 循环：预览→标定→结果→日志→预览。 */
TEST(CalibrationViewNavigationTest, F2CyclesFourViews) {
    int view = 0;
    const std::vector<int> expected{1, 2, 3, 0};
    for (const int want : expected)
        EXPECT_EQ(view = NextViewIndex(view, 4U), want);
}

}  // namespace
