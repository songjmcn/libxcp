#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace calmcar::xcp::tools {

/**
 * @brief 保存最近的界面行，限制长期运行时的面板内存增长。
 * @details 多线程共享时，调用方必须在外部同步 Append 与 Lines 访问。
 */
class BoundedLineBuffer {
public:
    /**
     * @brief 创建有界行缓冲区。
     * @param capacity 最大保留行数；为零时不保存任何行。
     */
    explicit BoundedLineBuffer(std::size_t capacity) : capacity_(capacity) {}

    /**
     * @brief 追加一行，超限时淘汰最旧的行。
     * @param line 新显示行。
     */
    void Append(std::string line) {
        if (capacity_ == 0U) return;
        if (lines_.size() == capacity_) lines_.erase(lines_.begin());
        lines_.push_back(std::move(line));
    }

    /**
     * @brief 返回当前保留行的只读引用。
     * @return 按时间顺序排列的行集合。
     */
    [[nodiscard]] const std::vector<std::string>& Lines() const noexcept {
        return lines_;
    }

    /**
     * @brief 返回当前保留行数。
     * @return 当前行数。
     */
    [[nodiscard]] std::size_t Size() const noexcept { return lines_.size(); }

private:
    std::size_t capacity_;           /**< 缓冲区行数上限。 */
    std::vector<std::string> lines_; /**< 从旧到新保存的有效显示行。 */
};

/**
 * @brief 按符号名保留最新观测值，避免周期更新无限追加重复行。
 * @details 多线程共享时，调用方必须在外部同步所有访问。
 */
struct ObservationSnapshot {
    std::string value;     /**< 最近成功取得的标量物理值文本。 */
    std::string status;    /**< 最新读取来源或错误状态。 */
    std::string timestamp; /**< 最近一次样本时间。 */
    bool has_value{false}; /**< 是否曾取得可显示的物理值。 */
    bool stale{false};     /**< 最近操作失败，当前值是旧值。 */
    bool stopped{false};   /**< 采样已停止；旧值仍可查看。 */
    bool is_array{false};  /**< 最近样本是否为完整数组批次。 */
    std::vector<std::string> array_values; /**< 原子发布的数组元素值。 */
};

/**
 * @brief 按符号名保留最新观测快照，供变量预览和详情弹窗查询。
 * @details 多线程共享时，调用方必须在外部同步所有访问。
 */
class LatestObservationModel {
public:
    /**
     * @brief 创建最新值模型。
     * @param capacity 最多保留的唯一符号数；为零时不保存数据。
     */
    explicit LatestObservationModel(std::size_t capacity)
        : capacity_(capacity) {}

    /**
     * @brief 新增或替换某一符号的兼容显示行。
     * @param key 符号规范名。
     * @param line 包含最新值和状态的显示文本。
     */
    void Update(std::string key, std::string line) {
        const auto index = FindIndex(key);
        if (index.has_value()) {
            lines_[*index] = std::move(line);
            samples_[*index] = {lines_[*index], "",    "",    true,
                                false,          false, false, {}};
            return;
        }
        if (!CanInsert(key)) return;
        keys_.push_back(std::move(key));
        samples_.push_back({line, "", "", true, false, false, false, {}});
        lines_.push_back(std::move(line));
    }

    /**
     * @brief 保存一次成功读取的物理值并清除旧值/停止标记。
     * @param key 符号规范名。
     * @param value 按 A2L 单位格式化的物理值。
     * @param status 来源状态，例如 DAQ、POLL 或手动读取。
     * @param timestamp 本地显示时间；可为空。
     */
    void UpdateValue(std::string key, std::string value, std::string status,
                     std::string timestamp = {}) {
        const auto index = FindIndex(key);
        ObservationSnapshot sample{std::move(value),
                                   std::move(status),
                                   std::move(timestamp),
                                   true,
                                   false,
                                   false,
                                   false,
                                   {}};
        if (index.has_value()) {
            samples_[*index] = std::move(sample);
            lines_[*index] = FormatLine(key, samples_[*index]);
            return;
        }
        if (!CanInsert(key)) return;
        keys_.push_back(std::move(key));
        lines_.push_back(FormatLine(keys_.back(), sample));
        samples_.push_back(std::move(sample));
    }

