/**
 * @file xcp_master_transport_sniff_test.cpp
 * @brief `tools/xcp_master/transport_sniff.hpp` 的单元测试。
 *
 * 依据 `code-plan/xcp_master_传输层与对齐从A2L推导_实施计划.md` §9（双路嗅探）
 * 与 §10（实施步骤 T1/T9）编写。
 *
 * @par 测试策略
 *   嗅探逻辑被刻意抽成**不碰 socket 的纯函数**，正是为了让"计划顺序"
 *   与"首胜即停"这类契约可以逐条钉住：真正的连通性验证放在入库之前的
 *   手工冒烟（见实施记录 §5）。本文件只断言"给定输入产生什么计划、
 *   谓词怎么被调用、结论如何汇总"。
 */

#include "tools/xcp_master/include_resolver.hpp"
#include "tools/xcp_master/transport_sniff.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using calmcar::xcp::FramingVerdict;
using calmcar::xcp::FramingVerificationResult;
using calmcar::xcp::TransportKind;
using calmcar::xcp::tools::A2lEndpointDeclaration;
using calmcar::xcp::tools::A2lEndpointSummary;
using calmcar::xcp::tools::SummarizeA2lEndpoints;
using calmcar::xcp::tools::BuildDualSniffPlan;
using calmcar::xcp::tools::DescribeSniffEntry;
using calmcar::xcp::tools::SniffExitCode;
using calmcar::xcp::tools::SniffPlanEntry;
using calmcar::xcp::tools::SniffScope;
using calmcar::xcp::tools::SniffTransport;
using calmcar::xcp::tools::SniffTransportName;
using calmcar::xcp::tools::IncludeReadOptions;
using calmcar::xcp::tools::MakeIncludeResolver;
using calmcar::xcp::tools::ReadIncludeFile;
using calmcar::xcp::tools::TransportSniffResult;

/**
 * @brief 构造一个"已声明"的端点摘要
 * @param host 地址
 * @param port 端口
 * @return 摘要对象（`declared = true`）
 */
A2lEndpointSummary Declared(const std::string& host, std::uint16_t port) {
    A2lEndpointSummary summary;
    summary.declared = true;
    summary.host = host;
    summary.port = port;
    summary.instance_count = 1;
    return summary;
}

/**
 * @brief 构造一个固定结论的谓词，并记录被调用的条目
 * @param verdict 期望返回的结论
 * @param calls 记录调用顺序的容器
 * @return 谓词
 */
calmcar::xcp::tools::SniffAttemptFn VerdictPredicate(
    FramingVerdict verdict, std::vector<std::string>* calls) {
    return [verdict, calls](const SniffPlanEntry& entry) {
        if (calls != nullptr) {
            calls->push_back(DescribeSniffEntry(entry));
        }
        FramingVerificationResult result;
        result.verdict = verdict;
        result.elapsed_ms = 7;
        if (verdict != FramingVerdict::Accepted) {
            result.reason = "固定结论";
        }
        return result;
    };
}

/**
 * @brief 统计计划中某一类传输层的条目数
 */
std::size_t CountOf(const std::vector<SniffPlanEntry>& plan,
                    TransportKind transport) {
    std::size_t count = 0;
    for (const auto& entry : plan) {
        if (entry.transport == transport) ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// 计划顺序（§9.3）
// ---------------------------------------------------------------------------

/**
 * @brief P3：TCP 与 UDP 都声明时，TCP 在计划里排在前面。
 */
TEST(TransportSniffPlan, TcpFirstWhenBothDeclared) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);

    ASSERT_FALSE(plan.empty());
    EXPECT_EQ(plan.front().transport, TransportKind::Tcp);
    EXPECT_EQ(CountOf(plan, TransportKind::Tcp) > 0, true);
    EXPECT_EQ(CountOf(plan, TransportKind::Udp), 1U);
}

