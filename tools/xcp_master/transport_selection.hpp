#pragma once

/**
 * @file transport_selection.hpp
 * @brief CLI 传输层选择解析：把「用户选项 + A2L 事实」解析为可审计的连接参数。
 *
 * 依据 `code-plan/TCP传输_实施计划_B-T1至B-T5.md` §8.1 实现（批次 B-T5）。
 *
 * @par 为什么要独立成纯函数
 *   §8.1 的九条规则里有七条是**纯决策**（选谁、冲突报错、拒绝推断），与
 *   socket、A2L 解析库、控制台输出都无关。把它们压在 `main.cpp` 的建链流程
 *   里，就只能靠"跑一遍二进制看输出"来验证；抽成纯函数后，规则的每一条
 *   都能被单元测试直接钉住，也不需要在测试里拉起真实从站。
 *
 * @par 本层不做什么
 *   - 不读 A2L：A2L 事实由调用方（桥接层）提取后以 `A2lTransportFacts` 传入；
 *   - 不猜 Profile：`TcpWireProfile` 必须由 `--tcp-profile` 或 preset 显式给出，
 *     A2L 的 profile hint **只**进入诊断文本（§1.2 禁止事项）；
 *   - 不因 `--tcp-profile asam-v1.4` 反推协议层版本（§8.1 规则 6）。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libxcp/protocol_types.hpp"
#include "libxcp/protocol_version.hpp"
#include "libxcp/tcp_wire_profile.hpp"

namespace calmcar::xcp::tools {

/// @brief CLI 选择的传输层类别
enum class TransportKind : std::uint8_t {
    Udp = 0,  ///< XCP on UDP/IP（默认，保持既有行为，§8.1 规则 9）
    Tcp = 1,  ///< XCP on TCP/IP（B-T5 新增）
};

/// @brief Wire Profile 的来源，用于状态输出与审计
enum class WireProfileSource : std::uint8_t {
    NotApplicable = 0,  ///< UDP 传输：不存在 Wire Profile 概念
    Cli,                ///< 来自 `--tcp-profile`
    Preset,             ///< 来自 `--preset xcplite` 的原子展开
};

/// @brief 端点（host/port）的来源
enum class EndpointSource : std::uint8_t {
    Cli = 0,  ///< 来自 `--host` / `--port`
    A2l,      ///< 来自 A2L 的 `XCP_ON_TCP_IP` 声明
};

/// @brief PACKET_ALIGNMENT 的来源
enum class AlignmentSource : std::uint8_t {
    Cli = 0,     ///< 来自 `--packet-alignment`
    A2l,         ///< 来自 A2L 的显式 `PACKET_ALIGNMENT` 声明
    ProfileDefault,  ///< Profile 决定（AsamV14 无 LEN 外 Tail → 恒 8 bit）
};

/// @brief 用户侧原始请求（全部来自命令行，未做任何补全）
struct TransportSelectionRequest {
    TransportKind transport = TransportKind::Udp;  ///< `--transport`

    /// @brief `--tcp-profile`；未给出表示用户没有显式选择
    std::optional<TcpWireProfile> tcp_profile;

    /// @brief `--packet-alignment` 的原始位宽（8/16/32）
    std::optional<std::uint8_t> packet_alignment_bits;

    /// @brief `--protocol-version`
    std::optional<XcpProtocolVersion> protocol_version;

    /// @brief `--transport-version`
    std::optional<XcpProtocolVersion> transport_version;

    /// @brief `--host`；与 `port` 必须成对出现（§8.1 规则 4）
    std::optional<std::string> host;

    /// @brief `--port`
    std::optional<std::uint16_t> port;

    /// @brief `--preset xcplite`：展开为 XCPlite 部署的固定组合（§8.1 规则 7）
    bool preset_xcplite = false;
};

/**
 * @brief A2L 中**单个**传输层端点的原始事实
 *
 * @details 多实例判定需要逐实例比较，不能只看"第一个"的字段（`A2lTransportFacts`
 *          的 port/host 是"选中的那一个"）。这里保留的字段刻意只有判定所需的最小
 *          集合：决定"是否连到同一目标"的全部要素。
 */