    /**
     * @brief 原子替换某一符号的完整数组样本。
     * @param key 符号规范名。
     * @param values 按 A2L 数组索引顺序格式化的全部元素值。
     * @param status 来源状态，例如 POLL。
     * @param timestamp 整批数组样本时间；可为空。
     */
    void UpdateArrayValue(std::string key, std::vector<std::string> values,
                          std::string status, std::string timestamp = {}) {
        const auto index = FindIndex(key);
        ObservationSnapshot sample;
        sample.status = std::move(status);
        sample.timestamp = std::move(timestamp);
        sample.has_value = true;
        sample.is_array = true;
        sample.array_values = std::move(values);
        if (index.has_value()) {
            samples_[*index] = std::move(sample);
            lines_[*index] = FormatLine(key, samples_[*index]);
            return;
        }
        if (!CanInsert(key)) return;
        keys_.push_back(std::move(key));
        lines_.push_back(FormatLine(keys_.back(), sample));
        samples_.push_back(std::move(sample));
    }

    /**
     * @brief 标记最近读取失败；保留上次值并明确标为旧值。
     * @param key 符号规范名。
     * @param status 失败原因或状态。
     * @param timestamp 失败发生时间；可为空。
     */
    void MarkFailure(std::string key, std::string status,
                     std::string timestamp = {}) {
        const auto index = FindIndex(key);
        ObservationSnapshot sample;
        if (index.has_value()) sample = samples_[*index];
        sample.status = std::move(status);
        sample.timestamp = std::move(timestamp);
        sample.stale = sample.has_value;
        sample.stopped = false;
        if (index.has_value()) {
            samples_[*index] = std::move(sample);
            lines_[*index] = FormatLine(key, samples_[*index]);
            return;
        }
        if (!CanInsert(key)) return;
        keys_.push_back(std::move(key));
        lines_.push_back(FormatLine(keys_.back(), sample));
        samples_.push_back(std::move(sample));
    }

    /**
     * @brief 标记变量已加入采样且等待新样本，保留已有物理值。
     * @param key 符号规范名。
     */
    void MarkPending(std::string key) {
        const auto index = FindIndex(key);
        ObservationSnapshot sample;
        if (index.has_value()) sample = samples_[*index];
        sample.status = "等待新样本";
        sample.stale = sample.has_value;
        sample.stopped = false;
        if (index.has_value()) {
            samples_[*index] = std::move(sample);
            lines_[*index] = FormatLine(key, samples_[*index]);
            return;
        }
        if (!CanInsert(key)) return;
        keys_.push_back(std::move(key));
        lines_.push_back(FormatLine(keys_.back(), sample));
        samples_.push_back(std::move(sample));
    }

