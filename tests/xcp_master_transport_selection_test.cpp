/**
 * @file xcp_master_transport_selection_test.cpp
 * @brief `tools/xcp_master/transport_selection.hpp` 的规则级单元测试。
 *
 * 依据 `code-plan/TCP传输_实施计划_B-T1至B-T5.md` §8.1 与 §9.4 编写（批次 B-T5）。
 *
 * @par 测试策略
 *   解析层之所以被刻意抽成纯函数，就是为了让 §8.1 的九条规则可以**逐条**
 *   被钉住：每个用例只改变一个输入维度，断言"选了谁、来源是什么、冲突是否
 *   报错"。任何依赖真实 socket 的验证都放到 `xcplite_tcp_interop_test.cpp`。
 */

#include "libxcp/transport_selection.hpp"

// CLI 文本层（选项解析与摘要渲染）仍留在工具侧，验收时与库层一起钉住。
#include "tools/xcp_master/cli_selection.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using calmcar::xcp::PacketAlignment;
using calmcar::xcp::TcpWireProfile;
using calmcar::xcp::XcpProtocolVersion;
using calmcar::xcp::A2lEndpointInstance;
using calmcar::xcp::A2lTransportFacts;
using calmcar::xcp::AlignmentSource;
using calmcar::xcp::EndpointSource;
using calmcar::xcp::ResolveTransportSelection;
using calmcar::xcp::ResolvedTransportSelection;
using calmcar::xcp::TransportKind;
using calmcar::xcp::TransportSelectionRequest;
using calmcar::xcp::WireProfileSource;

/// @brief 构造一个"UDP + CLI 端点齐全"的基线请求
TransportSelectionRequest MakeUdpRequest() {
    TransportSelectionRequest request;
    request.transport = TransportKind::Udp;
    request.host = std::string{"127.0.0.1"};
    request.port = static_cast<std::uint16_t>(5555);
    return request;
}

/// @brief 构造一个"TCP + CLI 端点 + V1.5 Profile"的基线请求
TransportSelectionRequest MakeTcpV15Request() {
    TransportSelectionRequest request;
    request.transport = TransportKind::Tcp;
    request.tcp_profile = TcpWireProfile::AsamV15;
    request.host = std::string{"127.0.0.1"};
    request.port = static_cast<std::uint16_t>(5555);
    return request;
}

/// @brief 便捷封装：调用解析函数并返回结果（失败时用断言报告错误文本）
ResolvedTransportSelection Resolve(const TransportSelectionRequest& request,
                                   const A2lTransportFacts& facts) {
    std::string error;
    const auto resolved = ResolveTransportSelection(request, facts, &error);
    EXPECT_TRUE(resolved.has_value()) << "意外失败: " << error;
    if (!resolved.has_value()) {
        return ResolvedTransportSelection{};
    }
    return *resolved;
}

/// @brief 便捷封装：断言解析失败并返回错误文本
std::string ResolveExpectFailure(const TransportSelectionRequest& request,
                                 const A2lTransportFacts& facts) {
    std::string error;
    const auto resolved = ResolveTransportSelection(request, facts, &error);
    EXPECT_FALSE(resolved.has_value()) << "本应失败却成功";
    EXPECT_FALSE(error.empty()) << "失败时必须给出可读原因";
    return error;
}

/** @brief 显式 TCP 端点不匹配或多实例时不得借用首端点证据。 */
TEST(TransportSelection, DiscardsUnmatchedTcpEndpointEvidence) {
    auto request = MakeTcpV15Request();
    A2lTransportFacts facts;
    facts.declared = true;
    facts.host = "127.0.0.2";
    facts.port = 5555;
    facts.instance_count = 1;
    facts.instances = {A2lEndpointInstance{facts.host, facts.port, 16, 0x0105}};
    facts.alignment_declared = true;
    facts.packet_alignment_bits = 16;
    facts.transport_version = XcpProtocolVersion{1, 5};
    facts.profile_hint = "first endpoint";
    EXPECT_TRUE(calmcar::xcp::DiscardUnmatchedTcpEndpointEvidence(request, facts));
    EXPECT_FALSE(facts.alignment_declared);
    EXPECT_FALSE(facts.transport_version);
    EXPECT_TRUE(facts.profile_hint.empty());
    EXPECT_NE(ResolveExpectFailure(request, facts).find("--packet-alignment"), std::string::npos);

    facts.host = *request.host;
    facts.instances = {A2lEndpointInstance{facts.host, facts.port, 8, 0x0104}};
    facts.transport_version = XcpProtocolVersion{1, 4};
    EXPECT_FALSE(calmcar::xcp::DiscardUnmatchedTcpEndpointEvidence(request, facts));
    EXPECT_TRUE(facts.transport_version);
    facts.instance_count = 2;
    facts.instances.push_back(facts.instances.front());
    EXPECT_TRUE(calmcar::xcp::DiscardUnmatchedTcpEndpointEvidence(request, facts));
    EXPECT_FALSE(facts.transport_version);
}

