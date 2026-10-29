#pragma once

/**
 * @file transport_selection.hpp
 * @brief CLI 传输层选择解析：把「用户选项 + A2L 事实」解析为可审计的连接参数。
 *
 * 依据 `code-plan/TCP传输_实施计划_B-T1至B-T5.md` §8.1 实现（批次 B-T5）。
 * 第二阶段（`code-plan/TCP_A2L与线路证据联合决策_两阶段计划.md`）把本组件从
 * `tools/xcp_master/transport_selection.hpp` 提取为库组件：决策逻辑不再依赖
 * `tools/` 的包含路径，CLI 文本解析与摘要渲染仍留在工具侧。
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

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libxcp/protocol_types.hpp"
#include "libxcp/protocol_version.hpp"
#include "libxcp/tcp_wire_profile.hpp"

namespace calmcar::xcp {

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
    Auto,               ///< 来自严格的 `--tcp-profile auto`
    AutoLegacy,         ///< 来自有歧义风险的首成功试连
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

    /// @brief `--tcp-profile auto`：严格自动选择，已知歧义时拒绝建连
    /// @details 与具体 Profile、旧模式、对齐选项及 preset 互斥。
    bool tcp_profile_auto = false;

    /// @brief `--tcp-profile auto-legacy`：显式选择有风险的旧首成功行为
    bool tcp_profile_auto_legacy = false;

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

    /// @brief 实例的 alignment 信息是否因未核验而无法用于等价判断。
    bool instance_alignment_unverified = false;

    /// @brief A2L 静态声明的 Protocol Layer 版本（弱证据，仅供诊断）
    std::optional<XcpProtocolVersion> protocol_version;

    /// @brief A2L 静态声明的 Transport Layer 版本（弱证据，仅供诊断）
    std::optional<XcpProtocolVersion> transport_version;

    /// @brief A2L profile 诊断候选（**禁止**参与任何自动选择，§1.2）
    std::string profile_hint;

    /// @brief A2L 是否声明了**另一类**传输层（用于给出更准确的错误文本）
    bool other_kind_declared = false;
};

/**
 * @brief 显式 TCP 端点无法与唯一 A2L 端点对应时，移除端点专属证据。
 * @param request 用户提供的传输及端点配置。
 * @param facts 按传输类型过滤后的 A2L 事实，会原地移除不可归属的事实。
 * @return 移除证据时返回 true。
 * @details 不猜测 DNS 别名或多个同地址实例的等价性；UDP 行为不变。
 */
inline bool DiscardUnmatchedTcpEndpointEvidence(const TransportSelectionRequest& request,
                                                 A2lTransportFacts& facts) {
    if (request.transport != TransportKind::Tcp || !request.host.has_value() ||
        request.host->empty() || !request.port.has_value() || *request.port == 0 ||
        !facts.declared) {
        return false;
    }
    if (facts.instances.size() == 1U && facts.instance_count == 1U &&
        facts.host == *request.host && facts.port == *request.port) {
        return false;
    }
    facts.alignment_declared = false;
    facts.packet_alignment_bits = 0;
    facts.transport_version.reset();
    facts.profile_hint.clear();
    return true;
}

/// @brief 解析结果：直接可用的连接参数 + 完整来源信息
struct ResolvedTransportSelection {
    TransportKind transport = TransportKind::Udp;        ///< 传输层类别
    TcpWireProfile wire_profile = TcpWireProfile::Unknown;  ///< Wire Profile（UDP 时无意义）
    WireProfileSource wire_profile_source = WireProfileSource::NotApplicable;

    PacketAlignment alignment = PacketAlignment::Unknown;  ///< 生效的 PACKET_ALIGNMENT
    AlignmentSource alignment_source = AlignmentSource::ProfileDefault;

    /// @brief 是否需要上层执行有风险的旧模式试连（仅 `auto-legacy`）
    /// @details 为 true 时 `wire_profile` / `alignment` 保持 `Unknown`：
    ///          它们要等试连结束后才能确定，调用方必须用
    ///          `DefaultFramingCandidates()` 逐个尝试并回填选中值。
    bool wire_profile_auto = false;

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
[[nodiscard]] std::string_view TransportKindName(TransportKind kind) noexcept;

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
                          std::string* out_error);

}  // namespace calmcar::xcp