    /**
     * @brief 移除指定符号的观测行。
     * @param key 符号规范名。
     */
    void Remove(std::string_view key) {
        const auto found = std::find_if(
            keys_.begin(), keys_.end(),
            [key](const std::string& current) { return current == key; });
        if (found == keys_.end()) return;
        const auto index = static_cast<std::size_t>(found - keys_.begin());
        keys_.erase(found);
        samples_.erase(samples_.begin() + static_cast<std::ptrdiff_t>(index));
        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    /**
     * @brief 将单个样本标记为停止，不清除最后物理值。
     * @param key 符号规范名。
     */
    void MarkStopped(std::string_view key) {
        const auto index = FindIndex(key);
        if (!index.has_value()) return;
        samples_[*index].stopped = true;
        lines_[*index] = FormatLine(keys_[*index], samples_[*index]);
    }

    /** @brief 将全部样本标记为停止，不清除最后物理值。 */
    void MarkAllStopped() {
        for (std::size_t index = 0; index < samples_.size(); ++index) {
            samples_[index].stopped = true;
            lines_[index] = FormatLine(keys_[index], samples_[index]);
        }
    }

    /**
     * @brief 查找某个符号的最新快照。
     * @param key 符号规范名。
     * @return 样本副本；从未见过时为空。
     */
    [[nodiscard]] std::optional<ObservationSnapshot> Find(
        std::string_view key) const {
        const auto found = std::find_if(
            keys_.begin(), keys_.end(),
            [key](const std::string& current) { return current == key; });
        if (found == keys_.end()) return std::nullopt;
        return samples_[static_cast<std::size_t>(found - keys_.begin())];
    }

    /**
     * @brief 返回按首次加入顺序排列的最新观测行。
     * @return 当前显示行集合的只读引用。
     */
    [[nodiscard]] const std::vector<std::string>& Lines() const noexcept {
        return lines_;
    }

    /** @brief 返回当前唯一观测变量数量。 */
    [[nodiscard]] std::size_t Size() const noexcept { return lines_.size(); }

private:
    /** @brief 查找键对应索引；未找到时返回空。 */
    [[nodiscard]] std::optional<std::size_t> FindIndex(
        std::string_view key) const {
        const auto found = std::find_if(
            keys_.begin(), keys_.end(),
            [key](const std::string& current) { return current == key; });
        if (found == keys_.end()) return std::nullopt;
        return static_cast<std::size_t>(found - keys_.begin());
    }

    /** @brief 判断能否插入新键；超限时同步淘汰最旧样本。 */
    bool CanInsert(const std::string& key) {
        if (capacity_ == 0U || key.empty()) return false;
        if (keys_.size() == capacity_) {
            keys_.erase(keys_.begin());
            samples_.erase(samples_.begin());
            lines_.erase(lines_.begin());
        }
        return true;
    }

    /** @brief 统一生成包含时间、状态、旧值和停止标记的显示行。 */
    [[nodiscard]] static std::string FormatLine(
        const std::string& key, const ObservationSnapshot& sample) {
        std::string line;
        if (!sample.timestamp.empty()) line = sample.timestamp + " ";
        line += key;
        if (sample.has_value && sample.is_array) {
            line += " = [" + std::to_string(sample.array_values.size()) +
                    " elements]";
        } else if (sample.has_value) {
            line += " = " + sample.value;
        } else {
            line += " | " + (sample.status.empty() ? "未读取" : sample.status);
        }
        if (sample.has_value && !sample.status.empty())
            line += " | " + sample.status;
        if (sample.stale && sample.has_value) line += "（旧值）";
        if (sample.stopped) line += " | 已停止";
        return line;
    }

    std::size_t capacity_;                     /**< 唯一符号数量上限。 */
    std::vector<std::string> keys_;            /**< 样本对应的规范名。 */
    std::vector<ObservationSnapshot> samples_; /**< 保留的物理值和状态。 */
    std::vector<std::string> lines_;           /**< 与样本同步的显示行。 */
};

/** @brief 预览菜单中的父符号或子值行。 */
struct PreviewRow {
    std::string symbol_name;                    /**< 所属 A2L 符号规范名。 */
    std::optional<std::uint64_t> element_index; /**< 子元素0基索引。 */
    bool is_child{false};                       /**< 是否为展开的子行。 */
    bool is_read_action{false}; /**< 是否为只读的一次性读取动作。 */
    bool is_placeholder{false}; /**< 是否为尚无样本的提示行。 */
};

/** @brief 创建一个符号的行布局所需信息。 */
struct PreviewRowSource {
    std::string symbol_name;           /**< A2L 符号规范名。 */
    bool expanded{false};              /**< 是否展开。 */
    bool has_sample{false};            /**< 是否已有完整成功样本。 */
    bool is_array{false};              /**< 样本是否为数组。 */
    std::size_t value_count{0U};       /**< 当前数组样本的元素数。 */
    bool has_dimensions{false};        /**< 符号元数据是否声明维度。 */
    bool manual_read_available{false}; /**< 是否可在停止采样时读取一次。 */
};

/**
 * @brief 为展开符号生成稳定父/子行映射，便于菜单滚动和测试。
 * @param sources 按 A2L 排序的符号显示状态。
 * @return 与菜单标签一一对应的父行和子值行。
 */
inline std::vector<PreviewRow> BuildPreviewRows(
    const std::vector<PreviewRowSource>& sources) {
    std::vector<PreviewRow> rows;
    for (const auto& source : sources) {
        rows.push_back({source.symbol_name, std::nullopt, false, false, false});
        if (!source.expanded) continue;
        if (source.manual_read_available)
            rows.push_back(
                {source.symbol_name, std::nullopt, true, true, false});
        if (source.has_sample && source.is_array && source.value_count > 0U) {
            for (std::size_t index = 0; index < source.value_count; ++index)
                rows.push_back({source.symbol_name,
                                static_cast<std::uint64_t>(index), true, false,
                                false});
        } else if (source.has_sample && !source.is_array) {
            rows.push_back(
                {source.symbol_name, std::nullopt, true, false, false});
        } else {
            rows.push_back(
                {source.symbol_name, std::nullopt, true, false, true});
        }
    }
    return rows;
}

/** @brief 预览行激活后的界面动作。 */
enum class PreviewRowAction { ToggleExpansion, ReadOnce, Ignore };

/**
 * @brief 根据预览行类型选择安全的 Enter 动作。
 * @param row 当前菜单行映射。
 * @return 父行展开、读取动作，或对子行/占位行无操作。
 */
inline PreviewRowAction ActionForPreviewRow(const PreviewRow& row) {
    if (row.is_read_action) return PreviewRowAction::ReadOnce;
    if (row.is_child || row.is_placeholder) return PreviewRowAction::Ignore;
    return PreviewRowAction::ToggleExpansion;
}

/**
 * @brief 为异步搜索输入提供轻量代际，阻止旧结果覆盖较新的编辑草稿。
 * @details 该模型只由 UI 线程读写；后台搜索将捕获值带回 UI 线程比较。
 */
class SearchInputRevision {
public:
    /** @brief 用户编辑搜索输入时递增代际。 */
    void MarkEdited() { ++revision_; }

