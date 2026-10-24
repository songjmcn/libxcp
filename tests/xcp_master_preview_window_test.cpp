/**
 * @file xcp_master_preview_window_test.cpp
 * @brief 验证虚拟列表窗口计算（P2）的逐条规则。
 */
#include <cstddef>

#include <gtest/gtest.h>

#include "tools/xcp_master/preview_window.hpp"

namespace {

using calmcar::xcp::tools::ComputePreviewWindow;
using calmcar::xcp::tools::PreviewWindowBounds;

}  // namespace

/** 空列表不渲染任何行。 */
TEST(XcpMasterPreviewWindowTest, EmptyListHasEmptyWindow) {
    const auto bounds = ComputePreviewWindow(0U, 0U, 20U, 0U);
    EXPECT_EQ(bounds.first, 0U);
    EXPECT_EQ(bounds.count, 0U);
}

/** 行数不超过可视行数时窗口覆盖全部行且不滚动。 */
TEST(XcpMasterPreviewWindowTest, ShortListShowsEverything) {
    const auto bounds = ComputePreviewWindow(5U, 3U, 20U, 0U);
    EXPECT_EQ(bounds.first, 0U);
    EXPECT_EQ(bounds.count, 5U);
}

/** 选中行仍在窗口内时必须保持窗口不动，避免刷新时抖动。 */
TEST(XcpMasterPreviewWindowTest, KeepsWindowWhenSelectionVisible) {
    const auto bounds = ComputePreviewWindow(1000U, 25U, 20U, 20U);
    EXPECT_EQ(bounds.first, 20U);
    EXPECT_EQ(bounds.count, 20U);
}

/** 选中行在窗口上方时窗口首行跟随选中行。 */
TEST(XcpMasterPreviewWindowTest, ScrollsUpToSelection) {
    const auto bounds = ComputePreviewWindow(1000U, 5U, 20U, 40U);
    EXPECT_EQ(bounds.first, 5U);
    EXPECT_EQ(bounds.count, 20U);
}

/** 选中行在窗口下方时窗口滚动到让选中行成为最后一行。 */
TEST(XcpMasterPreviewWindowTest, ScrollsDownToSelection) {
    const auto bounds = ComputePreviewWindow(1000U, 60U, 20U, 40U);
    EXPECT_EQ(bounds.first, 41U);
    EXPECT_EQ(bounds.count, 20U);
    EXPECT_EQ(bounds.first + bounds.count - 1U, 60U);
}

/** 上一帧首行越界时（列表变短）必须钳位到末页。 */
TEST(XcpMasterPreviewWindowTest, ClampsStaleFirstRow) {
    // 选中行落在钳位后的窗口内，因此只发生钳位、不发生跟随滚动。
    const auto bounds = ComputePreviewWindow(100U, 85U, 20U, 500U);
    EXPECT_EQ(bounds.first, 80U);
    EXPECT_EQ(bounds.count, 20U);
}

/** 选中行不在钳位后的窗口内时，钳位后仍要跟随选中行。 */
TEST(XcpMasterPreviewWindowTest, StaleFirstRowStillFollowsSelection) {
    const auto bounds = ComputePreviewWindow(100U, 0U, 20U, 500U);
    EXPECT_EQ(bounds.first, 0U);
    EXPECT_EQ(bounds.count, 20U);
}

/** 可视行数为 0 时按 1 行处理，不能返回空窗口。 */
TEST(XcpMasterPreviewWindowTest, TreatsZeroVisibleRowsAsOne) {
    const auto bounds = ComputePreviewWindow(10U, 3U, 0U, 0U);
    EXPECT_EQ(bounds.count, 1U);
    EXPECT_EQ(bounds.first, 3U);
}

/** 选中行超界时按最后一行计算（End 键后仍能看到末行）。 */
TEST(XcpMasterPreviewWindowTest, ClampsSelectionBeyondRange) {
    const auto bounds = ComputePreviewWindow(100U, 500U, 20U, 0U);
    EXPECT_EQ(bounds.first, 80U);
    EXPECT_EQ(bounds.count, 20U);
}