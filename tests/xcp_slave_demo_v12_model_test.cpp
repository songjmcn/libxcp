/**
 * @file xcp_slave_demo_v12_model_test.cpp
 * @brief XCPlite V1.2 从站示例的**模型层**单元测试（计划 §8：只做模型层单测）。
 *
 * @note 本文件只包含 `examples/xcp_slave_demo_v12/slave_model.hpp`，不包含任何
 *       XCPlite / libxcp 头文件，因此不链接 `xcplite_v12`，避免与 `XcpliteIntegration`
 *       已链接的 V2.2 `xcplite` 产生同名符号冲突（计划 §8 的接线约束）。
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "examples/xcp_slave_demo_v12/slave_model.hpp"

namespace xcp_slave_demo_v12 {
namespace {

/// @brief 断言浮点近似相等（模型全部是确定性算术，容差只需覆盖 float 舍入）。
void ExpectNear(float actual, double expected, double tolerance = 1e-5) {
    EXPECT_NEAR(static_cast<double>(actual), expected, tolerance);
}

/**
 * @brief 出厂默认标定参数必须满足"不改变波形"的中性取值。
 *
 * 动机：`CalibrationParameters` 的成员默认值同时被 A2L 参考页使用，
 * 一旦默认值带增益或偏置，主站读到的最小系统波形就不是原始正弦。
 */
TEST(XcpSlaveV12ModelTest, DefaultCalibrationIsNeutral) {
    const CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    EXPECT_FLOAT_EQ(parameters.gain, 1.0F);
    EXPECT_FLOAT_EQ(parameters.offset, 0.0F);
    EXPECT_FLOAT_EQ(parameters.limit, 100.0F);
    EXPECT_EQ(parameters.run, 1U);
    for (std::size_t index = 0; index < kCorrectionCount; ++index) {
        EXPECT_FLOAT_EQ(parameters.corrections[index], 0.0F) << "index=" << index;
    }
}

/**
 * @brief 构造即推进一个快速周期，主站在 DAQ 启动前就能读到有效数据。
 */
TEST(XcpSlaveV12ModelTest, ConstructionProducesFirstFrame) {
    const SlaveModel model;
    EXPECT_EQ(model.FastTick(), 1U);
    EXPECT_EQ(model.Fast().counter, 1U);
    // 相位 1：sine = sin(2π/64)，方波在上半周期为 +1（gain=1、offset=0）。
    const double expected_sine =
        std::sin(2.0 * 3.14159265358979323846 / static_cast<double>(kPhaseCount));
    ExpectNear(model.Fast().sine, expected_sine);
    EXPECT_FLOAT_EQ(model.Fast().square, 1.0F);
    // 锯齿相位 1 -> 2*1/63 - 1。
    ExpectNear(model.Fast().saw, 2.0 / 63.0 - 1.0);
    // 尚未推进慢速周期。
    EXPECT_EQ(model.SlowTick(), 0U);
    EXPECT_EQ(model.Slow().event_tick, 0U);
}

/**
 * @brief 快速计数单调递增，且相位按 64 点回绕。
 *
 * 回绕点很关键：正弦在 tick=64 时回到相位 0（sine=0），用于验证 A2L 中
 * `kPhaseCount` 的语义没有写错（写错会让 DAQ 波形看起来"跳变"）。
 */
TEST(XcpSlaveV12ModelTest, FastTickAdvancesAndPhaseWraps) {
    SlaveModel model;
    // 构造已推进到 tick=1，再推进 63 次正好落在 tick=64，即相位 0（回绕点）。
    for (std::uint32_t step = 0U; step < kPhaseCount - 1U; ++step) {
        model.StepFast();
    }
    EXPECT_EQ(model.FastTick(), kPhaseCount);
    EXPECT_EQ(model.Fast().counter, kPhaseCount);
    // 相位回到 0：sine 应为 0，方波回到上半周期（+1）。
    ExpectNear(model.Fast().sine, 0.0);
    EXPECT_FLOAT_EQ(model.Fast().square, 1.0F);
}