/**
 * @brief P1：只声明 TCP 时，计划里没有 UDP 条目。
 */
TEST(TransportSniffPlan, OnlyTcpDeclaredHasNoUdpEntry) {
    const auto plan =
        BuildDualSniffPlan(Declared("192.168.0.1", 5555), A2lEndpointSummary{},
                           std::nullopt, std::nullopt, SniffScope::Dual);

    ASSERT_FALSE(plan.empty());
    EXPECT_EQ(CountOf(plan, TransportKind::Udp), 0U);
    EXPECT_EQ(plan.front().transport, TransportKind::Tcp);
}

/**
 * @brief P2：只声明 UDP 时，UDP 排在 TCP 之前。
 */
TEST(TransportSniffPlan, UdpFirstWhenOnlyUdpDeclared) {
    const auto plan =
        BuildDualSniffPlan(A2lEndpointSummary{}, Declared("192.168.0.1", 5559),
                           std::nullopt, std::nullopt, SniffScope::Dual);

    ASSERT_FALSE(plan.empty());
    EXPECT_EQ(plan.front().transport, TransportKind::Udp);
    EXPECT_EQ(CountOf(plan, TransportKind::Tcp), 0U);
}

/**
 * @brief P4：A2L 什么都没声明但命令行给了端点时，两路都用命令行端点。
 */
TEST(TransportSniffPlan, CliEndpointUsedForBothWhenA2lSilent) {
    const auto plan = BuildDualSniffPlan(A2lEndpointSummary{},
                                         A2lEndpointSummary{},
                                         std::optional<std::string>{"10.0.0.2"},
                                         std::optional<std::uint16_t>{6000},
                                         SniffScope::Dual);

    ASSERT_FALSE(plan.empty());
    EXPECT_EQ(plan.front().transport, TransportKind::Tcp);
    EXPECT_EQ(CountOf(plan, TransportKind::Udp), 1U);
    for (const auto& entry : plan) {
        EXPECT_EQ(entry.host, "10.0.0.2");
        EXPECT_EQ(entry.port, 6000U);
        // 端点来自命令行：`endpoint_from_a2l` 必须为 false，否则 save 会把
        // "用户给的值"记成"A2L 声明"。
        EXPECT_FALSE(entry.endpoint_from_a2l);
    }
}

/**
 * @brief §9.4：命令行端点覆盖 A2L 声明（两路共用）。
 */
TEST(TransportSniffPlan, CliEndpointOverridesA2l) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.2", 5559),
                                         std::optional<std::string>{"127.0.0.1"},
                                         std::optional<std::uint16_t>{4444},
                                         SniffScope::Dual);

    ASSERT_FALSE(plan.empty());
    for (const auto& entry : plan) {
        EXPECT_EQ(entry.host, "127.0.0.1");
        EXPECT_EQ(entry.port, 4444U);
        EXPECT_FALSE(entry.endpoint_from_a2l);
    }
}

// ---------------------------------------------------------------------------
// 跳过规则（§9.4 / §9.3）
// ---------------------------------------------------------------------------

/**
 * @brief 多实例歧义的一路不进入计划：连错实例不会报错，只会连到另一台 ECU。
 */
TEST(TransportSniffPlan, AmbiguousEndpointIsSkipped) {
    auto udp = Declared("192.168.0.1", 5559);
    udp.ambiguous = true;

    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555), udp,
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);

    ASSERT_FALSE(plan.empty());
    EXPECT_EQ(CountOf(plan, TransportKind::Udp), 0U);
}

/**
 * @brief 端口为 0（不可用）的声明同样被跳过。
 */
TEST(TransportSniffPlan, ZeroPortIsSkipped) {
    auto udp = Declared("192.168.0.1", 0);

    const auto plan = BuildDualSniffPlan(A2lEndpointSummary{}, udp,
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);

    EXPECT_TRUE(plan.empty());
}

