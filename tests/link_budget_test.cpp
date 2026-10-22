/**
 * @file link_budget_test.cpp
 * @brief 链路带宽预算（T-07, TL V1.5 §7）单元测试。
 *
 * 规范基线：`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`
 * §7.1（PDF 第 25-27 页）。
 */

#include "libxcp/link_budget.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

/// @brief 构造一份"参数完整"的基准配置：10 Mbit/s、12% 负载
LinkBudgetConfig CompleteConfig() {
    LinkBudgetConfig cfg;
    cfg.nominal_bit_rate_bps = 10'000'000ULL;  // 10 Mbit/s
    cfg.max_bus_load_percent = 12.0;           // 12%
    return cfg;
}

TEST(LinkBudgetFrameBytes, HeaderPlusPacketPlusTail) {
    // 8-bit：无 Tail，纯 4 字节 Header + Packet
    EXPECT_EQ(XcpFrameBytes(40, PacketAlignment::Bit8), 44U);
    EXPECT_EQ(XcpFrameBytes(17, PacketAlignment::Bit8), 21U);
    EXPECT_EQ(XcpFrameBytes(2, PacketAlignment::Bit8), 6U);
    // 16-bit：4+40=44 已对齐 -> tail 0；4+17=21 为奇数 -> tail 1
    EXPECT_EQ(XcpFrameBytes(40, PacketAlignment::Bit16), 44U);
    EXPECT_EQ(XcpFrameBytes(17, PacketAlignment::Bit16), 22U);
    // 32-bit：4+17=21 -> 补 3 到 24
    EXPECT_EQ(XcpFrameBytes(17, PacketAlignment::Bit32), 24U);
}

TEST(LinkBudget, SpecWorkedExampleEightBitMatchesFormula) {
    // 规范 §7.1 的 worked example：
    //   2ms EVENT：2 个 ODT，packet 40 / 17 字节
    //   100ms EVENT：6 个 ODT，packet 40,40,40,40,40 / 2 字节
    // 规范按 8-bit（无 Tail）计算。
    //
    // 注意：按规范自己给出的公式复算两项之和为 34760 byte/s，
    // 而原文该例的总计写 34780 byte/s（差 20，约 0.06%）。
    // 两项无法用整数分量凑出 34780（若 2ms 项 32500，则 100ms 项须为 2280，
    // 其分子 228 反推 header 为 4.333 字节，非整数；反向同理）。
    // 本用例以**公式**为准并固化 34760，差异已在实施记录中如实标注。
    std::vector<EventBudgetInput> events;
    events.push_back({"2ms", 0.002, {40, 17}});
    events.push_back({"100ms", 0.1, {40, 40, 40, 40, 40, 2}});

    const auto r = ComputeLinkBudget(CompleteConfig(), events,
                                     PacketAlignment::Bit8);
    EXPECT_NEAR(r.total_bytes_per_second, 34760.0, 0.5);
    // 允许量 = 10e6 / 8 * 12 / 100 = 150000 byte/s
    EXPECT_NEAR(r.allowed_bytes_per_second, 150000.0, 0.5);
    EXPECT_EQ(r.verdict, BudgetVerdict::Verified);
    EXPECT_TRUE(r.allowed);
    ASSERT_EQ(r.events.size(), 2U);
    EXPECT_NEAR(r.events[0].bytes_per_second, 32500.0, 0.5);
    EXPECT_NEAR(r.events[1].bytes_per_second, 2260.0, 0.5);
    // 8-bit 下不应有任何 Tail 字节
    EXPECT_EQ(r.events[0].total_tail_bytes, 0U);
    EXPECT_EQ(r.events[1].total_tail_bytes, 0U);
}

TEST(LinkBudget, TailBytesCountTowardBudgetUnderAlignment) {
    // 同样的事件在 32-bit 对齐下净字节数必须**增加**：预算须反映 Tail
    std::vector<EventBudgetInput> events;
    events.push_back({"2ms", 0.002, {40, 17}});

    const auto r8 = ComputeLinkBudget(CompleteConfig(), events,
                                      PacketAlignment::Bit8);
    const auto r32 = ComputeLinkBudget(CompleteConfig(), events,
                                       PacketAlignment::Bit32);
    EXPECT_GT(r32.total_bytes_per_second, r8.total_bytes_per_second);
    // 32-bit：40 字节 packet -> 44 已对齐 tail 0；17 字节 -> 21 补 3 到 24
    EXPECT_EQ(r32.events[0].total_tail_bytes, 3U);
    EXPECT_EQ(r32.events[0].total_xcp_bytes, 44U + 24U);
}