/** @brief 不可信 TCP 对齐不能被解析层调用方意外使用。 */
TEST(TransportSelection, RejectsUnverifiedTcpAlignment) {
    auto request = MakeTcpV15Request();
    A2lTransportFacts facts;
    facts.alignment_declared = true;
    facts.packet_alignment_bits = 16;
    facts.instance_alignment_unverified = true;
    const auto error = ResolveExpectFailure(request, facts);
    EXPECT_NE(error.find("未核验"), std::string::npos);
    EXPECT_NE(error.find("--packet-alignment"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 规则 9：UDP 默认行为不得回归
// ---------------------------------------------------------------------------

/**
 * @brief 规则 9：不给 `--transport` 时仍按 UDP 处理，且 CLI 端点原样生效。
 * @details B-T5 之前 UDP 是唯一通路，本次改动不得悄悄改变其默认语义。
 */
TEST(TransportSelection, DefaultsToUdpAndKeepsCliEndpoint) {
    const auto resolved = Resolve(MakeUdpRequest(), {});

    EXPECT_EQ(resolved.transport, TransportKind::Udp);
    EXPECT_EQ(resolved.host, "127.0.0.1");
    EXPECT_EQ(resolved.port, 5555U);
    EXPECT_EQ(resolved.endpoint_source, EndpointSource::Cli);
    // UDP 不存在 Wire Profile：必须明确标记为不适用，而不是留一个假默认值。
    EXPECT_EQ(resolved.wire_profile, TcpWireProfile::Unknown);
    EXPECT_EQ(resolved.wire_profile_source, WireProfileSource::NotApplicable);
    // A2L 未声明对齐时回落到 8 bit，并标注来源为 Profile 默认值。
    EXPECT_EQ(resolved.alignment, PacketAlignment::Bit8);
    EXPECT_EQ(resolved.alignment_source, AlignmentSource::ProfileDefault);
}

/**
 * @brief 规则 2：A2L 显式声明 PACKET_ALIGNMENT 时 UDP 应采用该位宽。
 */
TEST(TransportSelection, UdpTakesExplicitA2lAlignment) {
    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 5555;
    facts.host = "192.168.0.10";
    facts.packet_alignment_bits = 32;
    facts.alignment_declared = true;
    facts.instance_count = 1;

    const auto resolved = Resolve(MakeUdpRequest(), facts);

    EXPECT_EQ(resolved.alignment, PacketAlignment::Bit32);
    EXPECT_EQ(resolved.alignment_source, AlignmentSource::A2l);
    // 端点仍来自 CLI：UDP 的既有契约是必须显式给出 host/port。
    EXPECT_EQ(resolved.host, "127.0.0.1");
    EXPECT_EQ(resolved.endpoint_source, EndpointSource::Cli);
}

// ---------------------------------------------------------------------------
// 规则 1 / 4：TCP 必须显式 Profile；端点可回落 A2L
// ---------------------------------------------------------------------------

/**
 * @brief 规则 1：TCP 连接缺少 `--tcp-profile` 时必须失败，不得猜 Profile。
 * @details 猜错 Profile 的后果是帧边界解析错误（V1.5 有 LEN 外 Tail，V1.4 没有），
 *          表现为"偶发不一致"而非明确报错，因此这里坚持拒绝。
 */
TEST(TransportSelection, TcpWithoutProfileIsRejected) {
    auto request = MakeTcpV15Request();
    request.tcp_profile.reset();

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("--tcp-profile"), std::string::npos)
        << "错误文本必须指出缺哪个选项: " << error;
}

/**
 * @brief 规则 4：TCP 的 host/port 只给一半必须失败（成对校验）。
 */
TEST(TransportSelection, TcpRequiresHostAndPortInPair) {
    auto request = MakeTcpV15Request();
    request.port.reset();

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("成对"), std::string::npos) << error;
}

/**
 * @brief 规则 4：TCP 两者皆缺时回落到 A2L 声明的 `XCP_ON_TCP_IP` 端点。
 */
TEST(TransportSelection, TcpFallsBackToA2lEndpoint) {
    auto request = MakeTcpV15Request();
    request.host.reset();
    request.port.reset();

    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 6000;
    facts.host = "192.168.0.20";
    facts.instance_count = 1;
    // asam-v1.5 还要求对齐依据（规则 2）；这里由 A2L 简要声明提供。
    facts.packet_alignment_bits = 8;
    facts.alignment_declared = true;

    const auto resolved = Resolve(request, facts);

    EXPECT_EQ(resolved.host, "192.168.0.20");
    EXPECT_EQ(resolved.port, 6000U);
    EXPECT_EQ(resolved.endpoint_source, EndpointSource::A2l);
}

/**
 * @brief 规则 4：TCP 两者皆缺且 A2L 也未声明端点时必须失败并说明原因。
 */
TEST(TransportSelection, TcpWithoutAnyEndpointIsRejected) {
    auto request = MakeTcpV15Request();
    request.host.reset();
    request.port.reset();

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("A2L"), std::string::npos)
        << "应提示可从 A2L 获取端点: " << error;
}

