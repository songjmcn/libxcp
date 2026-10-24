/**
 * @file windowed_menu.hpp
 * @brief 只构造可视行的虚拟菜单，替代 FTXUI Menu 以支撑 198k 行规模。
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "preview_window.hpp"

namespace calmcar::xcp::tools {

/**
 * @brief 虚拟菜单的外部回调。
 */
struct WindowedMenuOptions {
    /// 回车激活当前行时的回调（对应原 MenuOption::on_enter）。
    std::function<void()> on_enter;
};

/**
 * @brief 只构造可视行的虚拟菜单。
 * @details 行为对齐 FTXUI `Menu`（上下移动、翻页、Home/End、Tab 选中移动、
 *          回车激活、滚轮 3 行、左键点击选中），但渲染复杂度为
 *          O(可视行数)，与候选总数无关。
 */
class WindowedMenu final : public ftxui::ComponentBase {
public:
    /**
     * @brief 构造虚拟菜单。
     * @param entries 行文本容器，生命周期由调用方保证。
     * @param selected 选中行索引（0 基），生命周期由调用方保证。
     * @param options 外部回调。
     */
    WindowedMenu(const std::vector<std::string>* entries, int* selected,
                 WindowedMenuOptions options)
        : entries_(entries), selected_(selected), options_(std::move(options)) {}

    /**
     * @brief 设置本帧可视行数（由布局计算后每帧刷新）。
     * @param rows 可视行数，0 按 1 处理。
     */
    void SetVisibleRows(std::size_t rows) noexcept {
        visible_rows_ = std::max<std::size_t>(1U, rows);
    }

    /**
     * @brief 上一帧实际构造的行数（测试用）。
     * @return 上一帧构造的行数。
     */
    [[nodiscard]] std::size_t LastRenderedRows() const noexcept {
        return last_rendered_rows_;
    }

    /**
     * @brief 当前窗口首行（测试与状态栏用）。
     * @return 窗口首行（0 基）。
     */
    [[nodiscard]] std::size_t WindowFirstRow() const noexcept {
        return window_first_;
    }

    /// 虚拟菜单始终可以获得焦点（与 Menu 一致）。
    bool Focusable() const override { return true; }

    /**
     * @brief 只在窗口内构造行元素。
     * @return 本帧的菜单元素。
     */
    ftxui::Element OnRender() override {
        const std::size_t total = Count();
        const int selected = ClampedSelected();
        // 与 Menu::Clamp 一致：把越界的选中行写回调用方，避免过滤后
        // 选中行停留在不存在的行上（ActivatePreviewRow 会直接索引）。
        if (selected_ != nullptr && *selected_ != selected) WriteSelected(selected);
        const auto bounds = ComputePreviewWindow(
            total, static_cast<std::size_t>(selected), visible_rows_,
            window_first_);
        window_first_ = bounds.first;
        last_rendered_rows_ = bounds.count;
        row_boxes_.assign(bounds.count, ftxui::Box{});
        if (bounds.count == 0U) return ftxui::text("") | ftxui::reflect(box_);

        const bool is_focused = Focused();
        ftxui::Elements elements;
        elements.reserve(bounds.count);
        for (std::size_t offset = 0; offset < bounds.count; ++offset) {
            const std::size_t row = bounds.first + offset;
            ftxui::Element element = ftxui::text((*entries_)[row]);
            if (static_cast<int>(row) == selected) {
                element |= ftxui::inverted;
                if (is_focused) element |= ftxui::focus;
            }
            element |= ftxui::reflect(row_boxes_[offset]);
            elements.push_back(std::move(element));
        }
        return ftxui::vbox(std::move(elements)) | ftxui::reflect(box_);
    }