    /** @brief 获取提交搜索时应随请求保存的代际。 */
    [[nodiscard]] std::uint64_t Capture() const { return revision_; }

    /** @brief 判断完成的搜索是否仍对应当前输入草稿。 */
    [[nodiscard]] bool Matches(std::uint64_t revision) const {
        return revision_ == revision;
    }

private:
    std::uint64_t revision_{0}; /**< 搜索输入编辑代际。 */
};

/**
 * @brief 计算循环切换视图后的菜单索引。
 * @param selected 当前索引。
 * @param view_count 视图数量。
 * @return 下一个视图索引；数量为零时返回零。
 */
inline int NextViewIndex(int selected, std::size_t view_count) noexcept {
    if (view_count == 0U || selected < 0) return 0;
    const auto current = static_cast<std::size_t>(selected) % view_count;
    return static_cast<int>((current + 1U) % view_count);
}

/** @brief 预览模式下可通过 Tab 到达的三处焦点。 */
enum class PreviewFocusTarget { Search, Scope, List };

/**
 * @brief 计算预览区 Tab/Shift+Tab 的循环目标，不跨入命令框。
 * @param current 当前预览焦点。
 * @param reverse 是否为 Shift+Tab。
 * @return 下一个预览焦点。
 */
inline PreviewFocusTarget NextPreviewFocusTarget(PreviewFocusTarget current,
                                                 bool reverse) noexcept {
    const int index = static_cast<int>(current);
    const int offset = reverse ? 2 : 1;
    return static_cast<PreviewFocusTarget>((index + offset) % 3);
}

/**
 * @brief 管理预览/命令输入模式，并约束列表操作的入口。
 * @details 仅由 UI 线程访问；选择器地址提供给 FTXUI Tab 容器隔离事件。
 */
class TuiInputModeState {
public:
    /** @brief 返回供 FTXUI Tab 容器使用的模式选择器。 */
    [[nodiscard]] int* Selector() noexcept { return &selector_; }

    /** @brief 当前是否允许命令输入。 */
    [[nodiscard]] bool InCommandMode() const noexcept { return selector_ == 1; }

    /** @brief 显式进入命令模式；重复进入不会执行命令。 */
    void EnterCommandMode() noexcept { selector_ = 1; }

    /**
     * @brief 退出命令模式并回到变量预览视图，不触碰输入草稿。
     * @param selected_view 当前顶部视图索引。
     */
    void LeaveCommandMode(int& selected_view) noexcept {
        selector_ = 0;
        selected_view = 0;
    }

    /**
     * @brief 列表快捷键是否有资格分派；实际符号权限由业务层继续检查。
     * @param selected_view 当前顶部视图索引。
     * @param list_focused 列表是否为实际焦点。
     * @return 只在预览模式、预览视图、列表聚焦时为 true。
     */
    [[nodiscard]] bool AllowsPreviewListAction(
        int selected_view, bool list_focused) const noexcept {
        return !InCommandMode() && selected_view == 0 && list_focused;
    }

private:
    int selector_{0}; /**< 0=预览模式，1=命令模式，启动默认预览。 */
};

/**
 * @brief 管理命令输入历史及历史浏览期间的草稿恢复。
 * @details 仅由 UI 线程访问，不提供内部同步。
 */
class CommandHistory {
public:
    /**
     * @brief 创建有界命令历史。
     * @param capacity 最多保存的命令数；为零时禁用历史。
     */
    explicit CommandHistory(std::size_t capacity) : capacity_(capacity) {}

    /**
     * @brief 记录一条非空命令并结束当前历史浏览。
     * @param command 已提交的命令文本。
     */
    void Add(std::string command) {
        if (!command.empty() && capacity_ > 0U) {
            commands_.push_back(std::move(command));
            if (commands_.size() > capacity_)
                commands_.erase(commands_.begin());
        }
        cursor_ = commands_.size();
        draft_.clear();
    }

    /**
     * @brief 仅在命令成功入队后记录历史并清空输入草稿。
     * @param draft 输入框当前文本。
     * @param accepted 命令队列是否接受本次提交。
     * @return 已提交并清空草稿时返回 true；拒绝时原样保留。
     */
    [[nodiscard]] bool Commit(std::string& draft, bool accepted) {
        if (!accepted) return false;
        Add(draft);
        draft.clear();
        return true;
    }

    /**
     * @brief 返回上一条命令；首次向上时保存当前编辑草稿。
     * @param current 当前输入框文本。
     * @return 历史命令；历史为空时返回空。
     */
    [[nodiscard]] std::optional<std::string> Previous(
        const std::string& current) {
        if (commands_.empty()) return std::nullopt;
        if (cursor_ == commands_.size()) draft_ = current;
        if (cursor_ > 0U) --cursor_;
        return commands_[cursor_];
    }

