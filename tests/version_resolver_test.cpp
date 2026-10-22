/**
 * @file version_resolver_test.cpp
 * @brief 版本裁决器的回归用例（批次 B1）。
 *
 * @details 覆盖方案 B1 的核心硬约束：
 *          1. PL 与 TL 独立维度，不得互相推导；
 *          2. Minor 未确认时进入 `1.x-conservative`，**不得伪造 Minor**；
 *          3. **超时 ≠ 老版本**（方案 B1 明文要求）；
 *          4. 上报版本高于支持上限时封顶，且 `Capped` 作为状态标志保留原值；
 *          5. 冲突必须始终被记录（用户裁定 m03674：不提供静默模式）；
 *          6. 证据优先级：GET_VERSION > 调用方配置 > A2L > CONNECT 主版本。
 *
 * @note 本文件只测裁决器纯函数，不构造真实会话——裁决逻辑与传输解耦正是
 *       `VersionResolver` 被设计为无状态的原因。
 */

#include <gtest/gtest.h>

#include "libxcp/version_resolver.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 构造一个「GET_VERSION 成功」的探测结果
CommandExecutor::GetVersionProbe AvailableProbe(std::uint8_t pl_major,
                                                std::uint8_t pl_minor,
                                                std::uint8_t tl_major,
                                                std::uint8_t tl_minor) {
    CommandExecutor::GetVersionProbe probe;
    probe.outcome = CommandExecutor::GetVersionOutcome::Available;
    GetVersionResponse response;
    response.protocol_major = pl_major;
    response.protocol_minor = pl_minor;
    response.transport_major = tl_major;
    response.transport_minor = tl_minor;
    probe.response = response;
    return probe;
}

/// @brief 构造一个「GET_VERSION 不可用」的探测结果
CommandExecutor::GetVersionProbe UnavailableProbe(
    CommandExecutor::GetVersionOutcome outcome) {
    CommandExecutor::GetVersionProbe probe;
    probe.outcome = outcome;
    probe.response.reset();
    return probe;
}

// ---------------------------------------------------------------------------
// 1. PL / TL 独立维度
// ---------------------------------------------------------------------------

/// @brief PL 与 TL 必须各自取值，不得由一方推导另一方
TEST(VersionResolverTest, ProtocolAndTransportAreIndependentDimensions) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    // 刻意构造"协议层高、传输层低"的非对称组合：若实现错误地用某一方
    // 覆盖另一方，本用例会立刻失败。
    inputs.probe = AvailableProbe(1, 5, 1, 2);

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_EQ(result.protocol.evidence.reported, (XcpProtocolVersion{1, 5}));
    EXPECT_EQ(result.transport.evidence.reported, (XcpProtocolVersion{1, 2}));
    EXPECT_EQ(result.protocol.effective, (XcpProtocolVersion{1, 5}));
    EXPECT_EQ(result.transport.effective, (XcpProtocolVersion{1, 2}));
    EXPECT_NE(result.protocol.effective, result.transport.effective);
}

// ---------------------------------------------------------------------------
// 2. Minor 未确认 → 1.x-conservative，不得伪造 Minor
// ---------------------------------------------------------------------------

/// @brief 仅有 CONNECT 主版本时进入保守状态，且不得谎报 Minor
TEST(VersionResolverTest, ConnectOnlyYieldsConservativeWithoutFabricatingMinor) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = UnavailableProbe(
        CommandExecutor::GetVersionOutcome::UnimplementedCommand);

    const auto result = VersionResolver::Resolve(inputs);

    // 证据层：来源是 ConnectMajor、置信度是 MajorOnly、Minor 明确未确认
    EXPECT_EQ(result.protocol.evidence.source, VersionSource::ConnectMajor);
    EXPECT_EQ(result.protocol.evidence.confidence,
              VersionConfidence::MajorOnly);
    EXPECT_FALSE(result.protocol.evidence.minor_confirmed);
    EXPECT_FALSE(result.get_version_available);

    // 置信层：主版本 1 可用，但 Minor 字段恒为 0——**这是占位而非真实值**
    EXPECT_TRUE(result.protocol.evidence.IsAvailable());
    EXPECT_EQ(result.protocol.evidence.reported.major, 1U);
    EXPECT_EQ(result.protocol.evidence.reported.minor, 0U);

    // 裁决层：保守标志置位，生效版本取支持范围下限
    EXPECT_TRUE(result.protocol.conservative);
    EXPECT_EQ(result.protocol.effective, kProtocolVersion13);
    EXPECT_TRUE(result.transport.conservative);
}

