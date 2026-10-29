#pragma once

/**
 * @file cli_selection.hpp
 * @brief 传输层选择的 CLI 文本层：选项解析、来源说明与摘要渲染。
 *
 * @details 第二阶段把决策逻辑提取到 `libxcp/transport_selection.hpp` 后，
 *          工具侧只保留"字符串 ↔ 枚举"的解析、中文来源说明与单行摘要渲染。
 *          这些函数只服务于本工具的 `--help`/状态输出，不属于库契约，
 *          因此不进入 `include/libxcp`。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "libxcp/protocol_version.hpp"
#include "libxcp/tcp_framing_negotiation.hpp"
#include "libxcp/tcp_wire_profile.hpp"
#include "libxcp/transport_selection.hpp"

namespace calmcar::xcp::tools {

/// @brief 把 `WireProfileSource` 渲染为中文来源说明
[[nodiscard]] inline std::string_view WireProfileSourceName(
    WireProfileSource source) noexcept {
    switch (source) {
        case WireProfileSource::Cli:
            return "--tcp-profile";
        case WireProfileSource::Preset:
            return "preset 展开";
        case WireProfileSource::Auto:
            return "--tcp-profile auto";
        case WireProfileSource::AutoLegacy:
            return "--tcp-profile auto-legacy";
        case WireProfileSource::NotApplicable:
        default:
            return "不适用";
    }
}

/// @brief 把 `EndpointSource` 渲染为中文来源说明
[[nodiscard]] inline std::string_view EndpointSourceName(
    EndpointSource source) noexcept {
    return source == EndpointSource::A2l ? "A2L 端点声明" : "--host/--port";
}

/// @brief 把 `AlignmentSource` 渲染为中文来源说明
[[nodiscard]] inline std::string_view AlignmentSourceName(
    AlignmentSource source) noexcept {
    switch (source) {
        case AlignmentSource::Cli:
            return "--packet-alignment";
        case AlignmentSource::A2l:
            return "A2L 显式声明";
        case AlignmentSource::ProfileDefault:
        default:
            return "Profile 决定";
    }
}

namespace detail {


/**
 * @brief 解析十进制无符号整数（只接受纯数字，不接受符号与空白）
 * @param text 待解析文本
 * @param out 成功时写入解析结果
 * @return true 表示解析成功且未溢出
 */
[[nodiscard]] inline bool ParseDecimal(std::string_view text,
                                       std::uint32_t& out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint32_t>(ch - '0');
        if (value > (0xFFFFFFFFU - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    out = value;
    return true;
}

}  // namespace detail


/**
 * @brief 解析 `--transport` 取值
 * @param text `"udp"` 或 `"tcp"`
 * @return 对应类别；无法识别返回空
 */
[[nodiscard]] inline std::optional<TransportKind> ParseTransportKind(
    std::string_view text) {
    if (text == "udp") {
        return TransportKind::Udp;
    }
    if (text == "tcp") {
        return TransportKind::Tcp;
    }
    return std::nullopt;
}

/**
 * @brief 解析 `--tcp-profile` 取值
 * @param text `"asam-v1.5"` 或 `"asam-v1.4"`
 * @return 对应 Profile；无法识别返回空
 *
 * @note 只接受**显式全名**：不接受 `v1.4`、`1.4`、`xcplite` 等别名。别名会让
 *       "选的是线路布局还是对端产品"再次混淆——这正是 §1.2 要避免的。
 */
[[nodiscard]] inline std::optional<TcpWireProfile> ParseTcpWireProfileText(
    std::string_view text) {
    if (text == "asam-v1.5") {
        return TcpWireProfile::AsamV15;
    }
    if (text == "asam-v1.4") {
        return TcpWireProfile::AsamV14;
    }
    return std::nullopt;
}

/**
 * @brief 解析 `--packet-alignment` 取值
 * @param text `"8"` / `"16"` / `"32"`
 * @return 对应位宽；其它取值返回空
 */
[[nodiscard]] inline std::optional<std::uint8_t> ParseAlignmentBitsText(
    std::string_view text) {
    std::uint32_t parsed = 0;
    if (!detail::ParseDecimal(text, parsed)) {
        return std::nullopt;
    }
    if (parsed != 8U && parsed != 16U && parsed != 32U) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(parsed);
}

/**
 * @brief 解析 `--protocol-version` / `--transport-version` 取值
 * @param text 形如 `"1.2"` 的 `<major>.<minor>` 文本
 * @return 对应版本；格式不符或分量超出 0..255 返回空
 *
 * @note 本函数**不**判断版本是否落在支持窗口内：`1.4` 能否使用由
 *       `VersionPolicy::Validate()` 裁决，两处给出不同判词会让用户无从判断
 *       该信哪一个。
 */
[[nodiscard]] inline std::optional<XcpProtocolVersion> ParseVersionText(
    std::string_view text) {
    const auto dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0 ||
        dot + 1 >= text.size()) {
        return std::nullopt;
    }
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    if (!detail::ParseDecimal(text.substr(0, dot), major) ||
        !detail::ParseDecimal(text.substr(dot + 1), minor)) {
        return std::nullopt;
    }
    if (major > 0xFFU || minor > 0xFFU) {
        return std::nullopt;
    }
    return XcpProtocolVersion{static_cast<std::uint8_t>(major),
                              static_cast<std::uint8_t>(minor)};
}

