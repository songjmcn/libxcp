// =============================================================================
// structure_layout.hpp —— STRUCTURE/INSTANCE 叶子展开解析器（批次18，16-B）
//
// 内部头（仅 a2lbridge/src 使用，不进公开头树）。
// 输入三类上游事实（TYPEDEF_STRUCTURE / INSTANCE / TYPEDEF_MEASUREMENT），
// 输出每个 INSTANCE 的**可证叶子** SymbolInfo 列表与拒绝告警。
//
// 纪律（批次16 §2 设计决策的落地）：
//  - SDK 只导出事实，递归/寻址/换算解析全部在本文件 + 数据库装配层；
//  - 不猜测：成员引用不可证（缺 typedef、宽度未知、环、溢出、越界、
//    超预算）→ 该节点拒绝并留 LoadWarning，实例其余可证叶子不受牵连；
//  - 整块 STRUCTURE/INSTANCE 的读写拒绝（B-12）不变：本解析器只**新增**
//    限定路径叶子，不改变本体符号的元数据性质；
//  - 地址公式：INSTANCE 基址 + Σ成员 ADDRESS_OFFSET + 数组索引×元素步长；
//    AG 不乘入基址（B-1），address_extension 从 INSTANCE 原值透传，
//    成员级不存在独立扩展（XCPlite/ASAP2 语义核证）。
// =============================================================================

#ifndef LIBXCP_A2L_SRC_STRUCTURE_LAYOUT_HPP_
#define LIBXCP_A2L_SRC_STRUCTURE_LAYOUT_HPP_

#include <functional>
#include <string>
#include <vector>

#include "liba2l/liba2l_api.hpp"
#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::a2l::detail {

/// @brief 一个实例的叶子展开结果（诊断/调试用分组，最终合入 leaves）
struct InstanceLeaves {
    /// @brief 实例的限定路径前缀（MODULE 内实例名，如 g_outer）
    std::string instance_key;
    /// @brief 展开出的叶子（name 为 "instance.member[.member][[idx]]" 全路径）
    std::vector<SymbolInfo> leaves;
};

/// @brief 全库结构体叶子解析的产出
struct StructureLayoutResult {
    std::vector<SymbolInfo> leaves;     ///< 全部实例的可证叶子（未排序）
    std::vector<LoadWarning> warnings;  ///< 不可证节点的结构化拒绝记录
};

/**
 * @brief 展开所有 INSTANCE 的可证成员叶子
 * @param typedef_structs SDK 的 TYPEDEF_STRUCTURE 快照（含 members）
 * @param instances SDK 的 INSTANCE 快照（含基址/扩展/READ_WRITE）
 * @param typedefs SDK 的 TYPEDEF_MEASUREMENT 快照（标量成员类型；
 *        换算快照与 SymbolDto 同为 SDK 内嵌口径，无需外部解析）
 * @return 叶子列表 + 告警
 * @details 防护上界（批次16 §5 风险控制）：
 *  - 递归深度 ≤ 32（超限 → 该分支拒绝）；
 *  - 单实例叶子预算 4096，结构体数组索引展开 ≤ 1024 元素
 *    （超限 → 该数组节点拒绝并告警，防语料病态维度爆炸）；
 *  - 环检测：DFS 访问栈（自环与间接环都拒绝，错误信息带完整类型链）；
 *  - 越界检查：成员 offset + size ≤ 所在 TYPEDEF 的 SIZE（不可证则拒绝）；
 *  - 维度乘积 checked multiplication，溢出拒绝；
 *  - 成员数组（含多维标量数组）flatten 为单维 extent=乘积、stride=元素宽
 *    （连续数组线性化后 byte_offset = k×width 在 ROW/COLUMN_DIR 下等价，
 *    与 ComputeElementAddress 的线性索引口径天然兼容）。
 */
[[nodiscard]] StructureLayoutResult BuildStructureLeaves(
    const std::vector<liba2l::StructInfoDto>& typedef_structs,
    const std::vector<liba2l::StructInfoDto>& instances,
    const std::vector<liba2l::TypedefMeasurementDto>& typedefs);

}  // namespace calmcar::xcp::a2l::detail

#endif  // LIBXCP_A2L_SRC_STRUCTURE_LAYOUT_HPP_
