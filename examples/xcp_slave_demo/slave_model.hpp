/**
 * @file slave_model.hpp
 * @brief 独立 XCPlite Slave 示例的测量与标定数据模型。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace xcp_slave_demo {

/// @brief 示例中所有固定长度数组的元素数量。
inline constexpr std::size_t kSampleCount = 8;

/**
 * @brief 基础测量值集合，覆盖有符号、无符号整数及浮点基础类型。
 */
struct BasicMeasurementValues {
    std::int8_t signed_8{0};       ///< 有符号 8 位测量值。
    std::uint8_t unsigned_8{0};    ///< 无符号 8 位测量值。
    std::int16_t signed_16{0};     ///< 有符号 16 位测量值。
    std::uint16_t unsigned_16{0};  ///< 无符号 16 位测量值。
    std::int32_t signed_32{0};     ///< 有符号 32 位测量值。
    std::uint32_t unsigned_32{0};  ///< 无符号 32 位测量值。
    float real_32{0.0F};           ///< 32 位浮点测量值。
    double real_64{0.0};           ///< 64 位浮点测量值。
};

/**
 * @brief 快速测量事件中用于描述算法状态的嵌套结构体。
 */
struct FastMeasurementState {
    std::uint32_t source_tick{0};     ///< 最近一次计算使用的采样序号。
    double raw_output{0.0};           ///< 限幅前的算法输出。
    std::int32_t diagnostic_code{0};  ///< 最近一次标定参数校验诊断码。
};

/**
 * @brief 10 ms 快速测量事件的数据对象。
 */
struct FastMeasurements {
    std::uint32_t tick{0};          ///< 快速事件采样序号。
    float input{0.0F};              ///< 模拟算法输入。
    double output{0.0};             ///< 经标定与限幅后的算法输出。
    FastMeasurementState state{};   ///< 嵌套的快速算法状态。
    float samples[kSampleCount]{};  ///< 应用各标定修正量后的数组测量值。
};

/**
 * @brief 慢速测量事件的统计结构体。
 */
struct SlowMeasurementStatistics {
    std::uint32_t fast_cycle_count{0};  ///< 最近一次快循环序号。
    std::uint32_t sample_count{0};  ///< 参与均值更新的慢速样本数（饱和计数）。
    double mean_output{0.0};        ///< 快循环输出的累计均值。
    std::int32_t last_diagnostic_code{0};  ///< 最近一次快循环诊断码。
};

/**
 * @brief 100 ms 慢速测量事件的数据对象。
 */
struct SlowMeasurements {
    BasicMeasurementValues basic{};          ///< 慢速基础类型测量值。
    SlowMeasurementStatistics statistics{};  ///< 慢速统计结构体。
    std::uint32_t history[kSampleCount]{};   ///< 最近的快循环序号历史数组。
};

/**
 * @brief 基础标定值集合，覆盖有符号、无符号整数及浮点基础类型。
 */
struct BasicCalibrationValues {
    std::int8_t signed_8{-7};          ///< 有符号 8 位标定值。
    std::uint8_t unsigned_8{7};        ///< 无符号 8 位标定值。
    std::int16_t signed_16{-101};      ///< 有符号 16 位标定值。
    std::uint16_t unsigned_16{123};    ///< 无符号 16 位标定值。
    std::int32_t signed_32{-10001};    ///< 有符号 32 位标定值。
    std::uint32_t unsigned_32{10001};  ///< 无符号 32 位标定值。
    float real_32{1.25F};              ///< 32 位浮点标定值。
    double real_64{-2.5};              ///< 64 位浮点标定值。
};

/**
 * @brief 用于模拟算法的嵌套标定结构体。
 */
struct CalibrationModel {
    double gain{1.0};             ///< 算法增益，允许范围为 0 到 10。
    double offset{0.0};           ///< 算法偏置，允许范围为 -100 到 100。
    double lower_limit{-1000.0};  ///< 算法输出下限。
    double upper_limit{1000.0};   ///< 算法输出上限。
};

/**
 * @brief XCP 标定段中的完整标定对象。
 */
struct CalibrationParameters {
    BasicCalibrationValues basic{};     ///< 可直接读写验证的基础标定值。
    CalibrationModel model{};           ///< 供模拟算法使用的嵌套标定结构。
    float corrections[kSampleCount]{};  ///< 每个数组采样通道的标定修正量。
};

/**
 * @brief 返回默认标定参数。
 * @return 含有计划默认值的标定参数对象。
 */
[[nodiscard]] CalibrationParameters MakeDefaultCalibrationParameters() noexcept;

/**
 * @brief 检查标定参数是否处于算法允许的安全范围。
 * @param parameters 待检查的完整标定参数。
 * @param error_message 可选的中文错误说明输出位置。
 * @return 参数全部有效时返回 true。
 */
[[nodiscard]] bool ValidateCalibrationParameters(
    const CalibrationParameters& parameters,
    std::string* error_message = nullptr);

/**
 * @brief 按标定参数更新 10 ms 快速测量对象。
 * @param measurements 待更新的快速测量对象。
 * @param parameters 从 CalSeg 短时读锁快照得到的标定参数。
 * @param tick 当前快速采样序号。
 */
void UpdateFastMeasurements(FastMeasurements& measurements,
                            const CalibrationParameters& parameters,
                            std::uint32_t tick) noexcept;

/**
 * @brief 按最新快速测量值更新 100 ms 慢速测量对象。
 * @param measurements 待更新的慢速测量对象。
 * @param fast_measurements 最新快速测量快照。
 */
void UpdateSlowMeasurements(SlowMeasurements& measurements,
                            const FastMeasurements& fast_measurements) noexcept;

}  // namespace xcp_slave_demo
