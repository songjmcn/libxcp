/**
 * @file transport_selection_standalone_test.cpp
 * @brief 独立调用者视角的验收：只依赖 `include/libxcp` 公共头，不用任何 `tools/` 头。
 *
 * 依据 `code-plan/TCP_A2L与线路证据联合决策_两阶段计划.md` §5.3：第二阶段把
 * 组件提取进库之后，必须证明"库外调用者能在不知道 `xcp_master` 内部结构的前提下
 * 使用它们"。因此本文件**刻意**不 include 任何 `tools/...` 路径——一旦某个
 * 公共头泄漏了对工具侧头的依赖，这里会直接编译失败。
 *
 * @par 与 `xcp_master_transport_selection_test.cpp` 的分工
 *   那份用例是"规则穷举"（九条规则 × 输入维度），仍允许引用工具侧的 CLI 文本层；
 *   本文件是"契约冒烟"：少量用例钉住公共 API 的可用形态与关键结论，
 *   重点在依赖边界，不在覆盖度。
 */

// 三个公共头必须能各自独立编译（顺序无关、互不隐式提供对方声明）。
#include "libxcp/a2l_tcp_alignment.hpp"
#include "libxcp/transport_selection.hpp"
#include "libxcp/trusted_a2l_snapshot.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using calmcar::xcp::A2lAlignmentTrust;
using calmcar::xcp::A2lAlignmentVerdict;
using calmcar::xcp::A2lEndpointInstance;
using calmcar::xcp::A2lTcpCandidate;
using calmcar::xcp::A2lTransportFacts;
using calmcar::xcp::CrossCheckA2lTcpAlignment;
using calmcar::xcp::ResolveTransportSelection;
using calmcar::xcp::ResolvedTransportSelection;
using calmcar::xcp::ScanA2lTcpCandidate;
using calmcar::xcp::SdkTcpEndpointFacts;
using calmcar::xcp::TcpWireProfile;
using calmcar::xcp::TransportKind;
using calmcar::xcp::XcpProtocolVersion;
using calmcar::xcp::TransportSelectionRequest;

/// @brief 构造"TCP + 严格 auto + CLI 端点"的请求。
TransportSelectionRequest MakeStrictAutoRequest() {
    TransportSelectionRequest request;
    request.transport = TransportKind::Tcp;
    request.tcp_profile_auto = true;
    request.host = std::string{"127.0.0.1"};
    request.port = static_cast<std::uint16_t>(5555);
    return request;
}

/// @brief 构造一个声明了 XCP_ON_TCP_IP 的最小 A2L 文本。
std::string MakeA2lWithTcpBlock(const std::string& body) {
    return "ASAP2_VERSION 1 71\n"
           "/begin PROJECT p \"\"\n"
           "/begin MODULE m \"\"\n"
           "/begin IF_DATA XCP\n" +
           body +
           "/end IF_DATA\n"
           "/end MODULE\n"
           "/end PROJECT\n";
}

/// @brief 独立调用者必须能看到"严格 auto 把布局判定交给候选验证"的契约。
TEST(TransportSelectionStandaloneTest, StrictAutoDefersLayoutToVerification) {
    auto request = MakeStrictAutoRequest();
    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 5555;
    facts.host = "127.0.0.1";
    facts.instance_count = 2;
    A2lEndpointInstance first;
    first.host = "127.0.0.1";
    first.port = 5555;
    first.transport_version = 0x0104;
    A2lEndpointInstance second = first;
    second.port = 5556;
    facts.instances = {first, second};

    std::string error;
    const auto resolved = ResolveTransportSelection(request, facts, &error);
    ASSERT_TRUE(resolved.has_value()) << error;
    // 端点由 CLI 显式给出，因此多实例不会触发 fail closed；布局则必须留空，
    // 由调用方按候选逐个验证后回填。
    EXPECT_TRUE(resolved->wire_profile_auto);
    EXPECT_EQ(resolved->wire_profile, TcpWireProfile::Unknown);
    EXPECT_EQ(resolved->alignment, calmcar::xcp::PacketAlignment::Unknown);
    EXPECT_EQ(resolved->wire_profile_source,
              calmcar::xcp::WireProfileSource::Auto);
}

