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

/**
 * @brief 单个 DAQ Entry 的运行时限制（由已验证的运行时 profile 提供）
 * @details `supports_contiguous_1d_arrays=false` 时数组保持拒绝；粒度或最大
 *          Entry 长度为 0 表示未知，不能据此开放数组。
 *
 *          **单位纪律（批次 E2/F-12 修正）**：规范（原始 PDF 第 15 页
 *          §4.1.2）原文规定 MAX_ODT_ENTRY_SIZE_x 的单位是
 *          **ADDRESS_GRANULARITY**：
 *          "The MAX_ODT_ENTRY_SIZE_x parameters indicate the upper limits for
 *           the size of the element described by an ODT entry **in
 *           ADDRESS_GRANULARITY**."
 *          并给出等式 `SizeOf(ODT entry)[AG] <= MAX_ODT_ENTRY_SIZE_x[AG]`。
 *          因此本结构中的 `max_entry_size_elements` **以 AG 为单位（元素
 *          数）**，而非字节数——AG=1 时两者恰好等价，故旧命名
 *          `max_entry_size_bytes` 长期未暴露其单位错误。
 */
struct MeasurementDaqCapabilities {
    bool supports_contiguous_1d_arrays{false};  ///< 是否允许首期连续一维数组 Entry
    /// @brief DAQ Entry 粒度（**字节数**：规范 GRANULARITY_ODT_ENTRY_SIZE_x
    ///        本身即以 BYTE 表示，取值范围 {1,2,4,8}），0 表示未知
    std::uint8_t entry_granularity_bytes{0};
    /// @brief 单 Entry 最大元素数（**AG 单位**，对应规范
    ///        MAX_ODT_ENTRY_SIZE_x[AG]），0 表示未知
    std::uint8_t max_entry_size_elements{0};
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_MEASUREMENT_MEASUREMENT_TYPES_HPP_