/**
 * @brief 两路都无从连接时计划为空（调用方据此给出"没有可用端点"诊断）。
 */
TEST(TransportSniffPlan, EmptyWhenNothingUsable) {
    const auto plan = BuildDualSniffPlan(A2lEndpointSummary{},
                                         A2lEndpointSummary{}, std::nullopt,
                                         std::nullopt, SniffScope::Dual);

    EXPECT_TRUE(plan.empty());
}

/**
 * @brief `TcpOnly` 不产生 UDP 条目：用户已用 `--transport tcp` 表达意图。
 */
TEST(TransportSniffPlan, TcpOnlyScopeHasNoUdpEntry) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::TcpOnly);

    ASSERT_FALSE(plan.empty());
    EXPECT_EQ(CountOf(plan, TransportKind::Udp), 0U);
}

/**
 * @brief `TcpOnly` 且 A2L 未声明 TCP 时计划为空（不做 UDP 兜底）。
 */
TEST(TransportSniffPlan, TcpOnlyScopeEmptyWithoutTcpEndpoint) {
    const auto plan =
        BuildDualSniffPlan(A2lEndpointSummary{}, Declared("192.168.0.1", 5559),
                           std::nullopt, std::nullopt, SniffScope::TcpOnly);

    EXPECT_TRUE(plan.empty());
}

// ---------------------------------------------------------------------------
// TCP 候选展开（§9.2）
// ---------------------------------------------------------------------------

/**
 * @brief TCP 一条端点展开为多个帧布局候选，且都带候选与相同的端点。
 */
TEST(TransportSniffPlan, TcpEndpointExpandsToCandidates) {
    auto tcp = Declared("192.168.0.1", 5555);
    tcp.transport_layer_version = 0x0104U;
    tcp.packet_alignment_bits = 8;

    const auto plan = BuildDualSniffPlan(tcp, A2lEndpointSummary{}, std::nullopt,
                                         std::nullopt, SniffScope::TcpOnly);

    ASSERT_FALSE(plan.empty());
    for (const auto& entry : plan) {
        EXPECT_EQ(entry.transport, TransportKind::Tcp);
        EXPECT_TRUE(entry.candidate.has_value());
        EXPECT_EQ(entry.host, "192.168.0.1");
        EXPECT_EQ(entry.port, 5555U);
        EXPECT_TRUE(entry.endpoint_from_a2l);
    }
}

// ---------------------------------------------------------------------------
// 执行与结论（§9.3 首胜即停 / §9.8 失败语义）
// ---------------------------------------------------------------------------

/**
 * @brief 首胜即停：第三个条目被接受后不再尝试后面的条目。
 */
TEST(TransportSniffRun, StopsAtFirstAccepted) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);
    ASSERT_GE(plan.size(), 3U);

    std::vector<std::string> calls;
    std::size_t index = 0;
    const auto attempt = [&index, &calls](const SniffPlanEntry& entry) {
        calls.push_back(DescribeSniffEntry(entry));
        FramingVerificationResult result;
        result.elapsed_ms = 3;
        result.verdict = index++ == 2 ? FramingVerdict::Accepted
                                      : FramingVerdict::Rejected;
        return result;
    };

    const auto result = SniffTransport(plan, attempt);

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(result.attempts.size(), 3U);
    EXPECT_EQ(calls.size(), 3U);
    EXPECT_TRUE(result.tcp_responded);
    EXPECT_FALSE(result.udp_responded);
    // 胜出的就是第三次尝试的条目。
    EXPECT_EQ(DescribeSniffEntry(*result.selected), calls.back());
}

/**
 * @brief 三态逐条保留：被拒与未决都记录，且不终止扫描。
 */
