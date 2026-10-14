#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
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
class LatestObservationModel {
public:
    /**
     * @brief 创建最新值模型。
     * @param capacity 最多保留的唯一符号数；为零时不保存数据。
     */
    explicit LatestObservationModel(std::size_t capacity)
        : capacity_(capacity) {}

    /**
     * @brief 新增或替换某一符号的显示行。
     * @param key 符号规范名。
     * @param line 包含最新值和状态的显示文本。
     */
    void Update(std::string key, std::string line) {
        const auto found = std::find(keys_.begin(), keys_.end(), key);
        if (found != keys_.end()) {
            lines_[static_cast<std::size_t>(found - keys_.begin())] =
                std::move(line);
            return;
        }
        if (capacity_ == 0U || key.empty()) return;
        if (keys_.size() == capacity_) {
            keys_.erase(keys_.begin());
            lines_.erase(lines_.begin());
        }
        keys_.push_back(std::move(key));
        lines_.push_back(std::move(line));
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
        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    /** @brief 将现有行标记为采集已停止，不清除最后观测值。 */
    void MarkAllStopped() {
        constexpr std::string_view kStoppedSuffix = " | 已停止";
        for (auto& line : lines_) {
            if (!line.ends_with(kStoppedSuffix)) line += kStoppedSuffix;
        }
    }

    /**
     * @brief 返回按首次加入顺序排列的最新观测行。
     * @return 当前显示行集合的只读引用。
     */
    [[nodiscard]] const std::vector<std::string>& Lines() const noexcept {
        return lines_;
    }

    /**
     * @brief 返回当前唯一观测变量数量。
     * @return 当前行数。
     */
    [[nodiscard]] std::size_t Size() const noexcept { return lines_.size(); }

private:
    std::size_t capacity_;           /**< 唯一符号数量上限。 */
    std::vector<std::string> keys_;  /**< 与显示行一一对应的规范名。 */
    std::vector<std::string> lines_; /**< 每个符号最新的状态和值。 */
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

/**
 * @brief 在线程间传递单次危险操作的确认结果。
 * @details 每次 Begin 创建新的待确认周期；无明确接受结果或退出时默认取消。
 */
class ConfirmationGate {
public:
    /** @brief 开始新的待确认周期并清除先前结果。 */
    void Begin() {
        std::lock_guard lock(mutex_);
        answer_.reset();
        pending_ = true;
    }

    /**
     * @brief 等待用户结果，退出请求可打断等待并按取消处理。
     * @param stopping 工具退出标志。
     * @return 仅明确接受且未被退出打断时返回 true。
     */
    [[nodiscard]] bool Wait(const std::atomic<bool>& stopping) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this, &stopping] {
            return answer_.has_value() || stopping.load();
        });
        pending_ = false;
        return answer_.value_or(false);
    }

    /**
     * @brief 提交当前周期的明确选择。
     * @param accepted true 表示接受，false 表示取消。
     */
    void Resolve(bool accepted) {
        {
            std::lock_guard lock(mutex_);
            if (!pending_) return;
            answer_ = accepted;
        }
        condition_.notify_all();
    }

    /** @brief 取消当前待确认周期。 */
    void Cancel() { Resolve(false); }

    /**
     * @brief 查询确认周期是否仍处于待决状态。
     * @return 待用户选择或等待命令线程收取结果时为 true。
     */
    [[nodiscard]] bool Pending() const {
        std::lock_guard lock(mutex_);
        return pending_;
    }

private:
    mutable std::mutex mutex_;          /**< 确认周期状态互斥量。 */
    std::condition_variable condition_; /**< 确认结果等待通知。 */
    std::optional<bool> answer_;        /**< 当前周期的明确用户选择。 */
    bool pending_{false}; /**< 当前是否存在未收取结果的确认周期。 */
};

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