TEST(LinkBudget, MissingConfigDefaultsToAllowUnverified) {
    // 未提供配置：默认 AllowUnverified —— 保持 DAQ 可用但标记 Unverified
    std::vector<EventBudgetInput> events;
    events.push_back({"e", 0.01, {40}});

    const auto r = ComputeLinkBudget(std::nullopt, events, PacketAlignment::Bit8);
    EXPECT_EQ(r.verdict, BudgetVerdict::Unverified);
    EXPECT_TRUE(r.allowed);
    EXPECT_EQ(r.allowed_bytes_per_second, 0.0);
    // 说明文案必须点明"未配置 / Unverified"，便于上层诊断
    EXPECT_NE(r.message.find("Unverified"), std::string::npos);
}

TEST(LinkBudget, MissingConfigWithRejectPolicyBlocksDaq) {
    // Reject 仅在调用方显式提供配置并选择它时生效
    LinkBudgetConfig cfg;
    cfg.unknown_link_policy = UnknownLinkPolicy::Reject;
    std::vector<EventBudgetInput> events;
    events.push_back({"e", 0.01, {40}});

    const auto r = ComputeLinkBudget(cfg, events, PacketAlignment::Bit8);
    EXPECT_EQ(r.verdict, BudgetVerdict::Unverified);
    EXPECT_FALSE(r.allowed);  // 必须阻断
    EXPECT_NE(r.message.find("Reject"), std::string::npos);
}

TEST(LinkBudget, OverBudgetIsRejectedByDefault) {
    // 构造一个必然超预算的场景：周期极短、packet 很大
    LinkBudgetConfig cfg;
    cfg.nominal_bit_rate_bps = 1'000'000ULL;  // 1 Mbit/s
    cfg.max_bus_load_percent = 10.0;          // 允许 12500 byte/s
    std::vector<EventBudgetInput> events;
    events.push_back({"fast", 0.001, {200, 200, 200}});  // 1224 / 0.001

    const auto r = ComputeLinkBudget(cfg, events, PacketAlignment::Bit8);
    EXPECT_EQ(r.verdict, BudgetVerdict::OverBudget);
    EXPECT_FALSE(r.allowed);
    EXPECT_NEAR(r.allowed_bytes_per_second, 12500.0, 0.5);
    EXPECT_GT(r.total_bytes_per_second, r.allowed_bytes_per_second);
}

TEST(LinkBudget, AllowOverBudgetDowngradesToUnverifiedNotVerified) {
    // 显式 allow_over_budget 只能降为 Unverified，**不得**标记 Verified
    LinkBudgetConfig cfg;
    cfg.nominal_bit_rate_bps = 1'000'000ULL;
    cfg.max_bus_load_percent = 10.0;
    cfg.allow_over_budget = true;
    std::vector<EventBudgetInput> events;
    events.push_back({"fast", 0.001, {200, 200, 200}});

    const auto r = ComputeLinkBudget(cfg, events, PacketAlignment::Bit8);
    EXPECT_EQ(r.verdict, BudgetVerdict::Unverified);
    EXPECT_NE(r.verdict, BudgetVerdict::Verified);  // 硬约束
    EXPECT_TRUE(r.allowed);                         // 放行但不可证明
}

TEST(LinkBudget, UnknownCycleIsNotFabricated) {
    // 周期未知时不得假造周期；该项不参与速率计算，结论至少 Unverified
    std::vector<EventBudgetInput> events;
    events.push_back({"unknown_cycle", std::nullopt, {40}});

    const auto r = ComputeLinkBudget(CompleteConfig(), events,
                                     PacketAlignment::Bit8);
    EXPECT_EQ(r.verdict, BudgetVerdict::Unverified);
    EXPECT_FALSE(r.events[0].cycle_known);
    EXPECT_DOUBLE_EQ(r.events[0].bytes_per_second, 0.0);  // 未假造
    EXPECT_TRUE(r.allowed);
}

