/**
 * @file transport_selection.cpp
 * @brief 传输层选择解析的实现（见 `libxcp/transport_selection.hpp`）。
 *
 * @details 端点渲染与位宽换算只是本文件内部的呈现/换算助手，不对外暴露；
 *          决策规则本身逐条对应 §8.1，可被单元测试直接钉住。
 */

#include "libxcp/transport_selection.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace calmcar::xcp {
namespace {


/// @brief 把端点实例渲染成 `host:port`（附对齐与 TL 版本，便于人工核对）
/// @param instance 端点实例
/// @return 形如 `192.168.0.1:5555(align=8bit)/TL 1.4` 的文本
[[nodiscard]] std::string RenderEndpointTarget(
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
[[nodiscard]] PacketAlignment AlignmentFromBits(
    std::uint8_t bits) noexcept {
    return ParsePacketAlignment(bits);
}

}  // namespace

/// @brief 把 `TransportKind` 渲染为 `"udp"` / `"tcp"`
std::string_view TransportKindName(TransportKind kind) noexcept {
    return kind == TransportKind::Tcp ? "tcp" : "udp";
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
[[nodiscard]] std::optional<ResolvedTransportSelection>
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

    // ---- 规则 8：自动模式的前置校验 ---------------------------------------
    // 两种自动模式都与固定线路布局的参数互斥；严格模式在发现已知歧义时
    // 直接拒绝，只有明确选择旧模式才允许上层试连。
    if (request.tcp_profile_auto && request.tcp_profile_auto_legacy) {
        return fail("--tcp-profile 不能同时给出 auto 与 auto-legacy");
    }
    if (request.tcp_profile_auto || request.tcp_profile_auto_legacy) {
        const std::string mode = request.tcp_profile_auto ? "auto" : "auto-legacy";
        if (request.transport != TransportKind::Tcp) {
            return fail("--tcp-profile " + mode + " 只对 --transport tcp 有效");
        }
        if (request.preset_xcplite) {
            return fail("--tcp-profile " + mode + " 与 --preset xcplite 冲突：preset "
                        "已固定 asam-v1.4 与该部署的版本组合");
        }
        if (request.packet_alignment_bits.has_value()) {
            return fail("--tcp-profile " + mode +
                        " 与 --packet-alignment 冲突：自动模式不接受显式对齐");
        }
        if (request.tcp_profile.has_value()) {
            return fail("--tcp-profile 不能同时给出 " + mode + " 与具体 Profile");
        }
        // 严格 auto 的证据收敛已在 `tcp_framing_candidates.hpp` +
        // `tcp_framing_verification.hpp` 中实现：候选按证据排序，每个候选
        // 新建连接做协议层取证，只有"三次握手的字节能被对端正常解释"才接受。
        // 因此这里不再拒绝，而是交给上层按候选序列验证。
        resolved.wire_profile_auto = true;
        resolved.wire_profile = TcpWireProfile::Unknown;
        resolved.alignment = PacketAlignment::Unknown;
        if (request.tcp_profile_auto) {
            resolved.wire_profile_source = WireProfileSource::Auto;
            resolved.notes.push_back(
                "自动模式（auto）：按证据顺序为每个候选新建连接并验证，首个结论为"
                "「接受」的候选生效；全部结束时区分「被拒」与「未决」");
        } else {
            resolved.wire_profile_source = WireProfileSource::AutoLegacy;
            resolved.notes.push_back(
                "警告：--tcp-profile auto-legacy 使用旧的首成功试连；存在线路歧义，"
                "成功不证明选择正确，超时等失败也不证明候选不受支持");
        }
    }

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
        if (resolved.wire_profile_auto) {
            // auto 已在规则 8 置位：此处不得再用"必须显式 Profile"拦截，
            // 也不得填任何默认值——空着由试连结果回填。
        } else if (!effective_profile.has_value() ||
                   !IsTcpWireProfileResolved(*effective_profile)) {
            return fail(
                "TCP 传输必须显式指定 --tcp-profile asam-v1.5|asam-v1.4，或用 "
                "--tcp-profile auto 让工具按候选逐个验证；线路布局不能从版本号、"
                "A2L 或首帧内容直接推断");
        } else {
            resolved.wire_profile = *effective_profile;
            resolved.wire_profile_source = request.preset_xcplite
                                               ? WireProfileSource::Preset
                                               : WireProfileSource::Cli;
        }
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
                    return rendered == RenderEndpointTarget(instance);
                });
            if (!seen) {
                distinct.push_back(RenderEndpointTarget(instance));
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
        // 对齐事实不可信时，相同的渲染结果不证明实例配置等价。
        if (a2l.instance_alignment_unverified) {
            return fail("A2L 存在多个同类端点，但 alignment 未核验，无法确认"
                        "实例等价；请用 --host/--port 显式指定");
        }
        // 配置完全相同的重复声明：允许，但必须告知
        resolved.notes.push_back(
            "A2L 重复声明了 " + std::to_string(a2l.instances.size()) +
            " 个相同的端点，按同一目标处理：" +
            RenderEndpointTarget(a2l.instances.front()));
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
        cli_alignment = AlignmentFromBits(*request.packet_alignment_bits);
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
            AlignmentFromBits(a2l.packet_alignment_bits) !=
                PacketAlignment::Bit8 &&
            AlignmentFromBits(a2l.packet_alignment_bits) !=
                PacketAlignment::Unknown) {
            // A2L 声明与 Profile 不矛盾（v1.4 的对齐在 LEN 内由对端自行补齐），
            // 但必须让用户看见"这个声明不会改变本侧 framing"。
            resolved.notes.push_back(
                "A2L 声明了 PACKET_ALIGNMENT " +
                std::to_string(a2l.packet_alignment_bits) +
                " bit；asam-v1.4 的 framing 不使用该值（无 LEN 外 Tail）");
        }
    } else if (request.transport == TransportKind::Tcp &&
               resolved.wire_profile_auto) {
        // auto：对齐由胜出候选决定（候选本身已携带 8/16/32 bit），
        // 此处不做任何解析，也不校验 A2L 声明。
    } else if (request.transport == TransportKind::Tcp) {
        // 规则 2：AsamV15 必须有对齐依据；未验证的声明不能作为依据。
        if (cli_alignment.has_value()) {
            resolved.alignment = *cli_alignment;
            resolved.alignment_source = AlignmentSource::Cli;
        } else if (a2l.alignment_declared && !a2l.instance_alignment_unverified) {
            const PacketAlignment from_a2l =
                AlignmentFromBits(a2l.packet_alignment_bits);
            if (from_a2l == PacketAlignment::Unknown) {
                return fail("A2L 声明了 PACKET_ALIGNMENT 但取值无法识别（仅支持 "
                            "PACKET_ALIGNMENT_8/16/32），asam-v1.5 需要明确的对齐值");
            }
            resolved.alignment = from_a2l;
            resolved.alignment_source = AlignmentSource::A2l;
        } else {
            if (a2l.instance_alignment_unverified) {
                return fail("asam-v1.5 的 A2L alignment 未核验；请用 "
                            "--packet-alignment 显式给出 8、16 或 32");
            }
            return fail(
                "asam-v1.5 需要 PACKET_ALIGNMENT：请用 --packet-alignment 显式"
                "给出，或使用已核验 A2L 声明（未声明不得假定 8 bit）");
        }
    } else {
        // UDP：保持既有"CLI 覆盖、否则由调用方回填 A2L/默认"的行为
        if (cli_alignment.has_value()) {
            resolved.alignment = *cli_alignment;
            resolved.alignment_source = AlignmentSource::Cli;
        } else if (a2l.alignment_declared) {
            const PacketAlignment from_a2l =
                AlignmentFromBits(a2l.packet_alignment_bits);
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

}  // namespace calmcar::xcp