    /**
     * @brief 返回下一条命令或恢复开始浏览时保存的草稿。
     * @return 下一条命令/草稿；当前不在历史浏览时返回空。
     */
    [[nodiscard]] std::optional<std::string> Next() {
        if (cursor_ >= commands_.size()) return std::nullopt;
        ++cursor_;
        if (cursor_ == commands_.size()) return draft_;
        return commands_[cursor_];
    }

private:
    std::size_t capacity_;              /**< 历史命令数量上限。 */
    std::vector<std::string> commands_; /**< 从旧到新保存的命令。 */
    std::size_t cursor_{0};             /**< 当前历史位置；末尾表示编辑草稿。 */
    std::string draft_;                 /**< 开始历史浏览时保存的输入草稿。 */
};

/** @brief 按视图维护独立的显示窗口，历史浏览不因追加行自动跳回底部。 */
class PanelViewport {
public:
    /** @brief 默认面板页高；运行时会按终端尺寸更新。 */
    static constexpr std::size_t kPageSize = 18;

    /**
     * @brief 创建指定数量的视图窗口。
     * @param views 独立滚动视图数量。
     * @param page_size 初始每页行数。
     */
    explicit PanelViewport(std::size_t views, std::size_t page_size = kPageSize)
        : positions_(views), page_size_(std::max<std::size_t>(1U, page_size)) {}

    /** @brief 更新每页行数并夹紧所有滚动位置。 */
    void SetPageSize(std::size_t page_size) {
        page_size_ = std::max<std::size_t>(1U, page_size);
        for (auto& position : positions_)
            if (!position.follow)
                position.first = std::min(position.first, page_size_);
    }

    /** @brief 返回当前每页行数。 */
    [[nodiscard]] std::size_t PageSize() const noexcept { return page_size_; }

    /** @brief 获取窗口起点，截断被缩短的结果集合。 */
    [[nodiscard]] std::size_t Start(std::size_t view, std::size_t total) const {
        if (view >= positions_.size()) return 0;
        const auto maximum = total > page_size_ ? total - page_size_ : 0U;
        return positions_[view].follow
                   ? maximum
                   : std::min(positions_[view].first, maximum);
    }

    /** @brief 向更旧的行移动一屏。 */
    void PageUp(std::size_t view, std::size_t total) {
        if (view >= positions_.size()) return;
        const auto start = Start(view, total);
        positions_[view] = {start > page_size_ ? start - page_size_ : 0U,
                            false};
    }

    /** @brief 向更新的行移动一屏，到达尾部后重新跟随新增行。 */
    void PageDown(std::size_t view, std::size_t total) {
        if (view >= positions_.size()) return;
        const auto maximum = total > page_size_ ? total - page_size_ : 0U;
        const auto start = Start(view, total);
        const auto next = std::min(maximum, start + page_size_);
        positions_[view] = {next, next == maximum};
    }

    /** @brief 新结果快照从第一行开始浏览。 */
    void ResetToTop(std::size_t view) {
        if (view < positions_.size()) positions_[view] = {0U, false};
    }

    /** @brief 返回普通结果面板时恢复跟随其最新行。 */
    void FollowBottom(std::size_t view) {
        if (view < positions_.size()) positions_[view] = {};
    }

    /** @brief 有界行缓冲淘汰最旧一行时保持历史窗口可见位置。 */
    void EvictFront(std::size_t view) {
        if (view < positions_.size() && !positions_[view].follow &&
            positions_[view].first > 0U)
            --positions_[view].first;
    }

private:
    /** @brief 每个视图独立的浏览位置和是否跟随末尾。 */
    struct Position {
        std::size_t first{0}; /**< 历史窗口首行索引。 */
        bool follow{true};    /**< 当前窗口跟随最新行。 */
    };
    std::vector<Position> positions_; /**< 各视图的滚动位置。 */
    std::size_t page_size_;           /**< 当前终端尺寸对应的页高。 */
};

/** @brief 当前终端尺寸对应的固定层/弹窗尺寸规划。 */
struct TuiLayoutMetrics {
    static constexpr int kMinimumWidth = 80;  /**< 可用终端最小列数。 */
    static constexpr int kMinimumHeight = 24; /**< 可用终端最小行数。 */