// ---------------------------------------------------------------------------
// 规则 2 / 3：对齐与 Profile 的相容性
// ---------------------------------------------------------------------------

/**
 * @brief 规则 3：AsamV14 下 8 bit 对齐被接受，且来源标注为 Profile 决定。
 * @details V1.4 的帧严格是 `[LEN][CTR][LEN bytes]`，不存在 LEN 之外的 Tail，
 *          因此对齐天然只能是 8 bit——它不是用户的选择，而是 Profile 的推论。
 */
TEST(TransportSelection, AsamV14AcceptsEightBitAlignment) {
    auto request = MakeTcpV15Request();
    request.tcp_profile = TcpWireProfile::AsamV14;
    request.packet_alignment_bits = 8;

    const auto resolved = Resolve(request, {});

    EXPECT_EQ(resolved.wire_profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(resolved.alignment, PacketAlignment::Bit8);
    EXPECT_EQ(resolved.alignment_source, AlignmentSource::ProfileDefault);
}

/**
 * @brief 规则 3：AsamV14 与 16/32 bit 对齐组合必须被拒绝。
 */
TEST(TransportSelection, AsamV14RejectsWideAlignment) {
    for (const std::uint8_t bits : {std::uint8_t{16}, std::uint8_t{32}}) {
        auto request = MakeTcpV15Request();
        request.tcp_profile = TcpWireProfile::AsamV14;
        request.packet_alignment_bits = bits;

        const auto error = ResolveExpectFailure(request, {});
        EXPECT_NE(error.find("asam-v1.4"), std::string::npos)
            << bits << " bit 的错误文本应点名 Profile: " << error;
    }
}

/**
 * @brief 规则 2：AsamV15 下 CLI 对齐优先于 A2L 声明。
 */
TEST(TransportSelection, CliAlignmentWinsOverA2l) {
    auto request = MakeTcpV15Request();
    request.packet_alignment_bits = 32;

    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 5555;
    facts.host = "127.0.0.1";
    facts.packet_alignment_bits = 16;
    facts.alignment_declared = true;
    facts.instance_count = 1;

    const auto resolved = Resolve(request, facts);

    EXPECT_EQ(resolved.alignment, PacketAlignment::Bit32);
    EXPECT_EQ(resolved.alignment_source, AlignmentSource::Cli);
}

/**
 * @brief 规则 2：AsamV15 且两侧都未给出对齐时必须失败，不得默认 8 bit。
 * @details V1.5 的对齐决定 Tail 填充宽度，猜错的后果是静默丢字节。
 */
TEST(TransportSelection, AsamV15RequiresAlignmentEvidence) {
    const auto error = ResolveExpectFailure(MakeTcpV15Request(), {});
    EXPECT_NE(error.find("--packet-alignment"), std::string::npos) << error;
}

/**
 * @brief 规则 2：AsamV15 可从 A2L 显式声明取得对齐。
 */
TEST(TransportSelection, AsamV15TakesA2lAlignment) {
    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 5555;
    facts.host = "127.0.0.1";
    facts.packet_alignment_bits = 16;
    facts.alignment_declared = true;
    facts.instance_count = 1;

    const auto resolved = Resolve(MakeTcpV15Request(), facts);

    EXPECT_EQ(resolved.alignment, PacketAlignment::Bit16);
    EXPECT_EQ(resolved.alignment_source, AlignmentSource::A2l);
}

// ---------------------------------------------------------------------------
// 规则 5：端点/对齐/Profile 必须同属一个 transport kind
// ---------------------------------------------------------------------------

/**
 * @brief 规则 5：A2L 只声明了 UDP 端点时，TCP 连接不得借用其端口与对齐。
 */
TEST(TransportSelection, TcpIgnoresUdpOnlyA2lFacts) {
    auto request = MakeTcpV15Request();
    request.host.reset();
    request.port.reset();

    // 调用方按 kind 过滤后，TCP 视图里 declared 仍为 false；
    // 只有 other_kind_declared 为真，用于生成更有用的错误文本。
    A2lTransportFacts facts;
    facts.declared = false;
    facts.alignment_declared = false;
    facts.instance_count = 0;
    facts.other_kind_declared = true;

    const auto error = ResolveExpectFailure(request, facts);
    EXPECT_NE(error.find("UDP"), std::string::npos)
        << "应提示 A2L 只有 UDP 端点: " << error;
}

// ---------------------------------------------------------------------------
// 规则 6：PL 与 TL 各自独立
// ---------------------------------------------------------------------------

/**
 * @brief 规则 6：`--tcp-profile` 不得反推协议层版本。
 */
TEST(TransportSelection, ProfileDoesNotImplyProtocolVersion) {
    auto request = MakeTcpV15Request();
    request.packet_alignment_bits = 8;

    const auto resolved = Resolve(request, {});

    EXPECT_FALSE(resolved.protocol_version.has_value())
        << "asam-v1.5 只是 wire framing，不能推断 PL=1.5";
    EXPECT_FALSE(resolved.transport_version.has_value())
        << "TL 也不能由 Profile 推断";
}

/**
 * @brief 规则 6：显式给出的 PL/TL 原样透传。
 */
TEST(TransportSelection, ExplicitVersionsArePassedThrough) {
    auto request = MakeTcpV15Request();
    request.packet_alignment_bits = 8;
    request.protocol_version = XcpProtocolVersion{1, 2};
    request.transport_version = XcpProtocolVersion{1, 4};

    const auto resolved = Resolve(request, {});

    ASSERT_TRUE(resolved.protocol_version.has_value());
    ASSERT_TRUE(resolved.transport_version.has_value());
    EXPECT_EQ(*resolved.protocol_version, (XcpProtocolVersion{1, 2}));
    EXPECT_EQ(*resolved.transport_version, (XcpProtocolVersion{1, 4}));
}

/**
 * @brief 规则 6：A2L 静态版本只作为弱证据，与显式值冲突时不得覆盖显式值。
 */
TEST(TransportSelection, ExplicitVersionBeatsA2lEvidence) {
    auto request = MakeTcpV15Request();
    request.packet_alignment_bits = 8;
    request.transport_version = XcpProtocolVersion{1, 5};

    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 5555;
    facts.host = "127.0.0.1";
    facts.instance_count = 1;
    facts.transport_version = XcpProtocolVersion{1, 4};

    const auto resolved = Resolve(request, facts);

    ASSERT_TRUE(resolved.transport_version.has_value());
    EXPECT_EQ(*resolved.transport_version, (XcpProtocolVersion{1, 5}));
    EXPECT_FALSE(resolved.notes.empty())
        << "冲突必须留下可审计的提示，而不是静默选择";
}

// ---------------------------------------------------------------------------
// 规则 7：preset 原子展开
// ---------------------------------------------------------------------------

/**
 * @brief 规则 7：`--preset xcplite` 展开出 Profile/对齐/TL 的完整组合。
 */
TEST(TransportSelection, PresetXcpliteExpandsFullCombination) {
    TransportSelectionRequest request;
    request.transport = TransportKind::Tcp;
    request.preset_xcplite = true;
    request.host = std::string{"127.0.0.1"};
    request.port = static_cast<std::uint16_t>(5555);

    const auto resolved = Resolve(request, {});

    EXPECT_TRUE(resolved.preset_expanded);
    EXPECT_EQ(resolved.wire_profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(resolved.wire_profile_source, WireProfileSource::Preset);
    EXPECT_EQ(resolved.alignment, PacketAlignment::Bit8);
    EXPECT_EQ(resolved.alignment_source, AlignmentSource::ProfileDefault);
    EXPECT_TRUE(resolved.transport_version.has_value());
}

/**
 * @brief 规则 7：preset 与 `--transport udp` 冲突时必须报错而非静默改写。
 */
TEST(TransportSelection, PresetRejectsUdpTransport) {
    auto request = MakeUdpRequest();
    request.preset_xcplite = true;

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("preset"), std::string::npos) << error;
}

/**
 * @brief 规则 7：preset 展开后与显式 `--tcp-profile` 矛盾必须报错。
 * @details preset 的意义是"这一整套是 XCPlite 的部署事实"，单独改掉其中
 *          一项会让组合自相矛盾（例如 v1.4 framing 配 v1.5 语义），因此
 *          这里选择报错而不是让显式值静默胜出。
 */
TEST(TransportSelection, PresetRejectsConflictingProfile) {
    TransportSelectionRequest request;
    request.transport = TransportKind::Tcp;
    request.preset_xcplite = true;
    request.tcp_profile = TcpWireProfile::AsamV15;
    request.host = std::string{"127.0.0.1"};
    request.port = static_cast<std::uint16_t>(5555);

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("--tcp-profile"), std::string::npos) << error;
}

