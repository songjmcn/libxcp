#pragma once

/**
 * @file transport_sniff.hpp
 * @brief 传输层双路嗅探：未显式指定传输层时，对 TCP 与 UDP 各发一次
 *        CONNECT，以"首个得到合法协议层响应者"作为结论。
 *
 * @details 依据 code-plan/xcp_master_传输层与对齐从A2L推导_实施计划.md 的
 *          §9（R3 修订）与 T1/T4 实现。本文件只放**纯逻辑**：
 *          - 计划构建（哪些条目、什么顺序）：`BuildDualSniffPlan()`；
 *          - 执行与记录（首胜即停、三态保留）：`SniffTransport()`；
 *          - 端点渲染与退出码映射。
 *          真正的"连一次试试"由调用方以 `SniffAttemptFn` 注入，
 *          因此本模块不依赖 socket、不依赖 `XcpMaster`，可以被单元测试
 *          用假谓词完整覆盖。
 *
 * @par 为什么嗅探必须"两路都试"
 *   A2L 的 IF_DATA 是**静态声明**，现场常见"文件写的是 TCP、实际部署是
 *   UDP"（或反之）。只信 A2L 会立刻失败，只信默认值会连错。因而顺序由 A2L
 *   决定（声明优先），但两路都要试，结论以实际响应为准。
 *
 * @par 为什么要三态而不是布尔
 *   "对端明确拒绝"（协议层违规）与"纯超时"是两类事实：前者说明这条布局
 *   不适用于该对端，后者说明根本没有证据。压成一个 false 会把"端口写错"
 *   报成"两种传输都不支持"。三态类型沿用 `FramingVerdict`。
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cli_selection.hpp"
#include "libxcp/tcp_framing_candidates.hpp"
#include "libxcp/tcp_framing_negotiation.hpp"
#include "libxcp/tcp_framing_verdict.hpp"
#include "libxcp/transport_selection.hpp"

namespace calmcar::xcp::tools {

/**
 * @brief A2L 中**某一类**传输层的端点摘要（供计划构建使用）
 *
 * @details 只保留"能否构造一次连接尝试"所需的最小集合。刻意不复用
 *          `A2lTransportFacts`：后者还带着 TCP alignment 的原文核对结论，
 *          属于"解析阶段"的职责，不应渗进"嗅探阶段"。
 */
struct A2lEndpointSummary {
    /// @brief A2L 是否声明了该类传输层
    bool declared{false};

    /// @brief 端点主机名/地址（`declared` 为 true 时有效）
    std::string host;

    /// @brief 端点端口；0 表示不可用
    std::uint16_t port{0};

    /// @brief 该类端点的声明实例个数
    std::uint32_t instance_count{0};

    /**
     * @brief 多实例冲突，无法自动判定应连哪一个
     * @details 由调用方逐实例比较后置位。冲突时该路**不进入**嗅探计划：
     *          连错实例不会报错，只会连到另一台 ECU，现场无法定位。
     */
    bool ambiguous{false};

    /// @brief A2L 声明的传输层版本（如 0x0104）；空表示未声明
    std::optional<std::uint16_t> transport_layer_version;

    /// @brief A2L 显式声明的对齐位宽；0 表示未声明
    std::uint8_t packet_alignment_bits{0};
};

/**
 * @brief 嗅探范围
 */
enum class SniffScope : std::uint8_t {
    /// @brief 未显式指定传输层：TCP 与 UDP 都要试（计划 §9.1）
    Dual,

    /**
     * @brief 显式指定了 TCP 但没给 Profile：只试 TCP 的各候选
     * @details 对应用户已经用 `--transport tcp` 表达了传输层意图，
     *          此时再去试 UDP 等于推翻用户的显式选择。
     */
    TcpOnly,
};

/**
 * @brief 一次嗅探尝试的计划条目
 */
struct SniffPlanEntry {
    /// @brief 本次尝试的传输层类别
    TransportKind transport{TransportKind::Udp};

    /// @brief 本次尝试使用的地址
    std::string host;

    /// @brief 本次尝试使用的端口
    std::uint16_t port{0};

    /// @brief TCP 时的帧布局候选；UDP 时为空（UDP 没有帧布局概念）
    std::optional<TcpFramingCandidate> candidate;

    /// @brief 端点是否来自 A2L（false 表示来自命令行）
    bool endpoint_from_a2l{false};
};

/**
 * @brief 一次嗅探尝试的完整记录
 */
struct SniffAttempt {
    /// @brief 本次尝试的条目
    SniffPlanEntry entry;

    /// @brief 三态结论
    FramingVerdict verdict{FramingVerdict::Inconclusive};

