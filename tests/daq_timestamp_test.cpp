/**
 * @file daq_timestamp_test.cpp
 * @brief DaqTimestampConverter 单元测试（v0.6，L1）。
 *
 * 覆盖计划 §5.4 / v0.6 实现硬核对项：
 *   - 合法位宽 8/16/32 的模 2^bits 回卷延展（单帧至多 +1 周期，不猜丢帧）；
 *   - 非法位宽（24 等）→ 未知 → valid=false 且保留 raw（B-3 不猜）；
 *   - unit_ns=0（单位码表无权威来源，R13）→ valid=false 且保留 raw；
 *   - 回卷计数 WrapCount() 累计、Reset() 清基线不清历史计数；
 *   - 饱和乘法：extended×unit 越出 int64 时钳到最大值（防 UB）；
 *   - raw 持平（==prev）不判回卷。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>

#include "libxcp/daq/daq_timestamp.hpp"

namespace calmcar::xcp {
namespace {

TEST(DaqTimestampConverter, BaselineFirstCallNoWrap) {
  DaqTimestampConverter c(1, 32);
  const DaqTimestamp ts = c.Convert(1234);
  EXPECT_EQ(ts.raw, 1234U);
  EXPECT_TRUE(ts.valid);
  EXPECT_EQ(ts.value, std::chrono::nanoseconds(1234));
  EXPECT_EQ(c.WrapCount(), 0U);
}

TEST(DaqTimestampConverter, MonotonicNoWrap) {
  DaqTimestampConverter c(1, 8);
  EXPECT_EQ(c.Convert(10).value, std::chrono::nanoseconds(10));
  EXPECT_EQ(c.Convert(200).value, std::chrono::nanoseconds(200));
  EXPECT_EQ(c.Convert(255).value, std::chrono::nanoseconds(255));
  EXPECT_EQ(c.WrapCount(), 0U);
}

TEST(DaqTimestampConverter, EightBitWrapExtendsOnce) {
  DaqTimestampConverter c(1, 8);
  static_cast<void>(c.Convert(255));  // 建立基线
  const DaqTimestamp wrapped = c.Convert(1);
  // 255 → 1：跨过一次 2^8 边界，周期基线 +1 → extended = 256 + 1
  EXPECT_TRUE(wrapped.valid);
  EXPECT_EQ(wrapped.raw, 1U);
  EXPECT_EQ(wrapped.value, std::chrono::nanoseconds(257));
  EXPECT_EQ(c.WrapCount(), 1U);
}

TEST(DaqTimestampConverter, EightBitWrapAtZeroBoundary) {
  DaqTimestampConverter c(1, 8);
  static_cast<void>(c.Convert(255));
  const DaqTimestamp ts = c.Convert(0);
  EXPECT_EQ(ts.value, std::chrono::nanoseconds(256));
  EXPECT_EQ(c.WrapCount(), 1U);
}

TEST(DaqTimestampConverter, EqualRawIsNotWrap) {
  DaqTimestampConverter c(1, 8);
  static_cast<void>(c.Convert(255));
  const DaqTimestamp ts = c.Convert(255);  // 持平：raw < prev 不成立
  EXPECT_EQ(ts.value, std::chrono::nanoseconds(255));
  EXPECT_EQ(c.WrapCount(), 0U);
}

TEST(DaqTimestampConverter, SixteenBitTwoWrapsAccumulate) {
  DaqTimestampConverter c(1, 16);
  static_cast<void>(c.Convert(65535));    // 基线
  EXPECT_EQ(c.Convert(0).value, std::chrono::nanoseconds(65536));   // 第 1 次回卷
  static_cast<void>(c.Convert(40000));
  EXPECT_EQ(c.Convert(3).value, std::chrono::nanoseconds(131075));  // 第 2 次：2×65536+3
  EXPECT_EQ(c.WrapCount(), 2U);
}

TEST(DaqTimestampConverter, ThirtyTwoBitWrap) {
  DaqTimestampConverter c(1, 32);
  const std::uint64_t kMod = 1ULL << 32U;
  static_cast<void>(c.Convert(kMod - 1));  // 基线 = 0xFFFFFFFF
  const DaqTimestamp ts = c.Convert(5);
  EXPECT_EQ(ts.value, std::chrono::nanoseconds(static_cast<std::int64_t>(kMod + 5)));
  EXPECT_EQ(c.WrapCount(), 1U);
}

TEST(DaqTimestampConverter, UnitScalingAppliedToExtended) {
  // unit=10ns/tick：回卷延展后再乘单位
  DaqTimestampConverter c(10, 8);
  static_cast<void>(c.Convert(255));
  const DaqTimestamp ts = c.Convert(1);  // extended=257 → 2570ns
  EXPECT_EQ(ts.value, std::chrono::nanoseconds(2570));
}

TEST(DaqTimestampConverter, UnknownBitsInvalidKeepsRaw) {
  // 24 位宽非法：模未知 → 不解释（B-3），raw 保留
  DaqTimestampConverter c(1, 24);
  const DaqTimestamp ts = c.Convert(777);
  EXPECT_EQ(ts.raw, 777U);
  EXPECT_FALSE(ts.valid);
  EXPECT_EQ(ts.value.count(), 0);
  EXPECT_EQ(c.WrapCount(), 0U);
}

TEST(DaqTimestampConverter, UnknownUnitInvalidKeepsRaw) {
  DaqTimestampConverter c(0, 16);  // unit 未知（R13）
  static_cast<void>(c.Convert(10));
  const DaqTimestamp ts = c.Convert(5);  // 仍发生回卷判定与计数
  EXPECT_EQ(ts.raw, 5U);
  EXPECT_FALSE(ts.valid);
  EXPECT_EQ(ts.value.count(), 0);
  EXPECT_EQ(c.WrapCount(), 1U);
}

TEST(DaqTimestampConverter, ResetClearsBaselineKeepsWrapCount) {
  DaqTimestampConverter c(1, 8);
  static_cast<void>(c.Convert(255));
  static_cast<void>(c.Convert(1));  // 一次回卷
  EXPECT_EQ(c.WrapCount(), 1U);

  c.Reset();
  EXPECT_EQ(c.WrapCount(), 1U);  // 历史累计不清零（跨 Stop→Start 统计连续）

  // 基线已清：新流首帧重新建基线，raw 倒退也不判回卷
  const DaqTimestamp ts = c.Convert(3);
  EXPECT_TRUE(ts.valid);
  EXPECT_EQ(ts.value, std::chrono::nanoseconds(3));
  EXPECT_EQ(c.WrapCount(), 1U);

  // 之后正常回卷：cycles 从 0 重新起算 → 256 + 1
  static_cast<void>(c.Convert(255));
  const DaqTimestamp again = c.Convert(1);
  EXPECT_EQ(again.value, std::chrono::nanoseconds(257));
  EXPECT_EQ(c.WrapCount(), 2U);
}

TEST(DaqTimestampConverter, SaturateToMaxInt64) {
  // unit 巨大：extended=1 不越界，extended=2 越界 → 钳到 int64 最大值
  constexpr std::uint64_t kHuge =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  DaqTimestampConverter c(kHuge, 8);
  const DaqTimestamp first = c.Convert(1);
  EXPECT_TRUE(first.valid);
  EXPECT_EQ(first.value.count(),
            std::numeric_limits<std::int64_t>::max());  // 1×kHuge 恰不越界
  const DaqTimestamp second = c.Convert(0);              // 回卷：extended=256
  EXPECT_TRUE(second.valid);
  EXPECT_EQ(second.value.count(),
            std::numeric_limits<std::int64_t>::max());  // 饱和钳位
}

}  // namespace
}  // namespace calmcar::xcp