struct A2lEndpointInstance {
    std::string host;                        ///< 主机名/地址（已按回退链解析）
    std::uint16_t port = 0;                  ///< 端口
    std::uint8_t packet_alignment_bits = 0;  ///< 显式声明的位宽；0=未声明
    std::uint16_t transport_version = 0;     ///< `XCP_ON_*` 块首版本；0=未声明
};

/**
 * @brief 判断两个端点实例是否指向**同一线目标**（可安全去重）
 *
 * @details 只比较会改变线路行为的事实：host、port、对齐位宽、TL 版本。
 *          对齐位宽为 0 与"未声明 8 bit"在数值上都是 0，因此这里不区分声明
 *          状态——去重的目的是"配置是否唯一"，不是"证据强度是否相同"。
 * @param lhs 左实例
 * @param rhs 右实例
 * @return 四要素全等返回 true
 */
[[nodiscard]] inline bool SameWireTarget(const A2lEndpointInstance& lhs,
                                         const A2lEndpointInstance& rhs) noexcept {
    return lhs.port == rhs.port && lhs.host == rhs.host &&
           lhs.packet_alignment_bits == rhs.packet_alignment_bits &&
           lhs.transport_version == rhs.transport_version;
}

/**
 * @brief 某一传输层类别在 A2L 中的端点事实
 *
 * @details 由调用方按 `--transport` 过滤后传入：TCP 连接只接受 TCP 端点事实，
 *          不得把 UDP 端点的端口/对齐"借"给 TCP 连接（§8.1 规则 5）。
 */
struct A2lTransportFacts {
    /// @brief A2L 是否声明了该 transport kind 的端点
    bool declared = false;

    /// @brief 端点端口（`declared` 为 true 时有效）
    std::uint16_t port = 0;

    /// @brief 端点主机名（`HOST_NAME` → `ADDRESS` → `IPV6` 的既有回退结果）
    std::string host;

    /// @brief `PACKET_ALIGNMENT` 的显式声明位宽；0 表示未声明
    std::uint8_t packet_alignment_bits = 0;

    /// @brief A2L 是否**显式**写了 `PACKET_ALIGNMENT`（区分未声明与声明 8 bit）
    bool alignment_declared = false;

    /// @brief 该类传输层的实例个数（>1 表示多实例，需调用方决策）
    std::uint32_t instance_count = 0;

    /// @brief 同类端点的**逐实例**明细（按 A2L 声明顺序）
    /// @details 空表示调用方没有提供明细：此时无法判断多实例是否冲突，按
    ///          既有行为继续（仅提示）。计划 §7.3 要求"多个 TCP 实例且 CLI
    ///          未覆盖时 fail closed"，判定只能靠本列表。
    std::vector<A2lEndpointInstance> instances;

    /// @brief A2L 静态声明的 Protocol Layer 版本（弱证据，仅供诊断）
    std::optional<XcpProtocolVersion> protocol_version;

    /// @brief A2L 静态声明的 Transport Layer 版本（弱证据，仅供诊断）
    std::optional<XcpProtocolVersion> transport_version;

    /// @brief A2L profile 诊断候选（**禁止**参与任何自动选择，§1.2）
    std::string profile_hint;

    /// @brief A2L 是否声明了**另一类**传输层（用于给出更准确的错误文本）
    bool other_kind_declared = false;
};

/// @brief 解析结果：直接可用的连接参数 + 完整来源信息
struct ResolvedTransportSelection {
    TransportKind transport = TransportKind::Udp;        ///< 传输层类别
    TcpWireProfile wire_profile = TcpWireProfile::Unknown;  ///< Wire Profile（UDP 时无意义）
    WireProfileSource wire_profile_source = WireProfileSource::NotApplicable;

    PacketAlignment alignment = PacketAlignment::Unknown;  ///< 生效的 PACKET_ALIGNMENT
    AlignmentSource alignment_source = AlignmentSource::ProfileDefault;

