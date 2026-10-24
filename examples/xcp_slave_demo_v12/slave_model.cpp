/**
 * @file slave_model.cpp
 * @brief 从站测量与标定模型的确定性实现。
 * @note 本文件禁止包含 XCPlite / libxcp 头文件（原因见 slave_model.hpp 文件头）。
 */
#include "slave_model.hpp"

#include <cmath>

namespace xcp_slave_demo_v12 {
namespace {

/// @brief 圆周率（避免依赖 M_PI 的平台可见性）。
constexpr double kPi = 3.14159265358979323846;

/**
 * @brief 按 ±limit 限幅。
 * @param value 待限幅值。
 * @param limit 限幅上界（取绝对值使用）。
 * @return 落在 [-limit, +limit] 内的值。
 */
float ClampSymmetric(float value, float limit) noexcept {
    const float bound = std::fabs(limit);
    if (value > bound) {
        return bound;
    }
    if (value < -bound) {
        return -bound;
    }
    return value;
}

}  // namespace

CalibrationParameters MakeDefaultCalibrationParameters() noexcept {
    return CalibrationParameters{};
}

SlaveModel::SlaveModel(const CalibrationParameters& parameters) noexcept
    : parameters_(parameters) {
    // 构造即算出一帧初值，便于主站在 DAQ 启动前用 SHORT_UPLOAD 读到有效数据。
    StepFast();
}

void SlaveModel::SetCalibration(
    const CalibrationParameters& parameters) noexcept {
    parameters_ = parameters;
}

const CalibrationParameters& SlaveModel::Calibration() const noexcept {
    return parameters_;
}

float SlaveModel::Shape(float normalized) const noexcept {
    return ClampSymmetric(parameters_.gain * normalized + parameters_.offset,
                          parameters_.limit);
}

void SlaveModel::StepFast() noexcept {
    // run=0 时冻结推进：不递增计数、不改动输出，便于主站观察"标定生效"。
    if (parameters_.run != 0U) {
        ++fast_tick_;

        const std::uint32_t phase = fast_tick_ % kPhaseCount;
        const double angle =
            2.0 * kPi * static_cast<double>(phase) / static_cast<double>(kPhaseCount);

        fast_.sine = Shape(static_cast<float>(std::sin(angle)));
        fast_.square = Shape(phase < kPhaseCount / 2U ? 1.0F : -1.0F);
        // 锯齿：一个周期内从 -1 线性升到 +1。
        const float ramp = 2.0F * static_cast<float>(phase) /
                               static_cast<float>(kPhaseCount - 1U) -
                           1.0F;
        fast_.saw = Shape(ramp);

        // 数组元素参与修正量，保证标定 corrections[i] 能被观测到。
        for (std::size_t index = 0; index < kSampleCount; ++index) {
            fast_.samples[index] =
                ClampSymmetric(fast_.sine + parameters_.corrections[index],
                               4.0F * std::fabs(parameters_.limit));
        }
    }
    fast_.counter = fast_tick_;
}

void SlaveModel::StepSlow() noexcept {
    ++slow_tick_;

    // 八种基础类型：取值由慢速周期数确定性派生，便于断言。
    BasicMeasurementValues& basic = slow_.basic;
    basic.u8 = static_cast<std::uint8_t>(slow_tick_);
    basic.s8 = static_cast<std::int8_t>(-static_cast<int>(slow_tick_ % 100U));
    basic.u16 = static_cast<std::uint16_t>(slow_tick_ * 3U);
    basic.s16 = static_cast<std::int16_t>(-static_cast<int>(slow_tick_ % 1000U));
    basic.u32 = slow_tick_ * 1000U;
    basic.s32 = -static_cast<std::int32_t>(slow_tick_ * 7U);
    basic.f32 = fast_.sine;
    basic.f64 = static_cast<double>(fast_.sine);

    // 统计：累计均值按慢速周期数索引，除数为 0 时不产生 NaN。
    mean_accumulator_ += fast_.sine;
    slow_.stats.fast_cycles = fast_tick_;
    slow_.stats.slow_cycles = slow_tick_;
    slow_.stats.mean_output =
        mean_accumulator_ / static_cast<float>(slow_tick_);
    // 诊断码：参数越界时给出负值，正常为 0（用于验证标定写入被真正消费）。
    slow_.stats.last_diagnostic =
        (parameters_.limit <= 0.0F || parameters_.gain < 0.0F) ? -1 : 0;

    // 历史环形缓冲：左移一格后写入最新的正弦样本，history[7] 始终是最新值。
    for (std::size_t index = 1; index < kSampleCount; ++index) {
        slow_.history[index - 1U] = slow_.history[index];
    }
    slow_.history[kSampleCount - 1U] = fast_.sine;

    slow_.event_tick = slow_tick_;
}

const FastMeasurements& SlaveModel::Fast() const noexcept { return fast_; }

const SlowMeasurements& SlaveModel::Slow() const noexcept { return slow_; }

std::uint32_t SlaveModel::FastTick() const noexcept { return fast_tick_; }

std::uint32_t SlaveModel::SlowTick() const noexcept { return slow_tick_; }

}  // namespace xcp_slave_demo_v12