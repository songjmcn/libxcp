// =============================================================================
// record_layout.hpp —— RECORD_LAYOUT 元数据（设计文档 §4 /
// B-4，首里程碑仅识别）
//
// 首里程碑 SDK 不导出 RECORD_LAYOUT 明细（字段清单未定，B-4），本头文件仅
// 定义元数据结构与拒绝语义，供 CHARACTERISTIC Curve/Map 的“仅元数据”承诺
// （§6.3）使用；不做任何布局推导或字节猜测。
// =============================================================================

#ifndef LIBXCP_A2L_RECORD_LAYOUT_HPP_
#define LIBXCP_A2L_RECORD_LAYOUT_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::a2l {

/**
 * @brief RECORD_LAYOUT 的归一化类别
 * @details StandardComplex 等复杂版式在首里程碑一律拒绝读写（InvalidLayout）。
 */
enum class RecordLayoutKind : std::uint8_t {
    Unknown,    ///< 未识别
    Value,      ///< VALUE：单值（执行）
    ValBlk,     ///< VAL_BLK：连续块（执行，元素类型须显式已知）
    FncValues,  ///< 标准连续 FNC_VALUES（执行）
    AxisPts,    ///< AXIS_PTS_X/Y/Z（仅元数据）
    Complex,    ///< 含 RESERVE/DIST_OP/SHIFT_OP/RIP_ADDR 等非连续要素（拒绝）
};

/**
 * @brief RECORD_LAYOUT 元数据快照
 * @details 首里程碑只回答“可否读写”，不展开字段级布局（B-4 字段清单未定）。
 */
struct RecordLayoutInfo {
    std::string name;                                   ///< RECORD_LAYOUT 名
    RecordLayoutKind kind = RecordLayoutKind::Unknown;  ///< 归一化类别
    bool writable = false;                              ///< 是否属于可执行版式
};

/**
 * @brief 判断版式在首里程碑是否可执行（VALUE/VAL_BLK/FNC_VALUES）
 * @param info 元数据快照
 * @return 不可执行时返回 InvalidLayout 错误（禁止按字节数组猜测）
 */
[[nodiscard]] Result<void> CheckRecordLayoutExecutable(
    const RecordLayoutInfo& info) noexcept;

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_RECORD_LAYOUT_HPP_