/// @brief `auto-legacy` 是显式选择的风险路径，必须回填"待试连"标志。
TEST(TransportSelectionStandaloneTest, AutoLegacyIsExplicitlyFlagged) {
    auto request = MakeStrictAutoRequest();
    request.tcp_profile_auto = false;
    request.tcp_profile_auto_legacy = true;
    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 5555;
    facts.host = "127.0.0.1";
    facts.instance_count = 1;
    facts.instances = {A2lEndpointInstance{"127.0.0.1", 5555, 0, 0x0104}};

    std::string error;
    const auto resolved = ResolveTransportSelection(request, facts, &error);
    ASSERT_TRUE(resolved.has_value()) << error;
    EXPECT_TRUE(resolved->wire_profile_auto)
        << "auto-legacy 必须让调用方先试连再回填 Profile";
}

/// @brief 显式端点与 A2L 唯一端点不匹配时，端点专属证据必须被丢弃。
TEST(TransportSelectionStandaloneTest, MismatchedEndpointDropsEvidence) {
    TransportSelectionRequest request;
    request.transport = TransportKind::Tcp;
    request.tcp_profile = TcpWireProfile::AsamV15;
    request.host = std::string{"127.0.0.1"};
    request.port = static_cast<std::uint16_t>(5555);

    A2lTransportFacts facts;
    facts.declared = true;
    facts.port = 6000;  ///< 与 CLI 端点不同
    facts.host = "127.0.0.1";
    facts.packet_alignment_bits = 16;
    facts.alignment_declared = true;
    facts.transport_version = XcpProtocolVersion{1, 4};
    facts.profile_hint = "asam-v1.5";
    facts.instance_count = 1;
    facts.instances = {A2lEndpointInstance{"127.0.0.1", 6000, 16, 0x0104}};

    EXPECT_TRUE(calmcar::xcp::DiscardUnmatchedTcpEndpointEvidence(request, facts));
    EXPECT_FALSE(facts.alignment_declared);
    EXPECT_EQ(facts.packet_alignment_bits, 0);
    EXPECT_FALSE(facts.transport_version.has_value());
    EXPECT_TRUE(facts.profile_hint.empty());
}

/// @brief 扫描器对外是"字节进、候选出"的纯函数，独立调用者可直接使用。
TEST(TransportSelectionStandaloneTest, ScannerWorksWithoutToolSideHelpers) {
    const auto text = MakeA2lWithTcpBlock(
        "/begin XCP_ON_TCP_IP 0x104 5555\n"
        "      PACKET_ALIGNMENT PACKET_ALIGNMENT_16\n"
        "    /end XCP_ON_TCP_IP\n");
    const auto candidate = ScanA2lTcpCandidate(text);
    ASSERT_TRUE(candidate.has_value()) << "标准 TCP 块应被识别";
    EXPECT_EQ(candidate->version, 0x0104U);
    EXPECT_EQ(candidate->port, 5555U);
    ASSERT_TRUE(candidate->alignment.has_value());
    EXPECT_EQ(*candidate->alignment, 16U);
}

/// @brief 缺少 TCP 块的输入必须被拒绝，不能返回"默认 8 bit"的假声明。
TEST(TransportSelectionStandaloneTest, ScannerRejectsInputWithoutTcpBlock) {
    const auto text = MakeA2lWithTcpBlock("    /begin DAQ\n    /end DAQ\n");
    EXPECT_FALSE(ScanA2lTcpCandidate(text).has_value());
}

/// @brief 两源一致才可信；未声明与不一致都必须给出非可信结论。
TEST(TransportSelectionStandaloneTest, CrossCheckClassifiesThreeOutcomes) {
    A2lTcpCandidate scan;
    scan.version = 0x0104;
    scan.port = 5555;
    scan.alignment = 16;

    SdkTcpEndpointFacts match;
    match.host = "127.0.0.1";
    match.port = 5555;
    match.version = 0x0104;
    match.alignment = 16;
    match.alignment_declared = true;
    const auto trusted = CrossCheckA2lTcpAlignment(scan, {match}, {false});
    EXPECT_EQ(trusted.trust, A2lAlignmentTrust::kTrusted);

    SdkTcpEndpointFacts drift = match;
    drift.version = 0x0103;
    const auto untrusted = CrossCheckA2lTcpAlignment(scan, {drift}, {false});
    EXPECT_EQ(untrusted.trust, A2lAlignmentTrust::kUntrusted);

    A2lTcpCandidate bare;
    bare.version = 0x0104;
    bare.port = 5555;
    SdkTcpEndpointFacts sdk_bare;
    sdk_bare.port = 5555;
    sdk_bare.version = 0x0104;
    const auto not_declared = CrossCheckA2lTcpAlignment(bare, {sdk_bare}, {false});
    EXPECT_EQ(not_declared.trust, A2lAlignmentTrust::kNotDeclared);
}

}  // namespace