    /**
     * @brief 处理键盘与鼠标事件。
     * @param event FTXUI 事件。
     * @return 事件是否被消费。
     * @note 左右方向键与 h/l 刻意不消费，交上层处理（纵向菜单无左右语义）。
     */
    bool OnEvent(ftxui::Event event) override {
        const std::size_t total = Count();
        if (total == 0U) return false;
        if (event.is_mouse()) return OnMouseEvent(std::move(event));

        const int old_selected = ClampedSelected();
        int selected = old_selected;
        const int page = static_cast<int>(visible_rows_);
        if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k'))
            selected -= 1;
        else if (event == ftxui::Event::ArrowDown ||
                 event == ftxui::Event::Character('j'))
            selected += 1;
        else if (event == ftxui::Event::PageUp)
            selected -= page;
        else if (event == ftxui::Event::PageDown)
            selected += page;
        else if (event == ftxui::Event::Home)
            selected = 0;
        else if (event == ftxui::Event::End)
            selected = static_cast<int>(total) - 1;
        else if (event == ftxui::Event::Tab)
            selected = (selected + 1) % static_cast<int>(total);
        else if (event == ftxui::Event::TabReverse)
            selected =
                (selected + static_cast<int>(total) - 1) % static_cast<int>(total);
        else if (event == ftxui::Event::Return) {
            if (options_.on_enter) options_.on_enter();
            return true;
        } else
            return false;

        selected = std::clamp(selected, 0, static_cast<int>(total) - 1);
        if (selected == old_selected) return false;
        WriteSelected(selected);
        return true;
    }

private:
    /**
     * @brief 行总数。
     * @return 行总数，容器为空指针时返回 0。
     */
    [[nodiscard]] std::size_t Count() const noexcept {
        return entries_ == nullptr ? 0U : entries_->size();
    }

    /**
     * @brief 取当前选中行并钳位到合法区间。
     * @return 合法选中行（0 基）。
     */
    [[nodiscard]] int ClampedSelected() const noexcept {
        const int total = static_cast<int>(Count());
        if (selected_ == nullptr) return 0;
        if (total <= 0) return 0;
        return std::clamp(*selected_, 0, total - 1);
    }

    /**
     * @brief 写回选中行（同时钳位）。
     * @param selected 新的选中行（0 基）。
     */
    void WriteSelected(int selected) noexcept {
        if (selected_ == nullptr) return;
        *selected_ = selected;
    }

    /**
     * @brief 处理鼠标滚轮与左键点击。
     * @param event FTXUI 事件。
     * @return 事件是否被消费。
     */
    bool OnMouseEvent(ftxui::Event event) {
        const auto& mouse = event.mouse();
        if (mouse.button == ftxui::Mouse::WheelUp ||
            mouse.button == ftxui::Mouse::WheelDown) {
            return OnMouseWheel(mouse);
        }
        if (mouse.button != ftxui::Mouse::None &&
            mouse.button != ftxui::Mouse::Left) {
            return false;
        }
        if (!CaptureMouse(event)) return false;
        for (std::size_t offset = 0; offset < row_boxes_.size(); ++offset) {
            if (!row_boxes_[offset].Contain(mouse.x, mouse.y)) continue;
            TakeFocus();
            if (mouse.button == ftxui::Mouse::Left &&
                mouse.motion == ftxui::Mouse::Pressed) {
                WriteSelected(static_cast<int>(window_first_ + offset));
                return true;
            }
            return false;
        }
        return false;
    }

    /**
     * @brief 鼠标滚轮滚动三行。
     * @param mouse 鼠标事件负载。
     * @return 事件是否被消费（指针在菜单内即消费，与 Menu 一致）。
     */
    bool OnMouseWheel(const ftxui::Mouse& mouse) {
        if (!box_.Contain(mouse.x, mouse.y)) return false;
        const int total = static_cast<int>(Count());
        if (total <= 0) return false;
        int selected = ClampedSelected();
        if (mouse.button == ftxui::Mouse::WheelUp) selected -= 3;
        if (mouse.button == ftxui::Mouse::WheelDown) selected += 3;
        selected = std::clamp(selected, 0, total - 1);
        WriteSelected(selected);
        return true;
    }

    const std::vector<std::string>* entries_{nullptr};  ///< 行文本容器。
    int* selected_{nullptr};                            ///< 选中行索引。
    WindowedMenuOptions options_{};                     ///< 外部回调。
    std::size_t visible_rows_{20U};                     ///< 本帧可视行数。
    std::size_t window_first_{0U};                      ///< 当前窗口首行。
    std::size_t last_rendered_rows_{0U};                ///< 上一帧构造行数。
    std::vector<ftxui::Box> row_boxes_{};               ///< 可视行命中区域。
    ftxui::Box box_{};                                  ///< 整个菜单区域。
};

}  // namespace calmcar::xcp::tools