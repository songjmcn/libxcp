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
#include <unordered_set>
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
    /// @param dtos MEASUREMENT/CHARACTERISTIC 符号快照
    /// @param structs STRUCTURE/INSTANCE 元数据快照（批次13，B-12）
    /// @param record_layouts 规范键 → RECORD_LAYOUT 元数据（批次13 接线；
    ///        仅 SDK 给出了版式类别的 CHARACTERISTIC 才有项）
    /// @param leaf_symbols 结构体成员叶子 SymbolInfo（批次18，STRUCTLEAF；
    ///        name 为 "instance.member[.member][[idx]]" 限定路径）
    /// @param leaf_warnings 叶子解析产生的不可证拒绝告警（16-B，随快照发布）
    A2lDatabaseImpl(
        std::vector<liba2l::SymbolDto> dtos,
        std::vector<liba2l::StructInfoDto> structs,
        std::unordered_map<std::string, RecordLayoutInfo> record_layouts,
        std::vector<SymbolInfo> leaf_symbols,
        std::vector<LoadWarning> leaf_warnings);

    /// @brief 加载期被放弃的事实（批次15，F6：同名冲突跳过的结构体等）
    /// @details 门面 A2lBridge::ListLoadWarnings() 在此基础上再补自己的告警
    ///          （如 DAQ 列表未声明 FIRST_PID 的回退提示）。
    [[nodiscard]] const std::vector<LoadWarning>& LoadWarnings()
        const noexcept {
        return m_load_warnings_;
    }

    /// @brief 建立地址 → 符号名反查表（DAQ entry 归属用；同址多符号不建立映射）
    void BuildAddressIndex();

    /// @brief 按基地址反查规范符号名（无匹配/歧义返回空串）
    [[nodiscard]] std::string FindByAddress(std::uint64_t address) const;

    /**
     * @brief 按基地址列出**全部**候选规范符号名（批次14，B-6 保留 aliases）
     * @details `FindByAddress` 在歧义时故意返回空（不猜归属），但"不猜"不等于
     *          "不留痕"：本函数返回该地址上的全部符号名（升序稳定），供布局
     *          快照填 `symbol_aliases`。STRUCTURE/INSTANCE 同样被排除在外
     *          （B-12 不参与寻址）。
     */
    [[nodiscard]] std::vector<std::string> FindAllByAddress(
        std::uint64_t address) const;

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

    /**
     * @brief 数值读写前的统一准入门（批次13：R1 STRUCTURE + R3 RL 接线）
     * @details 两道门，顺序固定：
     *          ① `kind==Structure` → `UnsupportedOperation`（B-12：结构体
     *             只识别元数据，禁止当字节数组读写）；
     *          ② CHARACTERISTIC 且 SDK 给出了版式类别 →
     *             `CheckRecordLayoutExecutable`（B-4/B-12：AXIS_PTS →
     *             UnsupportedOperation，复合/不可证版式 → InvalidLayout，
     *             Phase::Layout）。无版式信息不放行到猜测，而是继续由
     *             下游 B-3 元素宽度门拒绝。
     */
    [[nodiscard]] Result<void> GateExecutable(const SymbolInfo& symbol,
                                              std::string_view name) const;

    std::vector<SymbolInfo> m_symbols_;  ///< 按全限定名排序的不可变快照
    /// @brief 加载期告警（批次15，F6；构造时填充后只读）
    std::vector<LoadWarning> m_load_warnings_;
    std::unordered_map<std::string, std::size_t>
        m_index_;  ///< module::name → 下标
    std::unordered_map<std::string, std::size_t>
        m_bare_counts_;  ///< 小写裸名 → 出现次数
    std::unordered_map<std::uint64_t, std::string>
        m_addr_to_symbol_;  ///< 基址反查（唯一归属才入表）
    /// @brief 基址 → 全部候选规范名（批次14，B-6 aliases；含歧义地址）
    std::unordered_map<std::uint64_t, std::vector<std::string>> m_addr_to_all_;
    /// @brief 规范键 → RECORD_LAYOUT 元数据（批次13；无类别的符号不在此表）
    std::unordered_map<std::string, RecordLayoutInfo> m_record_layouts_;
    /// @brief 结构体叶子在 m_symbols_ 中的位置集合（批次18）：地址反查表
    ///        **排除**这些项，保持既有 FindByAddress/aliases 行为不变
    ///        （叶子的归属检索是批次20 DAQ 成员路由的前置，另行接线）
    std::unordered_set<std::size_t> m_leaf_positions_;
};

}  // namespace calmcar::xcp::a2l::detail

#endif  // LIBXCP_A2L_SRC_A2L_DATABASE_IMPL_HPP_