TEST(TransportSniffRun, KeepsThreeStatesWithoutStopping) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);
    ASSERT_GE(plan.size(), 2U);

    const auto result =
        SniffTransport(plan, VerdictPredicate(FramingVerdict::Inconclusive,
                                              nullptr));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(result.attempts.size(), plan.size());
    for (const auto& item : result.attempts) {
        EXPECT_EQ(item.verdict, FramingVerdict::Inconclusive);
        EXPECT_EQ(item.reason, "固定结论");
    }
    // 未决不等于"对端拒绝"：退出码必须是 4（该查网络），不是 3（该改配置）。
    EXPECT_EQ(SniffExitCode(result),
              calmcar::xcp::tools::kAutoNegotiationInconclusive);
}

/**
 * @brief 全部被拒时退出码为 3。
 */
TEST(TransportSniffRun, AllRejectedMapsToExitCode3) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);

    const auto result = SniffTransport(
        plan, VerdictPredicate(FramingVerdict::Rejected, nullptr));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_EQ(SniffExitCode(result),
              calmcar::xcp::tools::kAutoNegotiationAllRejected);
}

/**
 * @brief 成功时退出码为 0。
 */
TEST(TransportSniffRun, AcceptedMapsToExitCode0) {
    const auto plan = BuildDualSniffPlan(A2lEndpointSummary{},
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);

    const auto result =
        SniffTransport(plan, VerdictPredicate(FramingVerdict::Accepted,
                                              nullptr));

    ASSERT_TRUE(result.Succeeded());
    EXPECT_EQ(SniffExitCode(result),
              calmcar::xcp::tools::kAutoNegotiationOk);
    EXPECT_TRUE(result.udp_responded);
    EXPECT_FALSE(result.tcp_responded);
    EXPECT_EQ(result.selected->transport, TransportKind::Udp);
}

/**
 * @brief 谓词抛异常按"未决"记录，异常不外泄。
 */
TEST(TransportSniffRun, PredicateExceptionBecomesInconclusive) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         A2lEndpointSummary{}, std::nullopt,
                                         std::nullopt, SniffScope::TcpOnly);
    ASSERT_FALSE(plan.empty());

    // 返回类型必须显式写出：lambda 只含 `throw` 时返回类型会被推导为 void。
    const auto attempt = [](const SniffPlanEntry&)
        -> FramingVerificationResult {
        throw std::runtime_error("连接被重置");
    };

    TransportSniffResult result;
    EXPECT_NO_THROW(result = SniffTransport(plan, attempt));
    EXPECT_FALSE(result.Succeeded());
    for (const auto& item : result.attempts) {
        EXPECT_EQ(item.verdict, FramingVerdict::Inconclusive);
        EXPECT_NE(item.reason.find("连接被重置"), std::string::npos);
    }
}

/**
 * @brief 没有谓词时不能伪造结论：全部记为未决。
 */
TEST(TransportSniffRun, EmptyPredicateIsInconclusive) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         A2lEndpointSummary{}, std::nullopt,
                                         std::nullopt, SniffScope::TcpOnly);

    const auto result = SniffTransport(plan, nullptr);

    EXPECT_FALSE(result.Succeeded());
    ASSERT_FALSE(result.attempts.empty());
    EXPECT_EQ(result.attempts.front().verdict, FramingVerdict::Inconclusive);
}

// ---------------------------------------------------------------------------
// 端点摘要归纳（评审问题 1/2：判歧义必须看四要素）
// ---------------------------------------------------------------------------

/**
 * @brief 构造一条端点声明
 * @param host 地址
 * @param port 端口
 * @param version 传输层版本，0 表示未声明
 * @param alignment 对齐位宽，0 表示未声明
 * @param instances A2L 中同类端点的声明个数
 * @return 声明对象
 */
A2lEndpointDeclaration Declaration(const std::string& host,
                                   std::uint16_t port,
                                   std::uint16_t version = 0,
                                   std::uint8_t alignment = 0,
                                   std::uint32_t instances = 1) {
    A2lEndpointDeclaration declaration;
    declaration.host = host;
    declaration.port = port;
    declaration.transport_layer_version = version;
    declaration.packet_alignment_bits = alignment;
    declaration.instance_count = instances;
    return declaration;
}