    int width{};                  /**< 终端列数。 */
    int height{};                 /**< 终端行数。 */
    std::size_t panel_rows{1U};   /**< 结果/日志主面板页高。 */
    std::size_t preview_rows{1U}; /**< 变量预览表格页高。 */
    int modal_width{1};           /**< 弹窗最大宽度。 */
    int modal_height{1};          /**< 弹窗最大高度。 */
    bool compact{true};           /**< 是否采用窄屏单行样式。 */
    bool too_small{true};         /**< 是否低于最低可用尺寸。 */
};

/**
 * @brief 按窗口尺寸计算页高、响应式模式和弹窗边界。
 * @param width 当前终端列数。
 * @param height 当前终端行数。
 * @return 所有行列值均为正数的布局快照。
 */
[[nodiscard]] inline TuiLayoutMetrics CalculateTuiLayout(int width,
                                                         int height) noexcept {
    TuiLayoutMetrics layout;
    layout.width = std::max(0, width);
    layout.height = std::max(0, height);
    layout.compact = layout.width < 100;
    layout.too_small = layout.width < TuiLayoutMetrics::kMinimumWidth ||
                       layout.height < TuiLayoutMetrics::kMinimumHeight;

    // 双行状态、页签、分隔线、命令栏和提示栏占 7 行，外框占 2 行；
    // 结果/日志再留一行显示分页位置，预览区则留搜索与状态两行。
    layout.panel_rows =
        static_cast<std::size_t>(std::max(1, layout.height - 10));
    layout.preview_rows =
        static_cast<std::size_t>(std::max(1, layout.height - 11));
    layout.modal_width = std::max(1, std::min(88, layout.width - 8));
    layout.modal_height = std::max(1, std::min(20, layout.height - 6));
    return layout;
}

/** @brief 变量详情弹窗的当前交互页面。 */
enum class VariableModalPage { Closed, Details, Calibration, Confirmation };

/** @brief 管理变量详情弹窗、写入确认和关闭时的稳定选择。 */
class VariableModalState {
public:
    /**
     * @brief 打开一个变量的详情页面。
     * @param qualified_name A2L 规范名称。
     * @param layout 当前尺寸规划。
     * @return 尺寸可用且名称非空时成功。
     */
    bool Open(std::string qualified_name, const TuiLayoutMetrics& layout) {
        if (layout.too_small || qualified_name.empty()) return false;
        selected_name_ = std::move(qualified_name);
        page_ = VariableModalPage::Details;
        return true;
    }

    /**
     * @brief 打开与只读详情分离的标定表单。
     * @param qualified_name A2L 规范名称。
     * @param layout 当前尺寸规划。
     * @return 尺寸可用且名称非空时成功。
     */
    bool OpenCalibration(std::string qualified_name,
                         const TuiLayoutMetrics& layout) {
        if (layout.too_small || qualified_name.empty()) return false;
        selected_name_ = std::move(qualified_name);
        page_ = VariableModalPage::Calibration;
        return true;
    }

    /**
     * @brief 打开危险操作确认页面；只可从标定页对可写项调用。
     * @param write_supported 当前符号是否满足写入资格。
     * @param layout 当前尺寸规划。
     * @return 允许时进入确认页面。
     */
    bool BeginConfirmation(bool write_supported,
                           const TuiLayoutMetrics& layout) {
        if (page_ != VariableModalPage::Calibration || !write_supported ||
            layout.too_small)
            return false;
        confirmation_return_page_ = VariableModalPage::Calibration;
        page_ = VariableModalPage::Confirmation;
        return true;
    }

    /**
     * @brief 从普通命令打开通用危险操作确认页面。
     * @param layout 当前尺寸规划。
     * @return 尺寸可用时进入确认页面。
     */
    bool OpenCommandConfirmation(const TuiLayoutMetrics& layout) {
        if (page_ != VariableModalPage::Closed || layout.too_small)
            return false;
        selected_name_.clear();
        confirmation_return_page_ = VariableModalPage::Closed;
        page_ = VariableModalPage::Confirmation;
        return true;
    }

    /** @brief 取消确认后返回发起确认的标定页或关闭通用确认弹窗。 */
    void CancelConfirmation() {
        if (page_ != VariableModalPage::Confirmation) return;
        page_ = confirmation_return_page_;
        if (page_ == VariableModalPage::Closed) selected_name_.clear();
    }

    /** @brief 确认请求结束后返回标定页或关闭通用确认弹窗。 */
    void CompleteConfirmation() { CancelConfirmation(); }

    /** @brief 关闭弹窗；列表过滤、行选择和滚动由调用方保留。 */
    void Close() {
        page_ = VariableModalPage::Closed;
        confirmation_return_page_ = VariableModalPage::Closed;
        selected_name_.clear();
    }

    /** @brief 弹窗是否显示。 */
    [[nodiscard]] bool IsOpen() const noexcept {
        return page_ != VariableModalPage::Closed;
    }

    /** @brief 弹窗打开时锁定背景变量列表，延迟异步搜索结果应用。 */
    [[nodiscard]] bool BlocksBackgroundListUpdates() const noexcept {
        return IsOpen();
    }

    /** @brief 弹窗当前页面。 */
    [[nodiscard]] VariableModalPage Page() const noexcept { return page_; }