    EndpointSource endpoint_source = EndpointSource::Cli;  ///< 端点来源
    std::string host;                                      ///< 生效远端地址
    std::uint16_t port = 0;                                ///< 生效远端端口

    /// @brief 传给 `VersionPolicy` 的显式协议层版本（未给出则为空）
    std::optional<XcpProtocolVersion> protocol_version;

    /// @brief 传给 `VersionPolicy` 的显式传输层版本（未给出则为空）
    std::optional<XcpProtocolVersion> transport_version;

    /// @brief 是否由 preset 展开得到（状态输出必须展示展开值及来源）
    bool preset_expanded = false;

    /// @brief 诊断信息（不阻断流程的告知项，按顺序输出）
    std::vector<std::string> notes;
};

/// @brief 把 `TransportKind` 渲染为 `"udp"` / `"tcp"`
[[nodiscard]] inline std::string_view TransportKindName(
    TransportKind kind) noexcept {
    return kind == TransportKind::Tcp ? "tcp" : "udp";
}

/// @brief 把 `WireProfileSource` 渲染为中文来源说明
[[nodiscard]] inline std::string_view WireProfileSourceName(
    WireProfileSource source) noexcept {
    switch (source) {
        case WireProfileSource::Cli:
            return "--tcp-profile";
        case WireProfileSource::Preset:
            return "preset 展开";
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

/// @brief 把端点实例渲染成 `host:port`（附对齐与 TL 版本，便于人工核对）
/// @param instance 端点实例
/// @return 形如 `192.168.0.1:5555(align=8bit)/TL 1.4` 的文本
[[nodiscard]] inline std::string RenderEndpointTarget(
    const A2lEndpointInstance& instance) {
    std::string text = instance.host;
    text += ':';
    text += std::to_string(instance.port);
    text += "(align=";
    text += instance.packet_alignment_bits == 0
                ? std::string("未声明")
                : std::to_string(instance.packet_alignment_bits) + "bit";
    text += ")";
    if (instance.transport_version != 0) {
        text += "/TL ";
        text += std::to_string(instance.transport_version >> 8U);
        text += '.';
        text += std::to_string(instance.transport_version & 0xFFU);
    }
    return text;
}

/// @brief 把位宽收敛为强类型；非 8/16/32 返回 `Unknown`
[[nodiscard]] inline PacketAlignment AlignmentFromBits(
    std::uint8_t bits) noexcept {
    return ParsePacketAlignment(bits);
}

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
 * @brief 按 §8.1 规则解析传输层选择
 *
 * @param request 用户侧原始请求
 * @param a2l 与 `request.transport` 同类的 A2L 端点事实（调用方已过滤）
 * @param out_error 失败时写入中文错误文本（可为空指针）
 * @return 解析成功返回结果；失败返回空可选值
 *
 * @details 逐条对应 §8.1：
 *  - 规则 1：TCP 必须显式给出 Profile，缺失即失败（A2L hint 不具选择权）；
 *  - 规则 2：AsamV15 的 alignment 必须"CLI > A2L 显式声明"，两者皆无则失败；
 *  - 规则 3：AsamV14 下 `--packet-alignment 16/32` 直接报错——该 Profile
 *    不存在 LEN 外 Tail，接受这个选项等于默许一个不会被执行的语义；
 *  - 规则 4：host/port 必须成对；TCP 下两者皆缺才回落到 A2L 端点；
 *  - 规则 5：端点、对齐、Profile 必须同属一个 transport kind；
 *  - 规则 6：PL 与 TL 各自独立，绝不互相推导；
 *  - 规则 7：`--preset xcplite` 原子展开，与显式选项冲突时报错而非覆盖。
 *
 * @note 本函数**不**校验版本是否落在本库支持窗口内——那是
 *       `VersionPolicy::Validate()` 的职责，避免同一规则在两处给出不同判词。
 */
[[nodiscard]] inline std::optional<ResolvedTransportSelection>
ResolveTransportSelection(const TransportSelectionRequest& request,
                          const A2lTransportFacts& a2l,
                          std::string* out_error) {
    const auto fail = [out_error](std::string message) {
        if (out_error != nullptr) {
            *out_error = std::move(message);
        }
        return std::nullopt;
    };

    ResolvedTransportSelection resolved;
    resolved.transport = request.transport;

    // ---- 规则 7：preset 原子展开 -------------------------------------------
    // preset 先把"缺省值"填好，随后显式选项逐项覆盖；一旦显式选项与展开值
    // 矛盾就报错。理由是 preset 的意义在于"这一整套是 XCPlite 的部署事实"，
    // 任一项被单独改掉，组合本身就不再成立。
    if (request.preset_xcplite && request.transport != TransportKind::Tcp) {
        return fail("--preset xcplite 描述的是 XCPlite 的 TCP 部署，与 "
                    "--transport udp 冲突");
    }
    std::optional<TcpWireProfile> effective_profile = request.tcp_profile;
    std::optional<XcpProtocolVersion> effective_pl = request.protocol_version;
    std::optional<XcpProtocolVersion> effective_tl = request.transport_version;

    if (request.preset_xcplite) {
        resolved.preset_expanded = true;
        constexpr TcpWireProfile kPresetProfile = TcpWireProfile::AsamV14;
        const XcpProtocolVersion kPresetPl = kProtocolVersion12;
        const XcpProtocolVersion kPresetTl = kTransportVersion14;

        if (effective_profile.has_value() &&
            *effective_profile != kPresetProfile) {
            return fail("--preset xcplite 固定使用 asam-v1.4，与显式 "
                        "--tcp-profile 冲突");
        }
        if (effective_pl.has_value() && !(*effective_pl == kPresetPl)) {
            return fail("--preset xcplite 固定协议层 1.2，与显式 "
                        "--protocol-version 冲突");
        }
        if (effective_tl.has_value() && !(*effective_tl == kPresetTl)) {
            return fail("--preset xcplite 固定传输层 1.4，与显式 "
                        "--transport-version 冲突");
        }
        effective_profile = kPresetProfile;
        effective_pl = kPresetPl;
        effective_tl = kPresetTl;
    }

    // ---- 规则 1：TCP 必须显式选择 Profile ----------------------------------
    if (request.transport == TransportKind::Tcp) {
        if (!effective_profile.has_value() ||
            !IsTcpWireProfileResolved(*effective_profile)) {
            return fail(
                "TCP 传输必须显式指定 --tcp-profile asam-v1.5|asam-v1.4："
                "线路布局无法从版本号、A2L 或首帧内容推断");
        }
        resolved.wire_profile = *effective_profile;
        resolved.wire_profile_source = request.preset_xcplite
                                           ? WireProfileSource::Preset
                                           : WireProfileSource::Cli;
    } else if (effective_profile.has_value()) {
        // UDP 上给 TCP Profile 是无意义配置，明确拒绝而不是静默忽略
        return fail("--tcp-profile 只对 --transport tcp 有效");
    }

    // ---- 规则 4 的前置：host/port 必须成对 ---------------------------------
    const bool host_given = request.host.has_value() && !request.host->empty();
    const bool port_given = request.port.has_value() && request.port.value() != 0;
    if (host_given != port_given) {
        return fail("--host 与 --port 必须成对出现（只给其中一个无法确定端点）");
    }

    // ---- 规则 5 的诊断补充：跨 transport kind 的误配 -----------------------
    // 必须在端点回落**之前**判断：否则"TCP 无端点"会先命中通用错误文本，
    // 用户看不到"A2L 里其实只声明了 UDP"这个真正原因。
    if (request.transport == TransportKind::Tcp && !a2l.declared &&
        a2l.other_kind_declared && !host_given) {
        return fail("A2L 只声明了 UDP 端点，与 --transport tcp 不属于同一 "
                    "transport kind；请用 --host/--port 显式给出 TCP 端点");
    }

    // ---- 规则 4 补充（计划 §7.3）：多实例在 CLI 未覆盖时 fail closed -------
    // 为什么必须拒绝而不是"先到先得"：选错实例不会报错，只会连到另一台 ECU
    // 的地址/端口，现象是"连接超时"或"读到的值不对"，现场无法定位到端点选择。
    // 去重是必要的放宽：同一份配置声明两次（例如生成器重复输出）不该阻断。
    if (!host_given && a2l.instances.size() > 1U) {
        std::vector<std::string> distinct;
        for (const A2lEndpointInstance& instance : a2l.instances) {
            const bool seen = std::any_of(
                distinct.begin(), distinct.end(),
                [&instance](const std::string& rendered) {
                    return rendered == detail::RenderEndpointTarget(instance);
                });
            if (!seen) {
                distinct.push_back(detail::RenderEndpointTarget(instance));
            }
        }
        if (distinct.size() > 1U) {
            std::string detail_text;
            for (const std::string& one : distinct) {
                if (!detail_text.empty()) {
                    detail_text += "、";
                }
                detail_text += one;
            }
            return fail("A2L 声明了 " + std::to_string(distinct.size()) +
                        " 个不同的同类端点（" + detail_text +
                        "），无法自动判定应连接哪一个；请用 --host/--port "
                        "显式指定");
        }
        // 配置完全相同的重复声明：允许，但必须告知
        resolved.notes.push_back(
            "A2L 重复声明了 " + std::to_string(a2l.instances.size()) +
            " 个相同的端点，按同一目标处理：" +
            detail::RenderEndpointTarget(a2l.instances.front()));
    }

    // ---- 规则 4 + 规则 5：端点解析 -----------------------------------------
    if (host_given && port_given) {
        resolved.host = *request.host;
        resolved.port = request.port.value();
        resolved.endpoint_source = EndpointSource::Cli;
    } else if (request.transport == TransportKind::Tcp && a2l.declared) {
        // 规则 5：只接受 TCP 端点，且必须是同一次 IF_DATA 里的那份
        resolved.host = a2l.host;
        resolved.port = a2l.port;
        resolved.endpoint_source = EndpointSource::A2l;
        if (resolved.host.empty() || resolved.port == 0) {
            return fail("A2L 的 XCP_ON_TCP_IP 端点缺少可用的地址或端口");
        }
        resolved.notes.push_back(
            "端点取自 A2L XCP_ON_TCP_IP 声明：" + resolved.host + ":" +
            std::to_string(resolved.port));
    } else {
        // UDP 保持既有契约：host/port 必填，不做 A2L 自动选择（规则 9）
        if (request.transport == TransportKind::Tcp) {
            return fail("TCP 传输需要 --host/--port，或 A2L 中声明 "
                        "XCP_ON_TCP_IP 端点");
        }
        return fail("--a2l, --host 与 --port 为必填项");
    }

    // ---- 规则 2 / 规则 3：PACKET_ALIGNMENT ---------------------------------
    const bool cli_alignment_given =
        request.packet_alignment_bits.has_value();
    std::optional<PacketAlignment> cli_alignment;
    if (cli_alignment_given) {
        cli_alignment = detail::AlignmentFromBits(*request.packet_alignment_bits);
        if (*cli_alignment == PacketAlignment::Unknown) {
            return fail("--packet-alignment 只接受 8、16 或 32");
        }
    }

    if (request.transport == TransportKind::Tcp &&
        resolved.wire_profile == TcpWireProfile::AsamV14) {
        // 规则 3：AsamV14 没有 LEN 外 Tail。接受 16/32 等于默许一个不会被
        // 执行的语义，故直接拒绝；8 bit 语义上等价于"不填充"，可接受。
        if (cli_alignment.has_value() &&
            *cli_alignment != PacketAlignment::Bit8) {
            return fail(
                "asam-v1.4 不存在 LEN 外 Tail，--packet-alignment 16/32 与之"
                "冲突；该 Profile 下对齐恒为 8 bit");
        }
        resolved.alignment = PacketAlignment::Bit8;
        resolved.alignment_source = AlignmentSource::ProfileDefault;
        if (a2l.alignment_declared &&
            detail::AlignmentFromBits(a2l.packet_alignment_bits) !=
                PacketAlignment::Bit8 &&
            detail::AlignmentFromBits(a2l.packet_alignment_bits) !=
                PacketAlignment::Unknown) {
            // A2L 声明与 Profile 不矛盾（v1.4 的对齐在 LEN 内由对端自行补齐），
            // 但必须让用户看见"这个声明不会改变本侧 framing"。
            resolved.notes.push_back(
                "A2L 声明了 PACKET_ALIGNMENT " +
                std::to_string(a2l.packet_alignment_bits) +
                " bit；asam-v1.4 的 framing 不使用该值（无 LEN 外 Tail）");
        }
    } else if (request.transport == TransportKind::Tcp) {
        // 规则 2：AsamV15 必须有对齐依据
        if (cli_alignment.has_value()) {
            resolved.alignment = *cli_alignment;
            resolved.alignment_source = AlignmentSource::Cli;
        } else if (a2l.alignment_declared) {
            const PacketAlignment from_a2l =
                detail::AlignmentFromBits(a2l.packet_alignment_bits);
            if (from_a2l == PacketAlignment::Unknown) {
                return fail("A2L 声明了 PACKET_ALIGNMENT 但取值无法识别（仅支持 "
                            "PACKET_ALIGNMENT_8/16/32），asam-v1.5 需要明确的对齐值");
            }
            resolved.alignment = from_a2l;
            resolved.alignment_source = AlignmentSource::A2l;
        } else {
            return fail(
                "asam-v1.5 需要 PACKET_ALIGNMENT：请用 --packet-alignment 显式"
                "给出，或在 A2L 中声明 PACKET_ALIGNMENT_x（未声明不得假定 8 bit）");
        }
    } else {
        // UDP：保持既有"CLI 覆盖、否则由调用方回填 A2L/默认"的行为
        if (cli_alignment.has_value()) {
            resolved.alignment = *cli_alignment;
            resolved.alignment_source = AlignmentSource::Cli;
        } else if (a2l.alignment_declared) {
            const PacketAlignment from_a2l =
                detail::AlignmentFromBits(a2l.packet_alignment_bits);
            if (from_a2l != PacketAlignment::Unknown) {
                resolved.alignment = from_a2l;
                resolved.alignment_source = AlignmentSource::A2l;
            }
        }
        if (resolved.alignment == PacketAlignment::Unknown) {
            // 既有行为：UDP 未声明时按 8 bit 运行并明确告知依据（T-05）
            resolved.alignment = PacketAlignment::Bit8;
            resolved.alignment_source = AlignmentSource::ProfileDefault;
            resolved.notes.push_back(
                "A2L 未声明 PACKET_ALIGNMENT，按 8 bit 运行");
        }
    }

    // ---- 规则 6：PL / TL 独立，不做任何互相推导 ----------------------------
    resolved.protocol_version = effective_pl;
    resolved.transport_version = effective_tl;

    // A2L 的静态版本声明只记录为诊断：它是弱证据，且与运行时证据冲突时
    // 必须以运行时为准（`VersionEvidence` 的既有裁决）。
    if (a2l.transport_version.has_value() &&
        resolved.transport_version.has_value() &&
        !(*a2l.transport_version == *resolved.transport_version)) {
        resolved.notes.push_back(
            "A2L 声明的 Transport Layer 版本与显式配置不同（A2L " +
            a2l.transport_version->ToString() + " vs 配置 " +
            resolved.transport_version->ToString() + "），以运行时为准");
    }

    if (!a2l.profile_hint.empty()) {
        resolved.notes.push_back(
            "A2L profile 候选=" + a2l.profile_hint +
            "（仅诊断，不参与 Profile 选择）");
    }

    if (a2l.instance_count > 1U) {
        resolved.notes.push_back(
            "A2L 声明了 " + std::to_string(a2l.instance_count) +
            " 个同类端点，当前使用第一个");
    }

    if (out_error != nullptr) {
        out_error->clear();
    }
    return resolved;
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
    text += std::to_string(AlignmentBytes(resolved.alignment) * 8U);
    text += "bit(";
    text += AlignmentSourceName(resolved.alignment_source);
    text += ") 端点=";
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

}  // namespace calmcar::xcp::tools