/**
 * @brief 单条声明：没有可比对象，不可能判歧义。
 * @details 同时钉住"未声明的版本不得出现在摘要里"——`optional` 为空表示
 *          没有证据，而不是"版本 0"。
 */
TEST(TransportSniffSummary, SingleDeclarationIsNotAmbiguous) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555, 0x0104U, 8U)};

    const auto summary = SummarizeA2lEndpoints(declarations, false);

    EXPECT_TRUE(summary.declared);
    EXPECT_FALSE(summary.ambiguous);
    EXPECT_EQ(summary.host, "192.168.0.1");
    EXPECT_EQ(summary.port, 5555U);
    EXPECT_EQ(summary.packet_alignment_bits, 8U);
    ASSERT_TRUE(summary.transport_layer_version.has_value());
    EXPECT_EQ(*summary.transport_layer_version, 0x0104U);
    EXPECT_EQ(summary.instance_count, 1U);
}

/**
 * @brief 未声明版本时摘要的 `optional` 保持为空。
 */
TEST(TransportSniffSummary, UndeclaredVersionStaysEmpty) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555)};

    const auto summary = SummarizeA2lEndpoints(declarations, false);

    EXPECT_FALSE(summary.transport_layer_version.has_value());
    EXPECT_EQ(summary.packet_alignment_bits, 0U);
}

/**
 * @brief 完全重复的声明（生成器重复输出）去重后仍是唯一，不判歧义。
 */
TEST(TransportSniffSummary, IdenticalDuplicatesAreNotAmbiguous) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555, 0x0104U, 8U),
        Declaration("192.168.0.1", 5555, 0x0104U, 8U)};

    const auto summary = SummarizeA2lEndpoints(declarations, false);

    EXPECT_FALSE(summary.ambiguous);
    EXPECT_EQ(summary.host, "192.168.0.1");
    EXPECT_EQ(summary.port, 5555U);
}

/**
 * @brief 地址不同：连错实例不会报错，只会连到另一台 ECU，必须判歧义。
 */
TEST(TransportSniffSummary, DifferentHostIsAmbiguous) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555), Declaration("192.168.0.2", 5555)};

    EXPECT_TRUE(SummarizeA2lEndpoints(declarations, false).ambiguous);
}

/**
 * @brief 端口不同：同上。
 */
TEST(TransportSniffSummary, DifferentPortIsAmbiguous) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555), Declaration("192.168.0.1", 5559)};

    EXPECT_TRUE(SummarizeA2lEndpoints(declarations, false).ambiguous);
}

/**
 * @brief 传输层版本不同：同址同端口但线路线格式不同，仍不能替用户选。
 * @details 这正是评审问题 2：只比 host+port 会把这种情况漏掉，让首实例的
 *          证据悄悄决定 TCP 候选顺序。
 */
TEST(TransportSniffSummary, DifferentTransportLayerVersionIsAmbiguous) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555, 0x0104U),
        Declaration("192.168.0.1", 5555, 0x0105U)};

    EXPECT_TRUE(SummarizeA2lEndpoints(declarations, false).ambiguous);
}

/**
 * @brief 对齐不同：同址同端口同版本，但线路线宽不同，同样判歧义。
 */
TEST(TransportSniffSummary, DifferentAlignmentIsAmbiguous) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555, 0x0104U, 8U),
        Declaration("192.168.0.1", 5555, 0x0104U, 16U)};

    EXPECT_TRUE(SummarizeA2lEndpoints(declarations, false).ambiguous);
}

/**
 * @brief 只有一条记录却声明了多个实例（v9 兼容路径不展开）：fail closed。
 */