    /** @brief 弹窗绑定的符号规范名。 */
    [[nodiscard]] const std::string& SelectedName() const noexcept {
        return selected_name_;
    }

private:
    VariableModalPage page_{VariableModalPage::Closed}; /**< 当前弹窗页。 */
    VariableModalPage confirmation_return_page_{
        VariableModalPage::Closed}; /**< 确认完成后恢复的来源页。 */
    std::string selected_name_;     /**< 打开时快照，关闭时清空。 */
};

/** @brief 将异步标定结果绑定到具体选择，防止旧结果污染新表单。 */
class CalibrationSelection {
public:
    /** @brief 选择快照，供后台读写命令和结果回填使用。 */
    struct Snapshot {
        std::string name;         /**< 规范符号名。 */
        std::uint64_t revision{}; /**< 选择变化代际。 */
    };

    /** @brief 选中另一变量或清空选择；只有变化才递增代际。 */
    [[nodiscard]] bool Select(std::string name) {
        if (name == name_) return false;
        name_ = std::move(name);
        ++revision_;
        return true;
    }

    /** @brief 当前变量及代际的不可变快照。 */
    [[nodiscard]] Snapshot Current() const { return {name_, revision_}; }

    /** @brief 判断异步命令的快照仍与当前选择一致。 */
    [[nodiscard]] bool Matches(const Snapshot& snapshot) const {
        return !snapshot.name.empty() && snapshot.name == name_ &&
               snapshot.revision == revision_;
    }

private:
    std::string name_;         /**< 当前规范符号名。 */
    std::uint64_t revision_{}; /**< 防止 A→B→A 的旧回填的代际。 */
};

/** @brief 标定表单的选择、草稿和值都绑定同一代际。 */
class CalibrationFormModel {
public:
    std::string target;                           /**< 当前目标物理值草稿。 */
    std::string current{"尚未读取"};              /**< 当前选择的最近回读值。 */
    std::string status{"请选择变量并读取当前值"}; /**< 当前选择的提示。 */

    /** @brief 切换选择时清除旧目标、回读和值状态；搜索更新可强制重置。 */
    [[nodiscard]] bool Select(std::string name, bool force = false) {
        if (force) static_cast<void>(selection_.Select({}));
        if (!selection_.Select(name) && !force) return false;
        target.clear();
        current = "尚未读取";
        status = name.empty() ? "没有匹配项" : "请选择变量并读取当前值";
        return true;
    }

    /** @brief 获取当前选择的不可变快照。 */
    [[nodiscard]] CalibrationSelection::Snapshot Snapshot() const {
        return selection_.Current();
    }

    /** @brief 判断异步命令的快照仍属于当前表单。 */
    [[nodiscard]] bool Matches(
        const CalibrationSelection::Snapshot& snapshot) const {
        return selection_.Matches(snapshot);
    }

    /** @brief 只应用属于当前选择代际的异步结果。 */
    [[nodiscard]] bool Apply(
        const CalibrationSelection::Snapshot& snapshot,
        const std::string& new_status,
        std::optional<std::string> new_current = std::nullopt) {
        if (!Matches(snapshot)) return false;
        if (new_current) current = std::move(*new_current);
        status = new_status;
        return true;
    }

private:
    CalibrationSelection selection_; /**< 当前选择代际。 */
};

/**
 * @brief 在线程间传递单次危险操作的确认结果。
 * @details 每次 Begin 创建新的待确认周期；无明确接受结果或退出时默认取消。
 */
class ConfirmationGate {
public:
    /** @brief 单次确认的标识，旧标识不能批准新操作。 */
    using Ticket = std::uint64_t;

    /**
     * @brief 开始新的确认周期，使旧周期的许可失效。
     * @return 本次操作的唯一标识。
     */
    [[nodiscard]] Ticket Begin() {
        Ticket ticket;
        {
            std::lock_guard lock(mutex_);
            ticket = ++ticket_;
            answer_.reset();
            consumed_ = false;
            pending_ = true;
        }
        condition_.notify_all();  // 旧票据等待者立即结束，而不是等新周期回答。
        return ticket;
    }

