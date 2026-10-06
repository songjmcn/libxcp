/**
 * @file a2l_measurement_database.hpp
 * @brief A2L 测量数据库适配器：把桥接层 `IA2lDatabase` 包装为核心窄接口
 *        `IMeasurementDatabase`（v0.5）。
 *
 * 对应测量子系统代码增长计划 §5.5 / D1 落点（`adapter/a2l/` 独立目录 +
 * 独立 CMake target `libxcp_measurement_adapter`）。适配器只做两件事：
 *   1. `Find(name)` → `MeasurementSymbolInfo`（A2L SymbolInfo 的核心字段映射）；
 *   2. `ToPhysical(name, raw)` → `MeasurementValue`（a2l::PhysicalValue → core 值）。
 * 物理换算、地址/宽度探测全部委托给桥接库，本类**不实现任何换算 / AG 逻辑**。
 *
 * 依赖方向：适配器 `.cpp` 仅包含核心 `<libxcp/measurement/*.hpp>` 与桥接层
 * `<libxcp/a2l/ia2l_database.hpp>`；核心**绝不**包含 `<libxcp/a2l/*>`。
 * A2lIsolation 门禁（tests/a2l_isolation_check.cmake）据此保证 S1/S2 0 命中。
 */

#ifndef LIBXCP_MEASUREMENT_ADAPTER_A2L_MEASUREMENT_DATABASE_HPP_
#define LIBXCP_MEASUREMENT_ADAPTER_A2L_MEASUREMENT_DATABASE_HPP_

#include "libxcp/measurement/measurement_database.hpp"

namespace calmcar::xcp {

namespace a2l {
class IA2lDatabase;  // 仅前置声明；实现转换在 .cpp 完成
}  // namespace a2l

/**
 * @brief A2L 数据库 → 核心 `IMeasurementDatabase` 的适配器
 *
 * @details 例：
 * @code
 * const a2l::A2lBridge* bridge;  // Load 成功后
 * A2lMeasurementDatabase adapter(*bridge->Database());
 * MeasurementSession session(adapter);
 * @endcode
 * 生命周期：适配器持有对 `IA2lDatabase` 的引用，Bridge/数据库须长于适配器。
 */
class A2lMeasurementDatabase : public IMeasurementDatabase {
public:
    /**
     * @brief 构造适配器
     * @param database A2L 桥接层数据库视图（非拥有，生命周期须长于本对象）
     * @details v0.9 修正：桥接层 `A2lBridge::Database()` 暴露的是 const 只读
     *          视图（`const IA2lDatabase*`），而 `IA2lDatabase` 的接口全部为
     *          const 成员，故适配器持 const 引用即可（v0.5 只写不编未暴露）。
     */
    explicit A2lMeasurementDatabase(const a2l::IA2lDatabase& database);

    ~A2lMeasurementDatabase() override;

    A2lMeasurementDatabase(const A2lMeasurementDatabase&) = delete;
    A2lMeasurementDatabase& operator=(const A2lMeasurementDatabase&) = delete;

    /**
     * @brief 按名字查找符号信息（地址/扩展/事件通道/宽度/元素数）
     * @param name 规范键（module::name）或全库唯一裸名
     * @return MeasurementSymbolInfo，或核心侧错误（NotFound/AmbiguousName…）
     */
    [[nodiscard]] MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const override;

    /**
     * @brief 原始字节 → 物理值（直接委托桥接库换算）
     * @param name 规范键或全库唯一裸名
     * @param raw 实际字节（长度须等于 element_size_bytes）
     * @return MeasurementValue（int64/uint64/double/string/bool），或核心侧错误
     */
    [[nodiscard]] MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view name, BytesView raw) const override;

private:
    const a2l::IA2lDatabase& m_database_;  ///< A2L 桥接层数据库（非拥有）
};

}  // namespace calmcar::xcp

#endif  // LIBXCP_MEASUREMENT_ADAPTER_A2L_MEASUREMENT_DATABASE_HPP_