TEST(TransportSniffSummary, InstanceCountAboveOneWithoutDetailsIsAmbiguous) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555, 0x0104U, 8U, 3U)};

    const auto summary = SummarizeA2lEndpoints(declarations, false);

    EXPECT_TRUE(summary.declared);
    EXPECT_TRUE(summary.ambiguous);
    EXPECT_EQ(summary.instance_count, 3U);
}

/**
 * @brief 桥接层已判多 MODULE 歧义时透传，即使只有一条端点记录。
 */
TEST(TransportSniffSummary, ModuleAmbiguityPropagates) {
    const std::vector<A2lEndpointDeclaration> declarations{
        Declaration("192.168.0.1", 5555)};

    EXPECT_TRUE(SummarizeA2lEndpoints(declarations, true).ambiguous);
}

/**
 * @brief 完全没有声明：`declared` 为 false，不参与嗅探计划。
 */
TEST(TransportSniffSummary, EmptyDeclarationsStayUndeclared) {
    const auto summary =
        SummarizeA2lEndpoints(std::vector<A2lEndpointDeclaration>{}, false);

    EXPECT_FALSE(summary.declared);
    EXPECT_FALSE(summary.ambiguous);
    EXPECT_EQ(summary.instance_count, 0U);
}

// ---------------------------------------------------------------------------
// 诊断文本（§9.8 / T8）
// ---------------------------------------------------------------------------

/**
 * @brief 失败报告含汇总首行与逐条结论（含耗时与原因）。
 */
TEST(TransportSniffReport, FailureReportListsEveryAttempt) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);

    const auto result = SniffTransport(
        plan, VerdictPredicate(FramingVerdict::Rejected, nullptr));
    const auto report = result.FailureReport();

    EXPECT_NE(report.find("传输层嗅探失败"), std::string::npos);
    EXPECT_NE(report.find("192.168.0.1:5555"), std::string::npos);
    EXPECT_NE(report.find("192.168.0.1:5559"), std::string::npos);
    EXPECT_NE(report.find("耗时 7 ms"), std::string::npos);
    EXPECT_NE(report.find("固定结论"), std::string::npos);
    // 每条尝试占一行，行数 = 1 + 尝试数。
    std::size_t lines = 1;
    for (const char ch : report) {
        if (ch == '\n') ++lines;
    }
    EXPECT_EQ(lines, result.attempts.size() + 1U);
}

/**
 * @brief 成功时失败报告为空串。
 */
TEST(TransportSniffReport, SuccessHasEmptyFailureReport) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         A2lEndpointSummary{}, std::nullopt,
                                         std::nullopt, SniffScope::TcpOnly);

    const auto result =
        SniffTransport(plan, VerdictPredicate(FramingVerdict::Accepted,
                                              nullptr));

    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.FailureReport().empty());
}

/**
 * @brief 条目渲染：TCP 带候选名，UDP 只带端点。
 */
TEST(TransportSniffReport, DescribesEntries) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);
    ASSERT_FALSE(plan.empty());

    bool saw_tcp = false;
    bool saw_udp = false;
    for (const auto& entry : plan) {
        const auto text = DescribeSniffEntry(entry);
        if (entry.transport == TransportKind::Tcp) {
            saw_tcp = true;
            EXPECT_EQ(text.rfind("TCP 192.168.0.1:5555", 0), 0U);
            EXPECT_GT(text.size(), std::string("TCP 192.168.0.1:5555").size());
        } else {
            saw_udp = true;
            EXPECT_EQ(text, "UDP 192.168.0.1:5559");
        }
    }
    EXPECT_TRUE(saw_tcp);
    EXPECT_TRUE(saw_udp);
}

/**
 * @brief 只试过 TCP 时，汇总行不得宣称"UDP 也没响应"。
 * @details 评审问题 6：用户会照汇总行去排查一个根本没尝试过的传输层。
 */
