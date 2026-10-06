/**
 * @file measurement_database.hpp
 * @brief 测量子系统的核心窄接口 `IMeasurementDatabase`（v0.4，A2L-free）。
 *
 * 依 AGENTS.md I8 与架构文档 §4.1：核心只依赖这一个窄接口与核心自有值类型，
 * **绝不**使用桥接层的 A2L 类型。桥接侧由适配器
 * （`adapter/a2l/`，v0.5）把 A2L 数据库接口包装为
 * 本接口。对应测量子系统代码增长计划 §5.1。
 */

#ifndef CALMCAR_XCP_MEASUREMENT_MEASUREMENT_DATABASE_HPP_
#define CALMCAR_XCP_MEASUREMENT_MEASUREMENT_DATABASE_HPP_

#include <cstdint>
#include <string_view>

#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/measurement/measurement_types.hpp"
#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp {

/**
 * @brief 核心可见的只读测量符号视图（窄：只取测量路径需要的最小集）
 * @details 适配器从 A2L `IA2lDatabase::Find` 的 `SymbolInfo` 投影而来（v0.5）。
 *          核心规划器只依赖此结构，不感知 A2L 的地址模式/换算细节：
 *          - element_size_bytes：单元素**实占字节**（B-1 口径，不除 AG）；
 *          - element_count：元素数（首版仅标量=1；数组在后续里程碑）。
 */
struct MeasurementSymbolInfo {
    Address address{0};        ///< 32 位地址（AG 无关；实际存取按 AG 换算）
    AddressExtension extension{0};  ///< 8 位地址扩展
    std::uint16_t event_channel{0};  ///< 来自 A2L Event（仅用于规划分组；0=不由通道触发）
    std::uint8_t element_size_bytes{0};  ///< 单元素实占字节（B-1 口径，不除 AG）
    std::uint8_t element_count{1};       ///< 元素数（首版仅标量=1）
};

/**
 * @brief 核心测量窄接口（只读数据库视图）
 * @details 生命周期须长于使用者（规划器/会话）；不可拷贝（持有者是唯一 owner）。
 *          name 可为全库唯一裸名或规范键；歧义 → AmbiguousName。
 *          ToPhysical 的 raw 期望为**元素宽度**字节（不除 AG，与 Find 的
 *          element_size_bytes 口径一致；长度不符 → RawSizeMismatch）。
 */
class IMeasurementDatabase {
public:
    virtual ~IMeasurementDatabase() = default;
    /// @brief 默认构造（显式：本接口含纯虚，需派生类可默认构造）
    IMeasurementDatabase() = default;
    IMeasurementDatabase(const IMeasurementDatabase&) = delete;
    IMeasurementDatabase& operator=(const IMeasurementDatabase&) = delete;

    /// @brief 按名字查命中测量符号的布局（地址/扩展/事件通道/元素宽度/数量）
    [[nodiscard]] virtual MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const = 0;

    /// @brief 原始字节（元素宽度）→ 物理值（长度不符 → RawSizeMismatch）
    [[nodiscard]] virtual MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view name, BytesView raw) const = 0;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_MEASUREMENT_MEASUREMENT_DATABASE_HPP_