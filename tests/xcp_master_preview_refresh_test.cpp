/**
 * @file xcp_master_preview_refresh_test.cpp
 * @brief 验证预览行索引与增量刷新（P1）的语义与代价。
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tools/xcp_master/preview_refresh.hpp"

namespace {

using calmcar::xcp::tools::PreviewRow;
using calmcar::xcp::tools::PreviewRowIndex;
using calmcar::xcp::tools::RefreshPreviewRows;

/**
 * @brief 构造一个父行。
 * @param name 符号规范名。
 * @return 父行描述。
 */
PreviewRow MakeParent(const std::string& name) {
    return PreviewRow{name, std::nullopt, false, false, false};
}

/**
 * @brief 构造一个数组元素子行。
 * @param name 所属符号规范名。
 * @param element 元素序号。
 * @return 子行描述。
 */
PreviewRow MakeChild(const std::string& name, std::uint64_t element) {
    return PreviewRow{name, element, true, false, false};
}

/**
 * @brief 构造"父行 + 若干子行"的行布局。
 * @param parents 父符号名，每个父行后跟 count 个子行。
 * @param children_per_parent 每个父符号的子行数量。
 * @return 行布局。
 */
std::vector<PreviewRow> MakeRows(const std::vector<std::string>& parents,
                                 std::size_t children_per_parent = 3U) {
    std::vector<PreviewRow> rows;
    for (const auto& name : parents) {
        rows.push_back(MakeParent(name));
        for (std::size_t index = 0; index < children_per_parent; ++index)
            rows.push_back(MakeChild(name, static_cast<std::uint64_t>(index)));
    }
    return rows;
}

}  // namespace

/** 父行与全部子行都必须登记在同一个符号名下。 */
TEST(XcpMasterPreviewRefreshTest, IndexesParentAndChildRowsByName) {
    const auto rows = MakeRows({"cal.gain", "cal.offset"});
    PreviewRowIndex index;
    index.Rebuild(rows);

    ASSERT_EQ(index.Size(), 2U);
    const auto* first = index.Find("cal.gain");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(*first, (std::vector<std::size_t>{0U, 1U, 2U, 3U}));
    const auto* second = index.Find("cal.offset");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(*second, (std::vector<std::size_t>{4U, 5U, 6U, 7U}));
    EXPECT_EQ(index.Find("cal.missing"), nullptr);
}

/** 重建索引必须丢弃上一版内容，避免行布局变化后残留旧行号。 */
TEST(XcpMasterPreviewRefreshTest, RebuildDropsPreviousRows) {
    PreviewRowIndex index;
    index.Rebuild(MakeRows({"a", "b"}));
    EXPECT_NE(index.Find("b"), nullptr);

    index.Rebuild(MakeRows({"a"}, 1U));
    EXPECT_EQ(index.Size(), 1U);
    EXPECT_EQ(index.Find("b"), nullptr);
    const auto* a = index.Find("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(*a, (std::vector<std::size_t>{0U, 1U}));
}

/** 增量刷新只能改动该符号占据的行，其余行必须逐字节保持不变。 */
TEST(XcpMasterPreviewRefreshTest, RefreshesOnlyItsOwnRows) {
    const auto rows = MakeRows({"cal.gain", "cal.offset"});
    PreviewRowIndex index;
    index.Rebuild(rows);
    std::vector<std::string> labels(rows.size(), "哨兵");

    const auto refreshed = RefreshPreviewRows(
        labels, index, "cal.gain",
        [](std::size_t row_index) { return "行" + std::to_string(row_index); });

    EXPECT_EQ(refreshed, 4U);
    for (std::size_t row_index = 0; row_index < 4U; ++row_index)
        EXPECT_EQ(labels[row_index], "行" + std::to_string(row_index));
    for (std::size_t row_index = 4U; row_index < labels.size(); ++row_index)
        EXPECT_EQ(labels[row_index], "哨兵");
}

/** 未出现在可见行集合中的符号必须被静默忽略。 */
TEST(XcpMasterPreviewRefreshTest, RefreshOfInvisibleSymbolIsNoop) {
    const auto rows = MakeRows({"cal.gain"});
    PreviewRowIndex index;
    index.Rebuild(rows);
    std::vector<std::string> labels(rows.size(), "哨兵");

    const auto refreshed = RefreshPreviewRows(
        labels, index, "cal.missing",
        [](std::size_t) { return std::string("被刷新"); });

    EXPECT_EQ(refreshed, 0U);
    for (const auto& label : labels) EXPECT_EQ(label, "哨兵");
}

/** 标签数组短于索引时（占位行场景）必须跳过越界行而不是崩溃。 */
TEST(XcpMasterPreviewRefreshTest, SkipsIndicesBeyondLabelArray) {
    const auto rows = MakeRows({"cal.gain"});
    PreviewRowIndex index;
    index.Rebuild(rows);
    std::vector<std::string> labels(2U, "哨兵");

    const auto refreshed = RefreshPreviewRows(
        labels, index, "cal.gain",
        [](std::size_t row_index) { return "行" + std::to_string(row_index); });

    EXPECT_EQ(refreshed, 2U);
    EXPECT_EQ(labels.size(), 2U);
    EXPECT_EQ(labels[0], "行0");
    EXPECT_EQ(labels[1], "行1");
}

/**
 * 增量刷新的代价必须与候选总数无关：198k 行规模下单次刷新远低于 5 ms，
 * 而改造前的整表重建实测为 335 ms。
 */
TEST(XcpMasterPreviewRefreshTest, RefreshesTwoHundredThousandRowsWithinBudget) {
    constexpr std::size_t kCandidateCount = 198556U;
    std::vector<PreviewRow> rows;
    rows.reserve(kCandidateCount + 4U);
    for (std::size_t index = 0; index < kCandidateCount; ++index)
        rows.push_back(MakeParent("sym_" + std::to_string(index)));
    const std::string target = "sym_100000";
    for (std::size_t index = 0; index < 4U; ++index)
        rows.push_back(MakeChild(target, static_cast<std::uint64_t>(index)));

    PreviewRowIndex index;
    index.Rebuild(rows);
    std::vector<std::string> labels(rows.size(), "哨兵");

    const auto started = std::chrono::steady_clock::now();
    const auto refreshed = RefreshPreviewRows(
        labels, index, target,
        [](std::size_t row_index) { return "行" + std::to_string(row_index); });
    const auto elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started)
            .count();

    std::printf("[PERF] 增量刷新：198556 行中只刷新 %zu 行 = %.3f ms\n",
                refreshed, elapsed_ms);
    EXPECT_EQ(refreshed, 5U);
    EXPECT_LT(elapsed_ms, 5.0);
    EXPECT_EQ(labels[0], "哨兵");
    EXPECT_EQ(labels[kCandidateCount], "行" + std::to_string(kCandidateCount));
}