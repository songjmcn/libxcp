#include "slave_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace xcp_slave_demo {
namespace {

/// @brief 浮点型基础标定值允许的绝对上限。
constexpr double kBasicFloatLimit = 1000000.0;
/// @brief 单个通道修正量允许的绝对上限。
constexpr float kCorrectionLimit = 5.0F;

static_assert(std::is_standard_layout_v<FastMeasurements>);
static_assert(std::is_standard_layout_v<SlowMeasurements>);
static_assert(std::is_standard_layout_v<CalibrationParameters>);

/**
 * @brief 将错误说明写入可选输出。
 * @param message 错误说明。
 * @param output 可选输出指针。
 */
void SetError(const char* message, std::string* output) {
    if (output != nullptr) {
        *output = message;
    }
}

}  // namespace

CalibrationParameters MakeDefaultCalibrationParameters() noexcept {
    return CalibrationParameters{};
}

bool ValidateCalibrationParameters(const CalibrationParameters& parameters,
                                   std::string* error_message) {
    if (error_message != nullptr) {
        error_message->clear();
    }

    if (!std::isfinite(parameters.basic.real_32) ||
        !std::isfinite(parameters.basic.real_64) ||
        std::abs(static_cast<double>(parameters.basic.real_32)) >
            kBasicFloatLimit ||
        std::abs(parameters.basic.real_64) > kBasicFloatLimit) {
        SetError("基础浮点标定值超出有限数值范围", error_message);
        return false;
    }

    if (!std::isfinite(parameters.model.gain) || parameters.model.gain < 0.0 ||
        parameters.model.gain > 10.0) {
        SetError("算法增益必须处于 0 到 10 之间", error_message);
        return false;
    }
    if (!std::isfinite(parameters.model.offset) ||
        parameters.model.offset < -100.0 || parameters.model.offset > 100.0) {
        SetError("算法偏置必须处于 -100 到 100 之间", error_message);
        return false;
    }
    if (!std::isfinite(parameters.model.lower_limit) ||
        !std::isfinite(parameters.model.upper_limit) ||
        parameters.model.lower_limit >= parameters.model.upper_limit) {
        SetError("算法输出上下限必须为有限值且下限小于上限", error_message);
        return false;
    }

    for (const float correction : parameters.corrections) {
        if (!std::isfinite(correction) ||
            std::abs(correction) > kCorrectionLimit) {
            SetError("数组通道修正量必须处于 -5 到 5 之间", error_message);
            return false;
        }
    }
    return true;
}

void UpdateFastMeasurements(FastMeasurements& measurements,
                            const CalibrationParameters& parameters,
                            std::uint32_t tick) noexcept {
    measurements.tick = tick;
    measurements.input = static_cast<float>(tick % 100U) / 10.0F;
    measurements.state.source_tick = tick;

    if (!ValidateCalibrationParameters(parameters)) {
        measurements.state.raw_output = 0.0;
        measurements.state.diagnostic_code = 1;
        measurements.output = 0.0;
        for (std::size_t index = 0; index < kSampleCount; ++index) {
            measurements.samples[index] = measurements.input;
        }
        return;
    }

    for (std::size_t index = 0; index < kSampleCount; ++index) {
        measurements.samples[index] =
            measurements.input + parameters.corrections[index];
    }

    const double raw_output =
        parameters.model.gain * static_cast<double>(measurements.samples[0]) +
        parameters.model.offset;
    measurements.state.raw_output = raw_output;
    measurements.state.diagnostic_code = 0;
    measurements.output = std::clamp(raw_output, parameters.model.lower_limit,
                                     parameters.model.upper_limit);
}

void UpdateSlowMeasurements(
    SlowMeasurements& measurements,
    const FastMeasurements& fast_measurements) noexcept {
    measurements.basic.signed_8 = static_cast<std::int8_t>(
        static_cast<std::int32_t>(fast_measurements.tick % 101U) - 50);
    measurements.basic.unsigned_8 =
        static_cast<std::uint8_t>(fast_measurements.tick % 256U);
    measurements.basic.signed_16 = static_cast<std::int16_t>(
        static_cast<std::int32_t>(fast_measurements.tick % 2001U) - 1000);
    measurements.basic.unsigned_16 =
        static_cast<std::uint16_t>(fast_measurements.tick % 65536U);
    measurements.basic.signed_32 =
        static_cast<std::int32_t>(fast_measurements.tick);
    measurements.basic.unsigned_32 = fast_measurements.tick;
    measurements.basic.real_32 = static_cast<float>(fast_measurements.output);
    measurements.basic.real_64 = fast_measurements.output;

    measurements.statistics.fast_cycle_count = fast_measurements.tick;
    measurements.statistics.last_diagnostic_code =
        fast_measurements.state.diagnostic_code;

    const std::uint32_t previous_count = measurements.statistics.sample_count;
    const double denominator = static_cast<double>(previous_count) + 1.0;
    measurements.statistics.mean_output =
        (measurements.statistics.mean_output *
             static_cast<double>(previous_count) +
         fast_measurements.output) /
        denominator;
    if (previous_count != std::numeric_limits<std::uint32_t>::max()) {
        measurements.statistics.sample_count = previous_count + 1U;
    }

    const std::size_t history_index =
        static_cast<std::size_t>(fast_measurements.tick % kSampleCount);
    measurements.history[history_index] = fast_measurements.tick;
}

}  // namespace xcp_slave_demo
