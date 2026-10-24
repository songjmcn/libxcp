#ifndef CALMCAR_XCP_TOOLS_PREVIEW_REFRESH_HPP
#define CALMCAR_XCP_TOOLS_PREVIEW_REFRESH_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tui_model.hpp"

namespace calmcar::xcp::tools {

/**
 * @brief 预览行索引：符号名到该符号占据的行号集合。
 *
 * 一个符号展开后可能占据多行（父行 + 若干子值行），因此索引里保存行号
 * 向量。索引与行布局同生命周期，行布局重建时必须同步重建索引。
 */
class PreviewRowIndex {
public:
    /** @brief 清空索引，供重建行布局前调用。 */
    void Clear() noexcept { rows_by_name_.clear(); }

    /**
     * @brief 按新的行布局重建索引。
     * @param rows 行布局，顺序与标签数组一一对应。
     */
    void Rebuild(const std::vector<PreviewRow>& rows) {
        rows_by_name_.clear();
        rows_by_name_.reserve(rows.size());
        for (std::size_t index = 0; index < rows.size(); ++index)
            rows_by_name_[rows[index].symbol_name].push_back(index);
    }

    /**
     * @brief 查找符号占据的行号集合。
     * @param name 符号规范名。
     * @return 行号集合指针；符号不在当前可见行集合中时返回 nullptr。
     */
    [[nodiscard]] const std::vector<std::size_t>* Find(
        std::string_view name) const noexcept {
        const auto iterator = rows_by_name_.find(std::string(name));
        return iterator == rows_by_name_.end() ? nullptr : &iterator->second;
    }

    /** @brief 已登记的符号数量，用于诊断与测试。 */
    [[nodiscard]] std::size_t Size() const noexcept {
        return rows_by_name_.size();
    }

private:
    /** 符号名到行号集合的映射。 */
    std::unordered_map<std::string, std::vector<std::size_t>> rows_by_name_;
};

/**
 * @brief 按行索引刷新某个符号占据的全部行文本。
 *
 * 这是增量刷新的核心：一次观测更新只重算该符号的父行与子值行，代价为
 * O(该符号行数)，与候选总数无关。符号不在可见行集合时不做任何事。
 *
 * @tparam Formatter 行号到文本的格式化回调类型，签名等价于
 *         std::string(std::size_t row_index)。
 * @param labels 标签数组，与建索引时的行布局一一对应。
 * @param index 行索引。
 * @param name 需要刷新的符号规范名。
 * @param formatter 格式化回调，负责读取当前观测状态生成该行文本。
 * @return 实际刷新的行数；符号不可见时返回 0。
 */
template <typename Formatter>
std::size_t RefreshPreviewRows(std::vector<std::string>& labels,
                               const PreviewRowIndex& index,
                               std::string_view name,
                               Formatter&& formatter) {
    const auto* row_indices = index.Find(name);
    if (row_indices == nullptr) return 0U;
    std::size_t refreshed = 0U;
    for (const auto row_index : *row_indices) {
        if (row_index >= labels.size()) continue;
        labels[row_index] = formatter(row_index);
        ++refreshed;
    }
    return refreshed;
}

}  // namespace calmcar::xcp::tools

#endif  // CALMCAR_XCP_TOOLS_PREVIEW_REFRESH_HPP