// =============================================================================
// a2l_database_impl.hpp —— 快照数据库类定义（内部头，仅 src/ 使用）
//
// 单独成头的原因：a2l_bridge.cpp（门面）持有 unique_ptr<A2lDatabaseImpl>，
// 需要完整类型做 ①deleter 实例化 ②向上转型为 IA2lDatabase* 返回给消费者。
// 成员函数定义全部在 a2l_database_impl.cpp（匿名命名空间辅助不泄漏）。
// =============================================================================

#ifndef LIBXCP_A2L_SRC_A2L_DATABASE_IMPL_HPP_
#define LIBXCP_A2L_SRC_A2L_DATABASE_IMPL_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "a2l_dto_map.hpp"
#include "libxcp/a2l/ia2l_database.hpp"

namespace calmcar::xcp::a2l::detail {

/**
 * @class A2lDatabaseImpl
 * @brief IA2lDatabase 的不可变快照实现（B-20：Load 成功后只读，可并发查询）
 */
class A2lDatabaseImpl final : public IA2lDatabase {
public:
    /// @brief 从 SDK DTO 全量列表构建快照（含规范键索引与裸名计数）
    explicit A2lDatabaseImpl(std::vector<liba2l::SymbolDto> dtos);

    /// @brief 建立地址 → 符号名反查表（DAQ entry 归属用；同址多符号不建立映射）
    void BuildAddressIndex();

    /// @brief 按基地址反查规范符号名（无匹配/歧义返回空串）
    [[nodiscard]] std::string FindByAddress(std::uint64_t address) const;

    /// @copydoc IA2lDatabase::Find
    [[nodiscard]] Result<SymbolInfo> Find(
        std::string_view qualified_or_unique_name) const override;

    /// @copydoc IA2lDatabase::Search
    [[nodiscard]] Result<std::vector<SymbolInfo>> Search(
        std::string_view pattern, std::size_t max_count) const override;

    /// @copydoc IA2lDatabase::Count
    [[nodiscard]] Result<std::size_t> Count() const override;

    /// @copydoc IA2lDatabase::ByteSizeOf
    [[nodiscard]] Result<std::size_t> ByteSizeOf(
        std::string_view qualified_name) const override;

    /// @copydoc IA2lDatabase::ToPhysical
    [[nodiscard]] Result<PhysicalValue> ToPhysical(
        std::string_view qualified_name, BytesView raw) const override;

    /// @copydoc IA2lDatabase::FromPhysical
    [[nodiscard]] Result<Bytes> FromPhysical(
        std::string_view qualified_name,
        const PhysicalValue& physical_value) const override;

private:
    /// @brief B-13/B-17 解析：module::name 精确；裸名须全库唯一
    [[nodiscard]] Result<SymbolInfo> Resolve(std::string_view name) const;

    std::vector<SymbolInfo> m_symbols_;  ///< 按全限定名排序的不可变快照
    std::unordered_map<std::string, std::size_t>
        m_index_;  ///< module::name → 下标
    std::unordered_map<std::string, std::size_t>
        m_bare_counts_;  ///< 小写裸名 → 出现次数
    std::unordered_map<std::uint64_t, std::string>
        m_addr_to_symbol_;  ///< 基址反查
};

}  // namespace calmcar::xcp::a2l::detail

#endif  // LIBXCP_A2L_SRC_A2L_DATABASE_IMPL_HPP_
