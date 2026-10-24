/**
 * @file preview_window.hpp
 * @brief 虚拟列表的窗口计算（纯逻辑，不依赖 FTXUI，可单独单测）。
 */
#pragma once

#include <algorithm>
#include <cstddef>

namespace calmcar::xcp::tools {

/**
 * @brief 虚拟列表的可视窗口：起始行与行数。
 */
struct PreviewWindowBounds {
    std::size_t first{0U};  ///< 窗口首行（0 基）。
    std::size_t count{0U};  ///< 窗口行数。
};

/**
 * @brief 计算虚拟列表应渲染的窗口，必要时滚动以保持选中行可见。
 * @param total 总行数。
 * @param selected 当前选中行（0 基，超界自动钳位）。
 * @param visible_rows 可视行数（0 按 1 处理）。
 * @param current_first 上一次窗口首行，用于避免抖动。
 * @return 新的窗口边界。
 * @note 选中行已在窗口内时保持 `current_first`，只有越界才滚动，
 *       否则高速采样刷新时窗口会随选中行抖动。
 */
[[nodiscard]] inline PreviewWindowBounds ComputePreviewWindow(
    std::size_t total, std::size_t selected, std::size_t visible_rows,
    std::size_t current_first) noexcept {
    if (total == 0U) return PreviewWindowBounds{0U, 0U};
    const std::size_t rows = std::max<std::size_t>(1U, visible_rows);
    if (total <= rows) return PreviewWindowBounds{0U, total};

    const std::size_t count = rows;
    const std::size_t max_first = total - count;
    const std::size_t clamped_selected = std::min(selected, total - 1U);
    std::size_t first = std::min(current_first, max_first);
    if (clamped_selected < first) {
        first = clamped_selected;
    } else if (clamped_selected >= first + count) {
        first = clamped_selected + 1U - count;
    }
    if (first > max_first) first = max_first;
    return PreviewWindowBounds{first, count};
}

}  // namespace calmcar::xcp::tools