TEST(TransportSniffReport, FailureReportNamesOnlyAttemptedTransports) {
    const auto plan =
        BuildDualSniffPlan(Declared("192.168.0.1", 5555), A2lEndpointSummary{},
                           std::nullopt, std::nullopt, SniffScope::TcpOnly);

    const auto result = SniffTransport(
        plan, VerdictPredicate(FramingVerdict::Rejected, nullptr));
    const auto report = result.FailureReport();

    EXPECT_NE(report.find("TCP 候选均未得到合法响应"), std::string::npos);
    EXPECT_EQ(report.find("UDP"), std::string::npos);
}

/**
 * @brief 只试过 UDP 时，汇总行只提 UDP。
 */
TEST(TransportSniffReport, FailureReportNamesUdpOnly) {
    const auto plan = BuildDualSniffPlan(A2lEndpointSummary{},
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);
    ASSERT_EQ(plan.size(), 1U);

    const auto result = SniffTransport(
        plan, VerdictPredicate(FramingVerdict::Inconclusive, nullptr));
    const auto report = result.FailureReport();

    EXPECT_NE(report.find("UDP 端点未得到合法响应"), std::string::npos);
    EXPECT_EQ(report.find("TCP"), std::string::npos);
}

/**
 * @brief 两路都试过时，汇总行才可以说"TCP 与 UDP 均未"。
 */
TEST(TransportSniffReport, FailureReportNamesBothTransports) {
    const auto plan = BuildDualSniffPlan(Declared("192.168.0.1", 5555),
                                         Declared("192.168.0.1", 5559),
                                         std::nullopt, std::nullopt,
                                         SniffScope::Dual);

    const auto result = SniffTransport(
        plan, VerdictPredicate(FramingVerdict::Rejected, nullptr));

    EXPECT_NE(result.FailureReport().find("TCP 与 UDP 均未得到合法响应"),
              std::string::npos);
}

/**
 * @brief 一个候选都没有时，报告不能说"对端没响应"——那是配置不全。
 */
TEST(TransportSniffReport, FailureReportOnEmptyPlan) {
    const auto result = SniffTransport(
        std::vector<SniffPlanEntry>{},
        VerdictPredicate(FramingVerdict::Rejected, nullptr));

    EXPECT_FALSE(result.Succeeded());
    EXPECT_NE(result.FailureReport().find("没有任何候选可试"),
              std::string::npos);
}

/**
 * @brief 传输层名渲染为全大写（诊断文本统一口径）。
 */
TEST(TransportSniffReport, TransportNamesAreUpperCase) {
    EXPECT_EQ(SniffTransportName(TransportKind::Tcp), "TCP");
    EXPECT_EQ(SniffTransportName(TransportKind::Udp), "UDP");
}

}  // namespace

// ---------------------------------------------------------------------------
// T6：`/include` 读取回调
// ---------------------------------------------------------------------------

/**
 * @brief 在可写目录下建一个带随机后缀的临时目录
 * @return 目录路径；无法创建时返回空
 *
 * @details 与既有测试一致：先试系统临时目录，失败再落到当前目录——沙箱下
 *          `%TEMP%` 未必可写，但构建目录一定可写。
 */
std::filesystem::path MakeScratchDir(const std::string& tag) {
    std::error_code ec;
    std::vector<std::filesystem::path> roots;
    const auto temp = std::filesystem::temp_directory_path(ec);
    if (!ec) roots.push_back(temp);
    roots.push_back(std::filesystem::current_path(ec));
    for (const auto& root : roots) {
        const auto dir = root / ("libxcp_sniff_" + tag);
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        if (!ec && std::filesystem::is_directory(dir, ec)) return dir;
    }
    return {};
}

