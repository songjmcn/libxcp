/**
 * @file xcp_slave_demo_model_test.cpp
 * @brief 独立 Slave 测量算法与安全标定模型单元测试。
 */
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

#include "slave_model.hpp"

namespace xcp_slave_demo {
namespace {

/// @brief 校验默认标定对象满足应用层安全规则。
TEST(XcpSlaveDemoModelTest, DefaultCalibrationIsValid) {
    const CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    std::string error_message;

    EXPECT_TRUE(ValidateCalibrationParameters(parameters, &error_message));
    EXPECT_TRUE(error_message.empty());
}

/// @brief 校验快循环按修正数组、增益、偏置和上下限计算输出。
TEST(XcpSlaveDemoModelTest, FastUpdateAppliesArrayAndModelCalibration) {
    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    parameters.model.gain = 2.0;
    parameters.model.offset = 1.0;
    parameters.model.lower_limit = -1.0;
    parameters.model.upper_limit = 5.0;
    parameters.corrections[0] = 1.0F;
    parameters.corrections[7] = -1.0F;

    FastMeasurements measurements;
    UpdateFastMeasurements(measurements, parameters, 23U);

    EXPECT_EQ(measurements.tick, 23U);
    EXPECT_FLOAT_EQ(measurements.input, 2.3F);
    EXPECT_FLOAT_EQ(measurements.samples[0], 3.3F);
    EXPECT_FLOAT_EQ(measurements.samples[7], 1.3F);
    EXPECT_NEAR(measurements.state.raw_output, 7.6, 1e-6);
    EXPECT_DOUBLE_EQ(measurements.output, 5.0);
    EXPECT_EQ(measurements.state.diagnostic_code, 0);
}

/// @brief 校验非法标定被安全拒绝且快循环返回有诊断的有限默认值。
TEST(XcpSlaveDemoModelTest, InvalidCalibrationIsRejectedWithoutUnsafeOutput) {
    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    parameters.model.lower_limit = 10.0;
    parameters.model.upper_limit = 10.0;
    std::string error_message;

    EXPECT_FALSE(ValidateCalibrationParameters(parameters, &error_message));
    EXPECT_FALSE(error_message.empty());

    FastMeasurements measurements;
    UpdateFastMeasurements(measurements, parameters, 9U);
    EXPECT_DOUBLE_EQ(measurements.output, 0.0);
    EXPECT_DOUBLE_EQ(measurements.state.raw_output, 0.0);
    EXPECT_EQ(measurements.state.diagnostic_code, 1);
    for (const float sample : measurements.samples) {
        EXPECT_TRUE(std::isfinite(sample));
    }
}

/// @brief 校验 NaN、无穷值和超范围数组修正量均被拒绝。
TEST(XcpSlaveDemoModelTest, RejectsNonFiniteAndOutOfRangeCalibration) {
    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    parameters.model.gain = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(ValidateCalibrationParameters(parameters));

    parameters = MakeDefaultCalibrationParameters();
    parameters.basic.real_64 = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(ValidateCalibrationParameters(parameters));

    parameters = MakeDefaultCalibrationParameters();
    parameters.corrections[3] = 5.01F;
    EXPECT_FALSE(ValidateCalibrationParameters(parameters));
}

/// @brief 校验慢速结构及定长数组从快速采样更新且保持计数安全。
TEST(XcpSlaveDemoModelTest, SlowUpdateCoversBasicTypesStructureAndArray) {
    FastMeasurements fast_measurements;
    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    UpdateFastMeasurements(fast_measurements, parameters, 42U);

    SlowMeasurements slow_measurements;
    UpdateSlowMeasurements(slow_measurements, fast_measurements);

    EXPECT_EQ(slow_measurements.basic.signed_8, -8);
    EXPECT_EQ(slow_measurements.basic.unsigned_8, 42U);
    EXPECT_EQ(slow_measurements.basic.signed_16, -958);
    EXPECT_EQ(slow_measurements.basic.unsigned_16, 42U);
    EXPECT_EQ(slow_measurements.basic.signed_32, 42);
    EXPECT_EQ(slow_measurements.basic.unsigned_32, 42U);
    EXPECT_FLOAT_EQ(slow_measurements.basic.real_32, 4.2F);
    EXPECT_NEAR(slow_measurements.basic.real_64, 4.2, 1e-6);
    EXPECT_EQ(slow_measurements.statistics.fast_cycle_count, 42U);
    EXPECT_EQ(slow_measurements.statistics.sample_count, 1U);
    EXPECT_NEAR(slow_measurements.statistics.mean_output, 4.2, 1e-6);
    EXPECT_EQ(slow_measurements.history[42U % kSampleCount], 42U);
}

/// @brief 校验饱和样本计数不会发生无符号回绕。
TEST(XcpSlaveDemoModelTest, SlowSampleCountSaturatesAtMaximum) {
    SlowMeasurements slow_measurements;
    slow_measurements.statistics.sample_count =
        std::numeric_limits<std::uint32_t>::max();
    slow_measurements.statistics.mean_output = 10.0;
    FastMeasurements fast_measurements;
    fast_measurements.tick = 11U;
    fast_measurements.output = 20.0;

    UpdateSlowMeasurements(slow_measurements, fast_measurements);

    EXPECT_EQ(slow_measurements.statistics.sample_count,
              std::numeric_limits<std::uint32_t>::max());
    EXPECT_GT(slow_measurements.statistics.mean_output, 10.0);
    EXPECT_LT(slow_measurements.statistics.mean_output, 20.0);
}

}  // namespace
}  // namespace xcp_slave_demo