/**
 * @brief 规则 7：显式值与 preset 一致时可以采用，且对齐仍可显式覆盖。
 */
TEST(TransportSelection, PresetAcceptsConsistentExplicitValues) {
    TransportSelectionRequest request;
    request.transport = TransportKind::Tcp;
    request.preset_xcplite = true;
    request.tcp_profile = TcpWireProfile::AsamV14;
    request.host = std::string{"127.0.0.1"};
    request.port = static_cast<std::uint16_t>(5555);

    const auto resolved = Resolve(request, {});

    EXPECT_TRUE(resolved.preset_expanded);
    EXPECT_EQ(resolved.wire_profile, TcpWireProfile::AsamV14);
    EXPECT_EQ(resolved.alignment, PacketAlignment::Bit8);
}

// ---------------------------------------------------------------------------
// 文本解析与渲染
// ---------------------------------------------------------------------------

/**
 * @brief `--tcp-profile` 只接受完整名称，避免 `1.5` 之类歧义写法。
 */
TEST(TransportSelection, ParsesTcpProfileNames) {
    using calmcar::xcp::tools::ParseTcpWireProfileText;

    ASSERT_TRUE(ParseTcpWireProfileText("asam-v1.5").has_value());
    EXPECT_EQ(*ParseTcpWireProfileText("asam-v1.5"), TcpWireProfile::AsamV15);
    ASSERT_TRUE(ParseTcpWireProfileText("asam-v1.4").has_value());
    EXPECT_EQ(*ParseTcpWireProfileText("asam-v1.4"), TcpWireProfile::AsamV14);
    EXPECT_FALSE(ParseTcpWireProfileText("1.5").has_value());
    EXPECT_FALSE(ParseTcpWireProfileText("asam-v1.6").has_value());
}