    /**
     * @brief 等待本次选择；退出或取消优先于已收到的确认。
     * @param ticket 本次操作标识。
     * @param stopping 工具退出标志。
     * @return 仅本次操作被明确接受且没有停止请求时返回 true。
     */
    [[nodiscard]] bool Wait(Ticket ticket, const std::atomic<bool>& stopping) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this, ticket, &stopping] {
            return ticket != ticket_ || answer_.has_value() || stopping.load();
        });
        if (ticket != ticket_) return false;
        pending_ = false;
        return !stopping.load() && answer_.value_or(false);
    }

    /**
     * @brief 提交本次明确选择；首次回答有效，取消允许撤销尚未执行的许可。
     * @param ticket 本次操作标识。
     * @param accepted true 表示接受，false 表示取消。
     */
    void Resolve(Ticket ticket, bool accepted) {
        {
            std::lock_guard lock(mutex_);
            if (ticket != ticket_ || consumed_) return;
            if (accepted) {
                if (!pending_ || answer_.has_value()) return;
                answer_ = true;
            } else {
                // 取消可撤销 Wait 已返回但尚未 Dispatch
                // 的许可；接受后不再能二次确认。
                answer_ = false;
                pending_ = false;
            }
        }
        condition_.notify_all();
    }

    /** @brief 无条件撤销尚未Dispatch的当前许可。 */
    void Cancel() {
        {
            std::lock_guard lock(mutex_);
            if (consumed_) return;
            answer_ = false;
            pending_ = false;
        }
        condition_.notify_all();
    }

    /**
     * @brief 原子取消尚未进入Dispatch的操作。
     * @return 若取消赢得Dispatch边界且确实撤销了许可则返回true。
     */
    [[nodiscard]] bool CancelBeforeDispatch() {
        {
            std::lock_guard lock(mutex_);
            if (consumed_ || (!pending_ && answer_ != true)) return false;
            answer_ = false;
            pending_ = false;
        }
        condition_.notify_all();
        return true;
    }

    /**
     * @brief 在发送危险命令前再次检查本次许可和停止状态。
     * @param ticket 本次操作标识。
     * @param stopping 工具退出标志。
     * @return 尚未撤销的明确许可。
     */
    [[nodiscard]] bool Authorized(Ticket ticket,
                                  const std::atomic<bool>& stopping) const {
        std::lock_guard lock(mutex_);
        return ticket == ticket_ && !consumed_ && !stopping.load() &&
               answer_ == true;
    }

    /**
     * @brief 原子消费确认许可，再在锁外调用同步发送函数。
     * @param ticket 本次操作标识。
     * @param stopping 停止或退出标志。
     * @param send 实际危险操作；许可被消费后，后续取消无法撤回。
     * @return 是否成功消费许可并进入发送函数。
     */
    template <typename Send>
    [[nodiscard]] bool Dispatch(Ticket ticket,
                                const std::atomic<bool>& stopping,
                                Send&& send) {
        {
            std::lock_guard lock(mutex_);
            if (ticket != ticket_ || consumed_ || stopping.load() ||
                answer_ != true)
                return false;
            // 消费许可是线性化点；之后的取消不会阻塞并视为已进入下发。
            consumed_ = true;
        }
        std::forward<Send>(send)();
        return true;
    }

    /**
     * @brief 查询确认周期是否仍处于待决状态。
     * @return 待用户选择或等待命令线程收取结果时为 true。
     */
    [[nodiscard]] bool Pending() const {
        std::lock_guard lock(mutex_);
        return pending_;
    }

    /** @brief 返回当前周期标识，供 UI 事件绑定本次回答。 */
    [[nodiscard]] Ticket CurrentTicket() const {
        std::lock_guard lock(mutex_);
        return ticket_;
    }

private:
    mutable std::mutex mutex_;          /**< 确认周期状态互斥量。 */
    std::condition_variable condition_; /**< 确认结果等待通知。 */
    std::optional<bool> answer_; /**< 当前周期的用户选择；取消不可变回确认。 */
    Ticket ticket_{0};           /**< 单调递增的周期标识。 */
    bool pending_{false};        /**< 尚待命令线程收取的确认周期。 */
    bool consumed_{false};       /**< 危险操作许可已被领取。 */
};

/**
 * @brief 仅在本次确认仍有效时调用危险操作发送者。
 * @param gate 确认状态。
 * @param ticket 本次操作标识。
 * @param stopping 停止或退出标志。
 * @param send 实际危险命令发送函数。
 * @return 是否进入了危险操作发送函数；请求已在途时不能撤销。
 */
template <typename Send>
[[nodiscard]] bool SendIfConfirmed(ConfirmationGate& gate,
                                   ConfirmationGate::Ticket ticket,
                                   const std::atomic<bool>& stopping,
                                   Send&& send) {
    return gate.Dispatch(ticket, stopping, std::forward<Send>(send));
}

/**
 * @brief 将确认键转换为确认/取消结果。
 * @param key 单个确认输入字符；不认识的字符不会给出结果。
 * @return true 表示确认，false 表示取消，空值表示忽略输入。
 */
[[nodiscard]] inline std::optional<bool> ConfirmationDecision(
    std::string_view key) noexcept {
    if (key == "y" || key == "Y") return true;
    if (key == "n" || key == "N") return false;
    return std::nullopt;
}

}  // namespace calmcar::xcp::tools