    /// @brief 失败原因；`Accepted` 时为空
    std::string reason;

    /// @brief 本次尝试耗时（毫秒），含建立连接与判定
    long long elapsed_ms{0};
};

/**
 * @brief 双路嗅探的最终结果
 */
struct TransportSniffResult {
    /// @brief 胜出条目；全部失败时为空
    std::optional<SniffPlanEntry> selected;

    /// @brief 按尝试顺序记录的全部结果（含胜出那次）
    std::vector<SniffAttempt> attempts;

    /// @brief TCP 是否出现过 `Accepted`
    bool tcp_responded{false};

    /// @brief UDP 是否出现过 `Accepted`
    bool udp_responded{false};

    /**
     * @brief 判定是否成功
     * @return 存在胜出条目返回 true
     */
    [[nodiscard]] bool Succeeded() const noexcept {
        return selected.has_value();
    }

    /**
     * @brief 生成失败时的逐行诊断文本
     * @return 多行中文文本；成功时返回空串
     * @details 形如：
     *          `传输层嗅探失败：TCP 与 UDP 均未得到合法响应` 换行后逐行
     *          `· TCP 192.168.0.1:5555 AsamV14/8bit：未决，耗时 1000 ms`。
     */
    [[nodiscard]] std::string FailureReport() const;
};

/**
 * @brief 判定谓词：对给定条目尝试一次真实连接
 *
 * @param entry 待尝试的条目（含传输层、端点、TCP 候选）
 * @return 三态结论 + 原因 + 耗时
 *
 * @note 实现方必须在返回前彻底断开连接，否则下一次尝试会带着上一条连接的
 *       残留状态运行（与 `FramingAttemptFn` 的约定一致）。
 */
using SniffAttemptFn =
    std::function<FramingVerificationResult(const SniffPlanEntry& entry)>;

/**
 * @brief 把条目渲染为诊断文本
 * @param entry 待渲染条目
 * @return `TCP 127.0.0.1:5555 AsamV14/8bit` 或 `UDP 127.0.0.1:5555`
 */
[[nodiscard]] std::string DescribeSniffEntry(const SniffPlanEntry& entry);

/**
 * @brief 构建嗅探计划（顺序即契约，调用方不得重排）
 *
 * @param tcp A2L 的 TCP 端点摘要
 * @param udp A2L 的 UDP 端点摘要
 * @param cli_host 命令行给定的地址；空可选值表示未给
 * @param cli_port 命令行给定的端口；空可选值表示未给
 * @param scope 嗅探范围（双路 / 仅 TCP）
 * @return 有序的尝试计划；无法构造任何端点时返回空
 *
 * @details 规则（计划 §9.3 / §9.4）：
 *          - 端点：命令行给了就用它（两路共用）；未给则每一路各自取 A2L
 *            中该类传输的端点；某一路既无命令行也无 A2L 声明 → 该路跳过，
 *            且**不计为失败**（无从连接，不是"拒绝了"）；
 *          - 顺序：A2L 声明优先排序；仅声明 UDP 时 UDP 在前，其余情况
 *            TCP 在前（含"都没声明但命令行给了端点"）；
 *          - TCP 展开为多个条目：候选序列来自 `BuildTcpFramingCandidates()`，
 *            只在 8 bit 下按 TL 版本区分 V1.4/V1.5，其余保持默认顺序；
 *          - 多实例冲突（`ambiguous`）的一路**不进入**计划，由调用方给出
 *            "请显式指定端点"的诊断。
 */
[[nodiscard]] std::vector<SniffPlanEntry> BuildDualSniffPlan(
    const A2lEndpointSummary& tcp, const A2lEndpointSummary& udp,
    const std::optional<std::string>& cli_host,
    const std::optional<std::uint16_t>& cli_port, SniffScope scope);

/**
 * @brief 依次执行嗅探计划，首个 `Accepted` 胜出
 *
 * @param plan 计划条目（通常来自 `BuildDualSniffPlan()`）
 * @param attempt 判定谓词（不得为空）
 * @return 结果（`attempts` 覆盖所有已尝试条目）
 *
 * @details 语义约定：
 *          - **首胜即停**：不做"两路都成功"的仲裁——真实 ECU 上同一端点
 *            同时提供 TCP 与 UDP 服务几乎不可能，出现时按计划顺序取第一个；
 *          - **不做重试**：每个条目只尝试一次；
 *          - `Inconclusive` 不终止扫描（后面的条目仍可能明确可用），
 *            但逐项保留，供调用方区分"被拒"与"未决"；
 *          - 谓词抛出的异常按 `Inconclusive` 记录（无法证明是拒绝），
 *            异常文本进入 `reason`，异常**不会**外泄给调用方。
 */