/// @brief 保守状态不得被当作"对端就是 V1.3"（超时 ≠ 老版本）
TEST(VersionResolverTest, TimeoutIsNotTreatedAsOldVersion) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = UnavailableProbe(
        CommandExecutor::GetVersionOutcome::TimedOut);

    const auto result = VersionResolver::Resolve(inputs);

    // 关键断言：timeout 绝不能升级为"确认的老版本"。
    // 来源仍是 ConnectMajor（弱证据），置信度仍是 MajorOnly，
    // 而不是 RuntimeConfirmed + 精确 V1.3。
    EXPECT_EQ(result.protocol.evidence.source, VersionSource::ConnectMajor);
    EXPECT_NE(result.protocol.evidence.confidence,
              VersionConfidence::RuntimeConfirmed);
    EXPECT_FALSE(result.protocol.evidence.minor_confirmed);
    // 保守标志表达的是"能力集按交集执行"，不是"对端版本低"
    EXPECT_TRUE(result.protocol.conservative);
}

/// @brief 子命令未实现与命令未实现同等对待（都属"不可用"）
TEST(VersionResolverTest, UnimplementedSubcommandIsAlsoUnavailable) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = UnavailableProbe(
        CommandExecutor::GetVersionOutcome::UnimplementedSubcommand);

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_FALSE(result.get_version_available);
    EXPECT_TRUE(result.protocol.conservative);
    EXPECT_EQ(result.protocol.evidence.source, VersionSource::ConnectMajor);
}

/// @brief 完全没有证据（未连接）不得伪造任何版本
TEST(VersionResolverTest, NoEvidenceYieldsUnknownWithoutGuessing) {
    VersionResolver::Inputs inputs;  // 全部默认：无 CONNECT 主版本、无探测
    inputs.probe = UnavailableProbe(
        CommandExecutor::GetVersionOutcome::TimedOut);

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_EQ(result.protocol.evidence.source, VersionSource::Unknown);
    EXPECT_EQ(result.protocol.evidence.confidence, VersionConfidence::Unknown);
    EXPECT_FALSE(result.protocol.evidence.IsAvailable());
    // 无证据时既不能声称保守（保守也需已知 Major），也不能声称精确
    EXPECT_FALSE(result.protocol.conservative);
    EXPECT_FALSE(result.protocol.effective.IsValid());
}

// ---------------------------------------------------------------------------
// 3. 封顶：Capped 是状态标志而非伪造来源
// ---------------------------------------------------------------------------

/// @brief 上报 1.6（高于上限 1.5）时保留原值并封顶
TEST(VersionResolverTest, VersionAboveCapIsCappedWithReportedValuePreserved) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = AvailableProbe(1, 6, 1, 9);

    const auto result = VersionResolver::Resolve(inputs);

    // reported 保留对端原话（不得改写）
    EXPECT_EQ(result.protocol.evidence.reported, (XcpProtocolVersion{1, 6}));
    EXPECT_TRUE(result.protocol.evidence.capped);
    // effective 被压到上限
    EXPECT_EQ(result.protocol.effective, kProtocolVersion15);
    // 来源仍是 GetVersion（**未被伪造成"Configured/Unknown"**），只是加了 capped
    EXPECT_EQ(result.protocol.evidence.source, VersionSource::GetVersion);
    EXPECT_TRUE(result.protocol.evidence.minor_confirmed);

    EXPECT_EQ(result.transport.evidence.reported, (XcpProtocolVersion{1, 9}));
    EXPECT_TRUE(result.transport.evidence.capped);
    EXPECT_EQ(result.transport.effective, kTransportVersion15);

    // 封顶必须产生可见冲突记录
    ASSERT_FALSE(result.conflicts.empty());
}

/// @brief 恰好等于上限时**不**封顶（边界，不能把合法值误判）
TEST(VersionResolverTest, VersionExactlyAtCapIsNotCapped) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = AvailableProbe(1, 5, 1, 5);

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_FALSE(result.protocol.evidence.capped);
    EXPECT_FALSE(result.transport.evidence.capped);
    EXPECT_EQ(result.protocol.effective, (XcpProtocolVersion{1, 5}));
}

/// @brief 跨主版本（2.0）同样被封顶
TEST(VersionResolverTest, HigherMajorVersionIsAlsoCapped) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 2;
    inputs.connect_transport_major = 2;
    inputs.probe = AvailableProbe(2, 0, 2, 0);

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_TRUE(result.protocol.evidence.capped);
    EXPECT_EQ(result.protocol.effective, kProtocolVersion15);
    EXPECT_EQ(result.protocol.evidence.reported, (XcpProtocolVersion{2, 0}));
}

// ---------------------------------------------------------------------------
// 4. 证据优先级与冲突记录
// ---------------------------------------------------------------------------