/**
 * @brief 方波在中点翻转符号，验证半周期判据。
 */
TEST(XcpSlaveV12ModelTest, SquareFlipsAtHalfPeriod) {
    SlaveModel model;  // 已推进到 tick=1
    for (std::uint32_t step = 1U; step < kPhaseCount / 2U; ++step) {
        model.StepFast();
    }
    // 此时 tick=32 -> 相位 32，属于下半周期。
    EXPECT_EQ(model.FastTick(), kPhaseCount / 2U);
    EXPECT_FLOAT_EQ(model.Fast().square, -1.0F);
}

/**
 * @brief `run=0` 必须冻结波形推进（主站用它在标定联调时"定格"观察）。
 */
TEST(XcpSlaveV12ModelTest, RunZeroFreezesWaveform) {
    SlaveModel model;
    model.StepFast();
    const std::uint32_t tick_before = model.FastTick();
    const float sine_before = model.Fast().sine;

    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    parameters.run = 0U;
    model.SetCalibration(parameters);
    for (int step = 0; step < 5; ++step) {
        model.StepFast();
    }

    EXPECT_EQ(model.FastTick(), tick_before);
    EXPECT_EQ(model.Fast().counter, tick_before);
    EXPECT_FLOAT_EQ(model.Fast().sine, sine_before);
}

/**
 * @brief gain/offset 必须被算法真实消费（否则标定只验证"写成功"）。
 */
TEST(XcpSlaveV12ModelTest, GainAndOffsetShapeWaveform) {
    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    parameters.gain = 2.0F;
    parameters.offset = 0.5F;
    SlaveModel model(parameters);

    const double raw_sine =
        std::sin(2.0 * 3.14159265358979323846 / static_cast<double>(kPhaseCount));
    ExpectNear(model.Fast().sine, 2.0 * raw_sine + 0.5);
    // 方波：gain*±1 + offset。
    EXPECT_FLOAT_EQ(model.Fast().square, 2.5F);
}

/**
 * @brief limit 必须对称限幅（abs(limit) 作界），且负的 limit 不应放行。
 */
TEST(XcpSlaveV12ModelTest, LimitClampsSymmetrically) {
    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    parameters.gain = 10.0F;
    parameters.limit = 0.25F;
    SlaveModel model(parameters);

    EXPECT_LE(model.Fast().sine, 0.25F);
    EXPECT_GE(model.Fast().sine, -0.25F);
    EXPECT_FLOAT_EQ(model.Fast().square, 0.25F);

    // 负 limit 取绝对值限幅（算法用 fabs），但诊断码应标记参数异常。
    parameters.limit = -0.25F;
    model.SetCalibration(parameters);
    model.StepSlow();
    EXPECT_FLOAT_EQ(model.Fast().square, 0.25F);
    EXPECT_EQ(model.Slow().stats.last_diagnostic, -1);
}

/**
 * @brief corrections[i] 必须逐通道进入 samples[i]，这是数组型标定的可观测证据。
 */
TEST(XcpSlaveV12ModelTest, CorrectionsReachSampleArray) {
    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    parameters.corrections[0] = 1.5F;
    parameters.corrections[7] = -2.5F;
    SlaveModel model(parameters);

    ExpectNear(model.Fast().samples[0], static_cast<double>(model.Fast().sine) + 1.5);
    ExpectNear(model.Fast().samples[7], static_cast<double>(model.Fast().sine) - 2.5);
    // 未设置修正量的通道与正弦保持一致。
    ExpectNear(model.Fast().samples[3], model.Fast().sine);
}

/**
 * @brief 慢速块的八种基础类型取值必须由慢速周期数确定性派生。
 */