[[nodiscard]] TransportSniffResult SniffTransport(
    const std::vector<SniffPlanEntry>& plan, const SniffAttemptFn& attempt);

/**
 * @brief 把嗅探结果映射为进程退出码
 *
 * @param result 嗅探结果
 * @return `0` 成功；`4` 至少存在一个未决条目；`3` 全部条目被拒
 * @details 复用 auto 协商的退出码语义（`kAutoNegotiationInconclusive` /
 *          `kAutoNegotiationAllRejected`）：3 = 对端在回话但都不认，
 *          用户该改配置；4 = 对端根本没回话，用户该查地址/端口/从站。
 */
[[nodiscard]] inline int SniffExitCode(
    const TransportSniffResult& result) noexcept {
    if (result.selected.has_value()) {
        return kAutoNegotiationOk;
    }
    for (const SniffAttempt& item : result.attempts) {
        if (item.verdict == FramingVerdict::Inconclusive) {
            return kAutoNegotiationInconclusive;
        }
    }
    return kAutoNegotiationAllRejected;
}

/// @brief 把传输层渲染为 `"TCP"` / `"UDP"`（诊断文本用，全大写）
[[nodiscard]] inline std::string_view SniffTransportName(
    TransportKind transport) noexcept {
    return transport == TransportKind::Tcp ? "TCP" : "UDP";
}

/**
 * @brief 把端点渲染为 `host:port`
 * @param host 地址
 * @param port 端口
 * @return 拼接结果
 */
[[nodiscard]] inline std::string RenderSniffEndpoint(const std::string& host,
                                                     std::uint16_t port) {
    return host + ':' + std::to_string(port);
}

/**
 * @brief 把条目渲染为诊断文本
 * @param entry 待渲染条目
 * @return `TCP 127.0.0.1:5555 AsamV14/8bit` 或 `UDP 127.0.0.1:5555`
 */
[[nodiscard]] inline std::string DescribeSniffEntry(
    const SniffPlanEntry& entry) {
    std::string text(SniffTransportName(entry.transport));
    text += ' ';
    text += RenderSniffEndpoint(entry.host, entry.port);
    if (entry.candidate.has_value()) {
        text += ' ';
        text += FramingCandidateName(*entry.candidate);
    }
    return text;
}

/**
 * @brief 生成失败时的逐行诊断文本
 * @return 多行中文文本；成功时返回空串
 */
[[nodiscard]] inline std::string TransportSniffResult::FailureReport() const {
    if (selected.has_value()) {
        return std::string();
    }
    std::string report = "传输层嗅探失败：TCP 与 UDP 均未得到合法响应";
    for (const SniffAttempt& item : attempts) {
        report += "\n  · ";
        report += DescribeSniffEntry(item.entry);
        report += "：";
        report += FramingVerdictName(item.verdict);
        report += "，耗时 ";
        report += std::to_string(item.elapsed_ms);
        report += " ms";
        if (!item.reason.empty()) {
            report += "，原因：";
            report += item.reason;
        }
    }
    return report;
}

/**
 * @brief 构建嗅探计划
 * @param tcp A2L 的 TCP 端点摘要
 * @param udp A2L 的 UDP 端点摘要
 * @param cli_host 命令行地址
 * @param cli_port 命令行端口
 * @param scope 嗅探范围
 * @return 有序计划；无法构造任何端点时为空
 */