/// @brief GET_VERSION 优先于调用方配置，但冲突必须被记录
TEST(VersionResolverTest, RuntimeWinsOverConfiguredAndConflictIsRecorded) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = AvailableProbe(1, 4, 1, 4);
    inputs.policy.protocol_version = XcpProtocolVersion{1, 3};

    const auto result = VersionResolver::Resolve(inputs);

    // 采用运行时值
    EXPECT_EQ(result.protocol.evidence.reported, (XcpProtocolVersion{1, 4}));
    EXPECT_EQ(result.protocol.evidence.source, VersionSource::GetVersion);
    // 冲突必须可见（不提供静默模式）
    bool found = false;
    for (const std::string& c : result.conflicts) {
        if (c.find("调用方配置") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "运行时与配置不一致时必须记录冲突";
}

/// @brief GET_VERSION 与 A2L 声明冲突时也须记录
TEST(VersionResolverTest, RuntimeVersusA2lConflictIsRecorded) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = AvailableProbe(1, 5, 1, 5);
    inputs.a2l_protocol_version = XcpProtocolVersion{1, 3};

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_EQ(result.protocol.evidence.reported, (XcpProtocolVersion{1, 5}));
    bool found = false;
    for (const std::string& c : result.conflicts) {
        if (c.find("A2L") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "运行时与 A2L 不一致时必须记录冲突";
}

/// @brief 无冲突时冲突列表必须为空（不能凭空报冲突）
TEST(VersionResolverTest, ConsistentEvidenceProducesNoConflict) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = AvailableProbe(1, 4, 1, 4);
    inputs.policy.protocol_version = XcpProtocolVersion{1, 4};
    inputs.a2l_protocol_version = XcpProtocolVersion{1, 4};

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_TRUE(result.conflicts.empty()) << "一致时不应产生冲突记录";
}

/// @brief 探测不可用时回退到调用方配置（Minor 视为已确认）
TEST(VersionResolverTest, FallsBackToConfiguredWhenProbeUnavailable) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = UnavailableProbe(
        CommandExecutor::GetVersionOutcome::TimedOut);
    inputs.policy.protocol_version = XcpProtocolVersion{1, 3};

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_EQ(result.protocol.evidence.source, VersionSource::Configured);
    EXPECT_EQ(result.protocol.evidence.confidence,
              VersionConfidence::Configured);
    EXPECT_TRUE(result.protocol.evidence.minor_confirmed);
    EXPECT_EQ(result.protocol.effective, (XcpProtocolVersion{1, 3}));
    // 配置来源已确认 Minor，不应进入保守状态
    EXPECT_FALSE(result.protocol.conservative);
}

/// @brief 探测不可用时回退到 A2L 声明（静态证据，弱于运行时）
TEST(VersionResolverTest, FallsBackToA2lWhenNothingElseAvailable) {
    VersionResolver::Inputs inputs;
    inputs.connect_protocol_major = 1;
    inputs.connect_transport_major = 1;
    inputs.probe = UnavailableProbe(
        CommandExecutor::GetVersionOutcome::UnimplementedCommand);
    inputs.a2l_protocol_version = XcpProtocolVersion{1, 4};

    const auto result = VersionResolver::Resolve(inputs);

    EXPECT_EQ(result.protocol.evidence.source, VersionSource::A2l);
    EXPECT_EQ(result.protocol.evidence.confidence,
              VersionConfidence::StaticEvidence);
    EXPECT_EQ(result.protocol.effective, (XcpProtocolVersion{1, 4}));
}

// ---------------------------------------------------------------------------
// 5. 版本类型自身行为
// ---------------------------------------------------------------------------

/// @brief 版本比较与有效性判定
TEST(XcpProtocolVersionTest, ComparisonAndValidity) {
    EXPECT_TRUE((XcpProtocolVersion{1, 3}).IsValid());
    EXPECT_FALSE((XcpProtocolVersion{0, 0}).IsValid());

    // 字典序：先比 Major
    EXPECT_LT((XcpProtocolVersion{1, 5}), (XcpProtocolVersion{2, 0}));
    // 同 Major 再比 Minor
    EXPECT_LT((XcpProtocolVersion{1, 3}), (XcpProtocolVersion{1, 4}));
    EXPECT_FALSE((XcpProtocolVersion{1, 4}) < (XcpProtocolVersion{1, 4}));
}

/// @brief 版本渲染为 X.Y
TEST(XcpProtocolVersionTest, ToStringRendersMajorDotMinor) {
    EXPECT_EQ((XcpProtocolVersion{1, 5}).ToString(), "1.5");
    EXPECT_EQ((XcpProtocolVersion{2, 0}).ToString(), "2.0");
}

/// @brief 策略自检拒绝自相矛盾配置
TEST(VersionPolicyTest, ValidateRejectsContradiction) {
    VersionPolicy policy;
    policy.probe_get_version = true;
    policy.allow_version_probe_roundtrip = false;
    EXPECT_FALSE(policy.Validate().empty())
        << "要求探测却禁止探测往返必须被拒绝";
}

/// @brief 策略自检拒绝 0.x 版本声明
TEST(VersionPolicyTest, ValidateRejectsZeroMajorVersion) {
    VersionPolicy policy;
    policy.protocol_version = XcpProtocolVersion{0, 9};
    EXPECT_FALSE(policy.Validate().empty()) << "0.x 不是合法版本声明";
}

/// @brief 默认策略必须合法（默认路径不能自己就把自己拒了）
TEST(VersionPolicyTest, DefaultPolicyIsValid) {
    EXPECT_TRUE(VersionPolicy{}.Validate().empty());
}

}  // namespace
}  // namespace calmcar::xcp