/**
 * @brief `--packet-alignment` 只接受 8/16/32 三个位宽。
 */
TEST(TransportSelection, ParsesAlignmentBits) {
    using calmcar::xcp::tools::ParseAlignmentBitsText;

    ASSERT_TRUE(ParseAlignmentBitsText("8").has_value());
    EXPECT_EQ(*ParseAlignmentBitsText("8"), 8U);
    EXPECT_EQ(*ParseAlignmentBitsText("16"), 16U);
    EXPECT_EQ(*ParseAlignmentBitsText("32"), 32U);
    EXPECT_FALSE(ParseAlignmentBitsText("24").has_value());
    EXPECT_FALSE(ParseAlignmentBitsText("").has_value());
}

/**
 * @brief 摘要必须同时给出生效值与来源，否则无法审计"这个值是谁定的"。
 */
TEST(TransportSelection, SummaryShowsValueAndSource) {
    auto request = MakeTcpV15Request();
    request.packet_alignment_bits = 16;
    const auto resolved = Resolve(request, {});

    const auto text = calmcar::xcp::tools::FormatSelectionSummary(resolved);

    EXPECT_NE(text.find("tcp"), std::string::npos) << text;
    EXPECT_NE(text.find("AsamV15"), std::string::npos) << text;
    EXPECT_NE(text.find("16"), std::string::npos) << text;
    EXPECT_NE(text.find("--packet-alignment"), std::string::npos) << text;
}

/**
 * @brief 运行期摘要必须把 reported 与 effective 并排，便于发现封顶/保守。
 */