TEST(LinkBudget, UnknownCycleWithRejectPolicyBlocks) {
    std::vector<EventBudgetInput> events;
    events.push_back({"unknown_cycle", std::nullopt, {40}});
    auto cfg = CompleteConfig();
    cfg.unknown_link_policy = UnknownLinkPolicy::Reject;

    const auto r = ComputeLinkBudget(cfg, events, PacketAlignment::Bit8);
    EXPECT_FALSE(r.allowed);
}

TEST(LinkBudget, UnknownAlignmentPreventsVerified) {
    // alignment 未知 -> 无法推算 Tail -> 不可标记 Verified（方案 §1.9A.4）
    std::vector<EventBudgetInput> events;
    events.push_back({"e", 0.01, {40}});

    const auto r = ComputeLinkBudget(CompleteConfig(), events,
                                     PacketAlignment::Unknown);
    EXPECT_EQ(r.verdict, BudgetVerdict::Unverified);
    EXPECT_NE(r.verdict, BudgetVerdict::Verified);
}

TEST(LinkBudget, ActualAvailableRateCapsEffectiveRate) {
    // 已知实测可用率低于标称时，生效速率取较小值（降额场景）
    auto cfg = CompleteConfig();
    cfg.actual_available_bit_rate_bps = 1'200'000ULL;  // 1.2 Mbit/s
    std::vector<EventBudgetInput> events;
    events.push_back({"e", 0.01, {40}});

    const auto r = ComputeLinkBudget(cfg, events, PacketAlignment::Bit8);
    EXPECT_EQ(r.effective_bit_rate_bps, 1'200'000ULL);
    // 1.2e6 / 8 * 0.12 = 18000 byte/s
    EXPECT_NEAR(r.allowed_bytes_per_second, 18000.0, 0.5);
}

TEST(LinkBudget, ActualAvailableHigherThanNominalIsIgnored) {
    // 实测值高于标称不放大预算：MIN 语义
    auto cfg = CompleteConfig();
    cfg.actual_available_bit_rate_bps = 100'000'000ULL;
    std::vector<EventBudgetInput> events;
    events.push_back({"e", 0.01, {40}});

    const auto r = ComputeLinkBudget(cfg, events, PacketAlignment::Bit8);
    EXPECT_EQ(r.effective_bit_rate_bps, cfg.nominal_bit_rate_bps);
}

TEST(LinkBudget, BudgetIsSumOverAllEvents) {
    // 总额必须等于各 Event 之和（不能被取整或漏项掩盖）
    std::vector<EventBudgetInput> events;
    events.push_back({"a", 0.01, {40, 40}});
    events.push_back({"b", 0.02, {10, 20, 30}});

    const auto r = ComputeLinkBudget(CompleteConfig(), events,
                                     PacketAlignment::Bit8);
    double sum = 0.0;
    for (const auto& e : r.events) {
        sum += e.bytes_per_second;
    }
    EXPECT_DOUBLE_EQ(r.total_bytes_per_second, sum);
    // 逐项手算：(44+44)/0.01 + (14+24+34)/0.02 = 8800 + 3600
    EXPECT_NEAR(r.total_bytes_per_second, 12400.0, 0.5);
}

TEST(LinkBudget, EmptyEventListIsVerifiedWithCompleteParams) {
    // 无 Event：无流量需要预算，参数完整即 Verified
    const auto r = ComputeLinkBudget(CompleteConfig(), {},
                                     PacketAlignment::Bit8);
    EXPECT_EQ(r.verdict, BudgetVerdict::Verified);
    EXPECT_TRUE(r.allowed);
    EXPECT_DOUBLE_EQ(r.total_bytes_per_second, 0.0);
}

TEST(LinkBudget, ZeroNominalRateTreatedAsUnconfigured) {
    // 标称率 0 表示未配置（而不是"带宽为 0 所以必然超预算"）
    LinkBudgetConfig cfg;
    cfg.nominal_bit_rate_bps = 0;
    cfg.max_bus_load_percent = 12.0;
    std::vector<EventBudgetInput> events;
    events.push_back({"e", 0.01, {40}});

    const auto r = ComputeLinkBudget(cfg, events, PacketAlignment::Bit8);
    EXPECT_EQ(r.verdict, BudgetVerdict::Unverified);
    EXPECT_TRUE(r.allowed);
}

}  // namespace
}  // namespace calmcar::xcp