TEST(XcpSlaveV12ModelTest, SlowBlockDerivesBasicTypesFromTick) {
    SlaveModel model;
    model.StepFast();
    model.StepSlow();

    const BasicMeasurementValues& basic = model.Slow().basic;
    EXPECT_EQ(model.SlowTick(), 1U);
    EXPECT_EQ(basic.u8, 1U);
    EXPECT_EQ(basic.s8, -1);
    EXPECT_EQ(basic.u16, 3U);
    EXPECT_EQ(basic.s16, -1);
    EXPECT_EQ(basic.u32, 1000U);
    EXPECT_EQ(basic.s32, -7);
    EXPECT_FLOAT_EQ(basic.f32, model.Fast().sine);
    EXPECT_DOUBLE_EQ(basic.f64, static_cast<double>(model.Fast().sine));
    EXPECT_EQ(model.Slow().event_tick, 1U);
}

/**
 * @brief 慢速统计的均值/计数必须自洽，且 history 环形缓冲最新值在末位。
 */
TEST(XcpSlaveV12ModelTest, SlowStatisticsAndHistoryAreConsistent) {
    SlaveModel model;
    double sine_sum = 0.0;
    for (std::uint32_t step = 0; step < 4U; ++step) {
        model.StepFast();
        sine_sum += static_cast<double>(model.Fast().sine);
        model.StepSlow();
    }

    EXPECT_EQ(model.Slow().stats.fast_cycles, model.FastTick());
    EXPECT_EQ(model.Slow().stats.slow_cycles, model.SlowTick());
    ExpectNear(model.Slow().stats.mean_output, sine_sum / 4.0, 1e-4);
    EXPECT_EQ(model.Slow().stats.last_diagnostic, 0);
    // history[7] 恒为最近一次快速正弦样本。
    EXPECT_FLOAT_EQ(model.Slow().history[kSampleCount - 1U], model.Fast().sine);
}

/**
 * @brief 两次相同调用序列必须得到逐字节相同的结果（DAQ 与 POLL 可对齐断言的前提）。
 */
TEST(XcpSlaveV12ModelTest, SameCallSequenceIsDeterministic) {
    SlaveModel first;
    SlaveModel second;
    for (int step = 0; step < 10; ++step) {
        first.StepFast();
        second.StepFast();
        if (step % 2 == 0) {
            first.StepSlow();
            second.StepSlow();
        }
    }

    EXPECT_EQ(first.FastTick(), second.FastTick());
    EXPECT_EQ(first.SlowTick(), second.SlowTick());
    EXPECT_EQ(first.Fast().counter, second.Fast().counter);
    EXPECT_FLOAT_EQ(first.Fast().sine, second.Fast().sine);
    EXPECT_FLOAT_EQ(first.Fast().saw, second.Fast().saw);
    EXPECT_EQ(first.Fast().samples[5], second.Fast().samples[5]);
    EXPECT_DOUBLE_EQ(first.Slow().basic.f64, second.Slow().basic.f64);
    EXPECT_FLOAT_EQ(first.Slow().stats.mean_output, second.Slow().stats.mean_output);
}

/**
 * @brief 标定参数快照的读回应等同于最后一次写入（主站在读回时观察一致）。
 */
TEST(XcpSlaveV12ModelTest, CalibrationSnapshotRoundTrips) {
    SlaveModel model;
    CalibrationParameters parameters = MakeDefaultCalibrationParameters();
    parameters.gain = 3.5F;
    parameters.offset = -1.25F;
    parameters.limit = 42.0F;
    parameters.run = 1U;
    parameters.corrections[2] = 0.125F;
    model.SetCalibration(parameters);

    const CalibrationParameters& snapshot = model.Calibration();
    EXPECT_FLOAT_EQ(snapshot.gain, 3.5F);
    EXPECT_FLOAT_EQ(snapshot.offset, -1.25F);
    EXPECT_FLOAT_EQ(snapshot.limit, 42.0F);
    EXPECT_FLOAT_EQ(snapshot.corrections[2], 0.125F);
}

}  // namespace
}  // namespace xcp_slave_demo_v12