TEST(TransportSelection, RuntimeSummaryShowsReportedAndEffective) {
    calmcar::xcp::tools::RuntimeVersionStatus status;
    status.get_version_outcome = "可用";
    status.pl_reported = XcpProtocolVersion{1, 5};
    status.pl_effective = XcpProtocolVersion{1, 2};
    status.tl_reported = XcpProtocolVersion{1, 5};
    status.tl_effective = XcpProtocolVersion{1, 4};

    const auto text = calmcar::xcp::tools::FormatRuntimeVersionSummary(status);

    EXPECT_NE(text.find("PL=1.5/1.2"), std::string::npos) << text;
    EXPECT_NE(text.find("TL=1.5/1.4"), std::string::npos) << text;
}

// ---------------------------------------------------------------------------
// 计划 §7.3：多实例在 CLI 未覆盖时 fail closed
// ---------------------------------------------------------------------------

/**
 * @brief 计划 §7.3：A2L 声明多个**配置不同**的同类端点且 CLI 未给端点时必须失败。
 * @details "先到先得"是危险默认值：选错实例不会报错，只会连到另一台 ECU，
 *          现象是连接超时或读数不对，现场无法定位到端点选择这一层。
 */
TEST(TransportSelection, MultipleDistinctTcpInstancesFailClosed) {
    auto request = MakeTcpV15Request();
    request.host.reset();
    request.port.reset();

    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 6000;
    facts.host = "192.168.0.20";
    facts.packet_alignment_bits = 8;
    facts.alignment_declared = true;
    facts.instance_count = 2;
    facts.instances = {A2lEndpointInstance{"192.168.0.20", 6000, 8, 0x0104},
                       A2lEndpointInstance{"192.168.0.21", 6001, 16, 0x0105}};

    const auto error = ResolveExpectFailure(request, facts);

    // 错误必须同时给出"数量/各实例"与"该怎么办"
    EXPECT_NE(error.find("不同的同类端点"), std::string::npos) << error;
    EXPECT_NE(error.find("192.168.0.20:6000"), std::string::npos) << error;
    EXPECT_NE(error.find("192.168.0.21:6001"), std::string::npos) << error;
    EXPECT_NE(error.find("--host"), std::string::npos) << error;
}

/**
 * @brief 去重放宽：同一目标被重复声明两次时不得阻断，只提示一次。
 * @details 生成器重复输出是现实存在的，把完全相同的事实判为冲突只会制造噪音。
 */
TEST(TransportSelection, IdenticalDuplicateInstancesAreAccepted) {
    auto request = MakeTcpV15Request();
    request.host.reset();
    request.port.reset();

    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 6000;
    facts.host = "192.168.0.20";
    facts.packet_alignment_bits = 8;
    facts.alignment_declared = true;
    facts.instance_count = 2;
    facts.instances = {A2lEndpointInstance{"192.168.0.20", 6000, 8, 0x0104},
                       A2lEndpointInstance{"192.168.0.20", 6000, 8, 0x0104}};

    const auto resolved = Resolve(request, facts);

    EXPECT_EQ(resolved.host, "192.168.0.20");
    EXPECT_EQ(resolved.port, 6000U);
    EXPECT_EQ(resolved.endpoint_source, EndpointSource::A2l);
    bool mentioned = false;
    for (const std::string& note : resolved.notes) {
        if (note.find("重复声明") != std::string::npos) mentioned = true;
    }
    EXPECT_TRUE(mentioned) << "去重必须留下可审计的提示";
}

/** @brief 不可信对齐不得使多个 TCP 实例看起来等价。 */
TEST(TransportSelection, UnverifiedAlignmentPreventsDuplicateDeduplication) {
    auto request = MakeTcpV15Request();
    request.host.reset();
    request.port.reset();
    A2lTransportFacts facts;
    facts.declared = true;
    facts.host = "192.168.0.20";
    facts.port = 6000;
    facts.instance_alignment_unverified = true;
    facts.instances = {A2lEndpointInstance{"192.168.0.20", 6000, 0, 0x0104},
                       A2lEndpointInstance{"192.168.0.20", 6000, 0, 0x0104}};
    const auto error = ResolveExpectFailure(request, facts);
    EXPECT_NE(error.find("无法确认"), std::string::npos) << error;
    EXPECT_NE(error.find("--host/--port"), std::string::npos) << error;
}

/**
 * @brief CLI 端点已给出时多实例不拦：用户意图明确，实例数无关。
 */