/**
 * @brief 渲染建链前的选择摘要（单行，供 CLI 与测试断言）
 * @param resolved 解析结果
 * @return 形如 `传输=tcp Profile=AsamV14(来源) 对齐=32bit(来源) 端点=h:p(来源)`
 *
 * @details §8.1 要求状态输出至少包含 resolved Profile、来源、alignment 与
 *          endpoint 来源；本函数只负责"连接前已知"的部分，运行期版本证据由
 *          `FormatRuntimeVersionSummary` 输出。
 */
[[nodiscard]] inline std::string FormatSelectionSummary(
    const ResolvedTransportSelection& resolved) {
    std::string text = "传输=";
    text += TransportKindName(resolved.transport);
    text += " Profile=";
    if (resolved.transport == TransportKind::Tcp) {
        text += TcpWireProfileName(resolved.wire_profile);
        text += '(';
        text += WireProfileSourceName(resolved.wire_profile_source);
        text += ')';
    } else {
        text += "不适用";
    }
    text += " 对齐=";
    if (resolved.alignment == PacketAlignment::Unknown) {
        text += "未确定";
    } else {
        text += std::to_string(AlignmentBytes(resolved.alignment) * 8U);
        text += "bit(";
        text += AlignmentSourceName(resolved.alignment_source);
        text += ')';
    }
    text += " 端点=";
    text += resolved.host;
    text += ':';
    text += std::to_string(resolved.port);
    text += '(';
    text += EndpointSourceName(resolved.endpoint_source);
    text += ')';
    return text;
}

/// @brief 运行期版本证据（建链后由 `XcpMaster` 的快照填充）
struct RuntimeVersionStatus {
    /// @brief GET_VERSION 的结果描述；如 `"可用"` 或 `"不可用:ERR_CMD_UNKNOWN"`
    std::string get_version_outcome{"未探测"};

    XcpProtocolVersion pl_reported{};   ///< 协议层上报版本
    XcpProtocolVersion pl_effective{};  ///< 协议层生效版本
    XcpProtocolVersion tl_reported{};   ///< 传输层上报版本
    XcpProtocolVersion tl_effective{};  ///< 传输层生效版本
    bool pl_conservative{false};  ///< 协议层是否处于保守能力状态
    bool tl_conservative{false};  ///< 传输层是否处于保守能力状态
};

/**
 * @brief 渲染运行期版本摘要（单行）
 * @param status 运行期版本证据
 * @return 形如 `GET_VERSION=不可用:ERR_CMD_UNKNOWN PL=1.2/1.2 TL=1.4/1.4`
 *
 * @details `reported/effective` 两值并排是刻意的：§8.1 要求区分"对端声称的
 *          版本"与"本库实际据此生效的能力版本"，封顶与保守策略都会让两者
 *          不同，只打印其中一个必然误导。
 */
[[nodiscard]] inline std::string FormatRuntimeVersionSummary(
    const RuntimeVersionStatus& status) {
    std::string text = "GET_VERSION=";
    text += status.get_version_outcome;
    text += " PL=";
    text += status.pl_reported.ToString();
    text += '/';
    text += status.pl_effective.ToString();
    if (status.pl_conservative) {
        text += "(conservative)";
    }
    text += " TL=";
    text += status.tl_reported.ToString();
    text += '/';
    text += status.tl_effective.ToString();
    if (status.tl_conservative) {
        text += "(conservative)";
    }
    return text;
}

/// @brief auto 协商成功时使用的退出码
inline constexpr int kAutoNegotiationOk = 0;
/// @brief auto 协商"全部候选都被对端拒绝"时使用的退出码
inline constexpr int kAutoNegotiationAllRejected = 3;
/// @brief auto 协商"存在未决候选（对端无任何协议层回应）"时使用的退出码
inline constexpr int kAutoNegotiationInconclusive = 4;

/**
 * @brief 把 auto 协商结果映射为进程退出码
 * @param outcome 协商结果（含全部尝试记录）
 * @return `0` 成功；`4` 至少存在一个未决候选；`3` 全部候选被拒
 *
 * @details 退出码是自动化脚本唯一可靠的判据，因此必须把两种情况分开：
 *          - 全部被拒（3）：对端在回话，只是布局不对——用户该改配置；
 *          - 存在未决（4）：对端根本没回话——用户该查地址/端口/从站是否在跑。
 *          混为同一码会把"端口写错"误报成"四种布局都不支持"。
 * @note 只要存在一个未决候选就返回 4：未决意味着"无法排除"，此时断言
 *       "全部被拒"不成立。
 */
[[nodiscard]] inline int TcpNegotiationExitCode(
    const TcpFramingNegotiationResult& outcome) noexcept {
    if (outcome.selected.has_value()) {
        return kAutoNegotiationOk;
    }
    for (const auto& item : outcome.attempts) {
        if (item.verdict == FramingVerdict::Inconclusive) {
            return kAutoNegotiationInconclusive;
        }
    }
    return kAutoNegotiationAllRejected;
}

}  // namespace calmcar::xcp::tools