/// @brief 写一个文本文件，失败返回 false
bool WriteScratchFile(const std::filesystem::path& path,
                      const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

/// @brief 相对路径按 `base_dir` 解析
TEST(IncludeResolverTest, ReadsRelativePathAgainstBaseDir) {
    const auto dir = MakeScratchDir("relative");
    ASSERT_FALSE(dir.empty());
    ASSERT_TRUE(WriteScratchFile(dir / "XCP.aml", "/begin A2ML\n/end A2ML\n"));

    IncludeReadOptions options;
    options.base_dir = dir;

    const auto content = ReadIncludeFile(options, "XCP.aml");
    ASSERT_TRUE(content.has_value());
    EXPECT_NE(content->find("/begin A2ML"), std::string::npos);

    std::filesystem::remove_all(dir);
}

/// @brief 绝对路径不受 `base_dir` 影响
TEST(IncludeResolverTest, ReadsAbsolutePathIgnoringBaseDir) {
    const auto dir = MakeScratchDir("absolute");
    ASSERT_FALSE(dir.empty());
    const auto file = dir / "child.aml";
    ASSERT_TRUE(WriteScratchFile(file, "child"));

    IncludeReadOptions options;
    options.base_dir = dir / "not_here";  // 故意给一个不存在的基准

    const auto content = ReadIncludeFile(options, file.string());
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, "child");

    std::filesystem::remove_all(dir);
}

/// @brief 文件不存在返回空（不抛异常）
TEST(IncludeResolverTest, MissingFileReturnsNullopt) {
    const auto dir = MakeScratchDir("missing");
    ASSERT_FALSE(dir.empty());

    IncludeReadOptions options;
    options.base_dir = dir;

    EXPECT_FALSE(ReadIncludeFile(options, "nope.aml").has_value());

    std::filesystem::remove_all(dir);
}

/// @brief 超过上限返回空，而不是截断内容
TEST(IncludeResolverTest, OversizedFileReturnsNullopt) {
    const auto dir = MakeScratchDir("oversized");
    ASSERT_FALSE(dir.empty());
    ASSERT_TRUE(WriteScratchFile(dir / "big.aml", std::string(64, 'a')));

    IncludeReadOptions options;
    options.base_dir = dir;
    options.max_bytes = 16;

    EXPECT_FALSE(ReadIncludeFile(options, "big.aml").has_value());

    std::filesystem::remove_all(dir);
}

/// @brief 目录不是普通文件，返回空
TEST(IncludeResolverTest, DirectoryReturnsNullopt) {
    const auto dir = MakeScratchDir("directory");
    ASSERT_FALSE(dir.empty());
    ASSERT_TRUE(std::filesystem::create_directories(dir / "sub"));

    IncludeReadOptions options;
    options.base_dir = dir;

    EXPECT_FALSE(ReadIncludeFile(options, "sub").has_value());

    std::filesystem::remove_all(dir);
}

/// @brief 空路径直接返回空，不去拼基准目录
TEST(IncludeResolverTest, EmptyPathReturnsNullopt) {
    IncludeReadOptions options;
    options.base_dir = std::filesystem::current_path();
    EXPECT_FALSE(ReadIncludeFile(options, "").has_value());
}

/// @brief `MakeIncludeResolver()` 产出的回调可直接交给扫描器
TEST(IncludeResolverTest, FactoryProducesUsableCallback) {
    const auto dir = MakeScratchDir("factory");
    ASSERT_FALSE(dir.empty());
    ASSERT_TRUE(WriteScratchFile(dir / "XCP.aml", "/begin A2ML\n/end A2ML\n"));

    IncludeReadOptions options;
    options.base_dir = dir;
    const auto resolver = MakeIncludeResolver(options);

    ASSERT_TRUE(static_cast<bool>(resolver));
    const auto content = resolver("XCP.aml");
    ASSERT_TRUE(content.has_value());
    EXPECT_NE(content->find("/begin A2ML"), std::string::npos);
    // 缺失文件必须落到空，扫描器据此判为不受信。
    EXPECT_FALSE(resolver("absent.aml").has_value());

    std::filesystem::remove_all(dir);
}