TEST(TransportSelection, CliEndpointBypassesMultiInstanceCheck) {
    auto request = MakeTcpV15Request();

    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 6000;
    facts.host = "192.168.0.20";
    facts.packet_alignment_bits = 8;
    facts.alignment_declared = true;
    facts.instance_count = 2;
    facts.instances = {A2lEndpointInstance{"192.168.0.20", 6000, 8, 0x0104},
                       A2lEndpointInstance{"192.168.0.21", 6001, 16, 0x0105}};

    const auto resolved = Resolve(request, facts);

    EXPECT_EQ(resolved.host, "127.0.0.1");
    EXPECT_EQ(resolved.port, 5555U);
    EXPECT_EQ(resolved.endpoint_source, EndpointSource::Cli);
}

// ---------------------------------------------------------------------------
// P3：`--tcp-profile auto` 的前置校验（连接级候选试连的入口条件）
// ---------------------------------------------------------------------------

/// @brief 构造一个"TCP + auto + CLI 端点"的请求
TransportSelectionRequest MakeTcpAutoRequest() {
    TransportSelectionRequest request;
    request.transport = TransportKind::Tcp;
    request.tcp_profile_auto = true;
    request.host = std::string{"127.0.0.1"};
    request.port = static_cast<std::uint16_t>(5555);
    return request;
}

/**
 * @brief 严格 auto 不再在建连前拒绝，而是把布局交给候选验证决定。
 *
 * @details 证据收敛（`BuildTcpFramingCandidates`）与协议层取证
 *          （`VerifyTcpFramingCandidate`）落地后，解析层只负责"允许自动并
 *          标记待定"，不再以"存在已知歧义"为由阻断用户。
 */
TEST(TransportSelection, TcpProfileAutoDefersLayoutToCandidateVerification) {
    const auto resolved = Resolve(MakeTcpAutoRequest(), {});

    EXPECT_TRUE(resolved.wire_profile_auto);
    EXPECT_EQ(resolved.wire_profile, TcpWireProfile::Unknown);
    EXPECT_EQ(resolved.alignment, PacketAlignment::Unknown);
    EXPECT_EQ(resolved.wire_profile_source, WireProfileSource::Auto);
    ASSERT_FALSE(resolved.notes.empty());
    EXPECT_NE(resolved.notes.front().find("候选"), std::string::npos)
        << resolved.notes.front();
    EXPECT_NE(resolved.notes.front().find("未决"), std::string::npos)
        << resolved.notes.front();
}

/**
 * @brief 旧模式保留待试连状态，同时明确告知首成功的风险。
 */
TEST(TransportSelection, TcpProfileAutoLegacyLeavesWireProfileUnresolved) {
    auto request = MakeTcpAutoRequest();
    request.tcp_profile_auto = false;
    request.tcp_profile_auto_legacy = true;
    const auto resolved = Resolve(request, {});

    EXPECT_TRUE(resolved.wire_profile_auto);
    EXPECT_EQ(resolved.wire_profile, TcpWireProfile::Unknown);
    EXPECT_EQ(resolved.alignment, PacketAlignment::Unknown);
    EXPECT_EQ(resolved.wire_profile_source, WireProfileSource::AutoLegacy);
    EXPECT_NE(calmcar::xcp::tools::FormatSelectionSummary(resolved).find("对齐=未确定"),
              std::string::npos);
    ASSERT_FALSE(resolved.notes.empty());
    EXPECT_NE(resolved.notes.front().find("auto-legacy"), std::string::npos);
    EXPECT_NE(resolved.notes.front().find("歧义"), std::string::npos);
    EXPECT_NE(resolved.notes.front().find("超时"), std::string::npos);
}

/**
 * @brief auto 只对 TCP 有意义：UDP 上给 auto 必须报错而不是忽略。
 */
TEST(TransportSelection, TcpProfileAutoRejectsUdpTransport) {
    auto request = MakeUdpRequest();
    request.tcp_profile_auto = true;

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("只对 --transport tcp 有效"), std::string::npos) << error;
}

/**
 * @brief auto 与 preset 互斥：preset 已经把 asam-v1.4 固定下来。
 */
TEST(TransportSelection, TcpProfileAutoRejectsPreset) {
    auto request = MakeTcpAutoRequest();
    request.preset_xcplite = true;

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("--preset xcplite 冲突"), std::string::npos) << error;
}

/**
 * @brief auto 与 `--packet-alignment` 互斥：对齐由候选决定。
 */