[[nodiscard]] inline std::vector<SniffPlanEntry> BuildDualSniffPlan(
    const A2lEndpointSummary& tcp, const A2lEndpointSummary& udp,
    const std::optional<std::string>& cli_host,
    const std::optional<std::uint16_t>& cli_port, SniffScope scope) {
    // 命令行端点优先：给了就两路共用，不再从 A2L 取（用户显式覆盖）。
    const bool cli_endpoint_usable = cli_host.has_value() &&
                                     cli_port.has_value() &&
                                     !cli_host->empty() &&
                                     *cli_port != 0;

    // 每一路各自的端点决议；空表示"这一路无从连接"。
    const auto resolve = [&](const A2lEndpointSummary& summary)
        -> std::optional<std::pair<std::string, std::uint16_t>> {
        if (cli_endpoint_usable) {
            return std::make_pair(*cli_host, *cli_port);
        }
        // 多实例冲突时不敢替用户选：该路直接不进计划。
        if (!summary.declared || summary.ambiguous || summary.port == 0 ||
            summary.host.empty()) {
            return std::nullopt;
        }
        return std::make_pair(summary.host, summary.port);
    };

    const auto tcp_endpoint = resolve(tcp);
    const auto udp_endpoint = resolve(udp);

    // TCP 的候选序列：只在"8 bit 且声明了 TL 版本"时区分 V1.4/V1.5，
    // 其余保持默认顺序。对齐声明仅用于排序，不代表已核验。
    std::vector<TcpFramingCandidate> tcp_candidates;
    if (tcp_endpoint.has_value()) {
        TcpFramingCandidateRequest evidence;
        evidence.transport_layer_version = tcp.transport_layer_version;
        switch (tcp.packet_alignment_bits) {
            case 8:
                evidence.alignment = PacketAlignment::Bit8;
                break;
            case 16:
                evidence.alignment = PacketAlignment::Bit16;
                break;
            case 32:
                evidence.alignment = PacketAlignment::Bit32;
                break;
            default:
                evidence.alignment = PacketAlignment::Unknown;
                break;
        }
        tcp_candidates = BuildTcpFramingCandidates(evidence);
    }

    const auto make_tcp_entries = [&]() {
        std::vector<SniffPlanEntry> entries;
        for (const TcpFramingCandidate& candidate : tcp_candidates) {
            SniffPlanEntry entry;
            entry.transport = TransportKind::Tcp;
            entry.host = tcp_endpoint->first;
            entry.port = tcp_endpoint->second;
            entry.candidate = candidate;
            entry.endpoint_from_a2l = !cli_endpoint_usable;
            entries.push_back(std::move(entry));
        }
        return entries;
    };
    const auto make_udp_entry = [&]() {
        std::vector<SniffPlanEntry> entries;
        SniffPlanEntry entry;
        entry.transport = TransportKind::Udp;
        entry.host = udp_endpoint->first;
        entry.port = udp_endpoint->second;
        entry.endpoint_from_a2l = !cli_endpoint_usable;
        entries.push_back(std::move(entry));
        return entries;
    };

    std::vector<SniffPlanEntry> plan;
    if (scope == SniffScope::TcpOnly) {
        return tcp_endpoint.has_value() ? make_tcp_entries()
                                        : std::vector<SniffPlanEntry>();
    }

    // 顺序：仅声明 UDP 时 UDP 在前；其余（仅 TCP / 都声明 / 都没声明）
    // 一律 TCP 在前——8 bit 下两种 Profile 字节相同，TCP 的首个候选
    // 即可覆盖绝大多数部署，UDP 作为补充证据。
    const bool udp_first = udp_endpoint.has_value() &&
                           !tcp_endpoint.has_value();
    const auto append = [&plan](std::vector<SniffPlanEntry> entries) {
        for (SniffPlanEntry& entry : entries) {
            plan.push_back(std::move(entry));
        }
    };
    if (udp_first) {
        append(make_udp_entry());
        if (tcp_endpoint.has_value()) {
            append(make_tcp_entries());
        }
    } else {
        if (tcp_endpoint.has_value()) {
            append(make_tcp_entries());
        }
        if (udp_endpoint.has_value()) {
            append(make_udp_entry());
        }
    }
    return plan;
}

/**
 * @brief 依次执行嗅探计划，首个 `Accepted` 胜出
 * @param plan 计划条目
 * @param attempt 判定谓词
 * @return 嗅探结果
 */
[[nodiscard]] inline TransportSniffResult SniffTransport(
    const std::vector<SniffPlanEntry>& plan, const SniffAttemptFn& attempt) {
    TransportSniffResult result;
    for (const SniffPlanEntry& entry : plan) {
        FramingVerificationResult outcome;
        if (!attempt) {
            // 没有谓词就等于没有证据：按未决记录，不伪造结论。
            outcome.verdict = FramingVerdict::Inconclusive;
            outcome.reason = "未提供嗅探谓词";
        } else {
            try {
                outcome = attempt(entry);
            } catch (const std::exception& error) {
                // 谓词自身抛异常无法证明对端拒绝，只能算未决。
                outcome.verdict = FramingVerdict::Inconclusive;
                outcome.reason = std::string("嗅探异常：") + error.what();
            } catch (...) {
                outcome.verdict = FramingVerdict::Inconclusive;
                outcome.reason = "嗅探抛出未知异常";
            }
        }
        SniffAttempt record;
        record.entry = entry;
        record.verdict = outcome.verdict;
        record.reason = outcome.reason;
        record.elapsed_ms = outcome.elapsed_ms;
        result.attempts.push_back(std::move(record));
        if (outcome.verdict == FramingVerdict::Accepted) {
            if (entry.transport == TransportKind::Tcp) {
                result.tcp_responded = true;
            } else {
                result.udp_responded = true;
            }
            result.selected = entry;
            break;  // 首胜即停
        }
    }
    return result;
}

}  // namespace calmcar::xcp::tools
