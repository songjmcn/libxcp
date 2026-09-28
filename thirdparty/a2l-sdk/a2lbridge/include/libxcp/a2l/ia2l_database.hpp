// =============================================================================
// ia2l_database.hpp —— 只读数据库查询接口（设计文档 §4.2，B-13/B-17/B-20）
//
// 实现方为 A2lDatabaseImpl（src/a2l_database_impl.cpp），持有 Load 成功后
// 一次性发布的不可变快照（B-20）；加载中所有查询返回 NotReady。
// =============================================================================

#ifndef LIBXCP_A2L_IA2L_DATABASE_HPP_
#define LIBXCP_A2L_IA2L_DATABASE_HPP_

#include <cstddef>
#include <string_view>
#include <vector>

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::a2l {

/**
 * @class IA2lDatabase
 * @brief A2L 符号数据库的查询视图（不可变快照，可并发只读访问）。
 */
class IA2lDatabase {
public:
    IA2lDatabase() = default;
    virtual ~IA2lDatabase() = default;

    IA2lDatabase(const IA2lDatabase&) = delete;
    IA2lDatabase& operator=(const IA2lDatabase&) = delete;
    IA2lDatabase(IA2lDatabase&&) = delete;
    IA2lDatabase& operator=(IA2lDatabase&&) = delete;

    /**
     * @brief 按 module::symbol 精确查找（B-13/B-17）
     * @param qualified_or_unique_name `module::name` 形式精确匹配；
     *        裸名仅在全库唯一时成功，歧义返回 AmbiguousName。
     * @return 符号快照副本，或结构化错误
     */
    [[nodiscard]] virtual Result<SymbolInfo> Find(
        std::string_view qualified_or_unique_name) const = 0;

    /**
     * @brief 大小写无关通配符检索（`*` 任意串、`?` 单字符）
     * @param pattern 匹配 `module::name` 全限定名的模式
     * @param max_count 稳定排序后的截断上限（0 视为非法参数 BadArgument）
     * @return 命中列表（按全限定名字典序），或结构化错误
     */
    [[nodiscard]] virtual Result<std::vector<SymbolInfo>> Search(
        std::string_view pattern, std::size_t max_count = 200) const = 0;

    /// @brief 已发布快照中的符号数量；加载中返回 NotReady（B-20）
    [[nodiscard]] virtual Result<std::size_t> Count() const = 0;

    /**
     * @brief 计算元素连续存储所占实际字节数（B-1：返回值不除以 AG）
     * @param qualified_name 规范键或全库唯一裸名
     */
    [[nodiscard]] virtual Result<std::size_t> ByteSizeOf(
        std::string_view qualified_name) const = 0;

    /**
     * @brief raw 字节 → tagged 物理值（B-8/B-14）
     * @param qualified_name 规范键或全库唯一裸名
     * @param raw 实际字节（长度必须等于 element_size_bytes，RawSizeMismatch）
     * @details 仅支持标量符号（dimensions 为空）；数组/曲线类元素级解码走
     *          IDaqLayout::Decode，此处返回 UnsupportedOperation。
     */
    [[nodiscard]] virtual Result<PhysicalValue> ToPhysical(
        std::string_view qualified_name, BytesView raw) const = 0;

    /**
     * @brief 物理值 → raw 字节（默认严格拒绝越界、NaN/Inf 和不可逆映射，B-15）
     * @param qualified_name 规范键或全库唯一裸名
     * @param physical_value tagged 物理值
     */
    [[nodiscard]] virtual Result<Bytes> FromPhysical(
        std::string_view qualified_name,
        const PhysicalValue& physical_value) const = 0;
};

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_IA2L_DATABASE_HPP_