TEST(TransportSelection, TcpProfileAutoRejectsExplicitAlignment) {
    auto request = MakeTcpAutoRequest();
    request.packet_alignment_bits = static_cast<std::uint8_t>(16);

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("--packet-alignment"), std::string::npos) << error;
}

/**
 * @brief auto 与具体 Profile 互斥（同一参数两种模式不可并存）。
 */
TEST(TransportSelection, TcpProfileAutoRejectsExplicitProfile) {
    auto request = MakeTcpAutoRequest();
    request.tcp_profile = TcpWireProfile::AsamV14;

    const auto error = ResolveExpectFailure(request, {});
    EXPECT_NE(error.find("不能同时给出 auto"), std::string::npos) << error;
}

/**
 * @brief 旧模式遵守与严格模式相同的参数互斥规则。
 */
TEST(TransportSelection, TcpProfileAutoLegacyRejectsConflicts) {
    auto request = MakeTcpAutoRequest();
    request.tcp_profile_auto = false;
    request.tcp_profile_auto_legacy = true;
    auto conflict = request;
    conflict.transport = TransportKind::Udp;
    EXPECT_NE(ResolveExpectFailure(conflict, {}).find("tcp"), std::string::npos);
    conflict = request;
    conflict.preset_xcplite = true;
    EXPECT_NE(ResolveExpectFailure(conflict, {}).find("preset"), std::string::npos);
    conflict = request;
    conflict.packet_alignment_bits = 8;
    EXPECT_NE(ResolveExpectFailure(conflict, {}).find("alignment"), std::string::npos);
    conflict = request;
    conflict.tcp_profile = TcpWireProfile::AsamV14;
    EXPECT_NE(ResolveExpectFailure(conflict, {}).find("Profile"), std::string::npos);
    conflict = request;
    conflict.tcp_profile_auto = true;
    EXPECT_NE(ResolveExpectFailure(conflict, {}).find("auto-legacy"), std::string::npos);
}

// ---------------------------------------------------------------------------
// P4：auto 协商结果的退出码映射（脚本唯一可靠判据）
// ---------------------------------------------------------------------------

/// @brief 构造"全部已尝试、均无胜出"的协商结果
calmcar::xcp::TcpFramingNegotiationResult MakeNegotiation(
    const std::vector<calmcar::xcp::FramingVerdict>& verdicts) {
    calmcar::xcp::TcpFramingNegotiationResult outcome;
    for (const auto verdict : verdicts) {
        calmcar::xcp::FramingAttemptOutcome item;
        item.verdict = verdict;
        item.failure_reason = "测试构造";
        outcome.attempts.push_back(item);
    }
    return outcome;
}

/**
 * @brief 全部候选被拒 → 退出码 3（对端在回话，布局不对）
 */
TEST(TransportSelection, NegotiationExitCodeIsThreeWhenAllRejected) {
    const auto outcome = MakeNegotiation(
        {calmcar::xcp::FramingVerdict::Rejected,
         calmcar::xcp::FramingVerdict::Rejected});
    EXPECT_EQ(calmcar::xcp::tools::TcpNegotiationExitCode(outcome),
              calmcar::xcp::tools::kAutoNegotiationAllRejected);
}

/**
 * @brief 只要有一个未决 → 退出码 4（对端没回话，不能断言"布局都不支持"）
 */
TEST(TransportSelection, NegotiationExitCodeIsFourWhenAnyInconclusive) {
    const auto outcome = MakeNegotiation(
        {calmcar::xcp::FramingVerdict::Rejected,
         calmcar::xcp::FramingVerdict::Inconclusive,
         calmcar::xcp::FramingVerdict::Rejected});
    EXPECT_EQ(calmcar::xcp::tools::TcpNegotiationExitCode(outcome),
              calmcar::xcp::tools::kAutoNegotiationInconclusive);
}

/**
 * @brief 有胜出候选 → 退出码 0，且与"被拒记录"并存时仍视为成功
 */
TEST(TransportSelection, NegotiationExitCodeIsZeroWhenSelected) {
    auto outcome = MakeNegotiation(
        {calmcar::xcp::FramingVerdict::Rejected,
         calmcar::xcp::FramingVerdict::Accepted});
    outcome.selected = calmcar::xcp::TcpFramingCandidate{
        calmcar::xcp::TcpWireProfile::AsamV15,
        calmcar::xcp::PacketAlignment::Bit16};
    EXPECT_EQ(calmcar::xcp::tools::TcpNegotiationExitCode(outcome),
              calmcar::xcp::tools::kAutoNegotiationOk);
}

}  // namespace
