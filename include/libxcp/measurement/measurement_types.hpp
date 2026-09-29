/**
 * @file measurement_types.hpp
 * @brief 测量子系统的核心值类型（v0.4，A2L-free）。
 *
 * 依 AGENTS.md I8：核心测量类型**绝不**使用桥接层的 `PhysicalValue`
 * 或 A2L 的 `Bytes`；此处自持 `MeasurementValue`（镜像桥接层 PhysicalValue 的
 * 五选一变体）。对应架构文档 §11 与测量子系统代码增长计划 §5.1。
 */

#ifndef CALMCAR_XCP_MEASUREMENT_MEASUREMENT_TYPES_HPP_
#define CALMCAR_XCP_MEASUREMENT_MEASUREMENT_TYPES_HPP_

#include <cstdint>
#include <string>
#include <variant>

namespace calmcar::xcp {

/**
 * @brief 测量物理值（核心自持类型）
 * @details 镜像桥接层 `PhysicalValue = variant<int64_t,uint64_t,double,string,bool>`
 *          （thirdparty/a2l-sdk/桥接目录/.../a2l_types.hpp:111）。五种备选与
 *          ASAM 数据类型一一对应：
 *          - int64_t    —— 有符号整数（SByte/SWord/.../SLong64 等）
 *          - uint64_t   —— 无符号整数（UByte/UShort/.../ULong64 与 Boolean 展开）
 *          - double     —— 定点/浮点换算后物理值（Float32/Float64/FixedPoint）
 *          - std::string —— String/Text 类型
 *          - bool       —— Boolean 类型（换算无关时）
 *          本类型只承载"物理值"，不做换算；原始字节在 `MeasurementSample::raw`。
 */
using MeasurementValue =
    std::variant<std::int64_t, std::uint64_t, double, std::string, bool>;

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_MEASUREMENT_MEASUREMENT_TYPES_HPP_