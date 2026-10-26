/**
 * @file protocol_version.hpp
 * @brief XCP 协议版本发现模型（批次 B1）
 *
 * @details 本文件定义协议层（Protocol Layer）与传输层（Transport Layer）的
 *          **两个独立版本快照**及其来源、置信度与能力策略。它是批次 B1
 *          「PL/TL 双版本状态机」的类型基础。
 *
 * @par 为什么必须分开两个快照
 *      原始 PDF 第 114 页 Table 45 的 GET_VERSION 响应同时携带协议层与传输层
 *      版本（`[0][Reserved][Major PL][Minor PL][Major TL][Minor TL]`），二者是
 *      **两个独立维度**：协议层规定命令语义，传输层规定线路布局（Tail、
 *      Alignment、Packet 边界）。因此**禁止**用其中一方的版本推导另一方
 *      （方案 §4 第 3 条）。
 *
 * @par 版本号语义（原始 PDF 第 12 页 §3.1.1 逐字）
 * @verbatim
 *      The XCP Protocol Layer Version Number is a 16-bit value, where the
 *      high byte contains the major version (X) and low byte contains the
 *      minor version (Y) number.
 *      If ... a functional modification in the slave's driver software is
 *      needed, the higher byte ... will be incremented. ...
 *      ... no direct influence ... the lower byte ... will be incremented.
 *      The slave only returns the most significant byte of the XCP Protocol
 *      Layer Version Number in the response upon CONNECT.
 * @endverbatim
 *      最后一句是引入 GET_VERSION 的根本动机：**CONNECT 只给主版本**，
 *      故仅凭 CONNECT 无法区分 V1.3 / V1.4 / V1.5。
 *
 * @warning **来源限定（用户裁决 m01451）**：本文件的规范引用全部回溯到
 *          `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`。
 *          禁止引用 Markdown 转换版行号。
 */

#ifndef LIBXCP_PROTOCOL_VERSION_HPP
#define LIBXCP_PROTOCOL_VERSION_HPP

#include <cstdint>
#include <optional>
#include <string>

namespace calmcar::xcp {

/**
 * @brief 协议/传输层版本号（Major.Minor 二元组）
 *
 * @details 对应规范 §3.1.1 的 16 位版本号：高字节 Major、低字节 Minor。
 *          本结构**只表达版本数值本身，不携带来源与置信度**——来源信息由
 *          `VersionEvidence` 承载，避免「数值」与「证据强度」被混为一谈。
 */
struct XcpProtocolVersion {
    /// @brief 主版本（高字节）
    std::uint8_t major{0};

    /// @brief 次版本（低字节）
    std::uint8_t minor{0};

    /// @brief 相等比较（同时比较 Major 与 Minor）
    [[nodiscard]] friend constexpr bool operator==(
        const XcpProtocolVersion&, const XcpProtocolVersion&) noexcept = default;

    /// @brief 字典序比较，先比 Major 再比 Minor
    [[nodiscard]] friend constexpr bool operator<(
        const XcpProtocolVersion& lhs,
        const XcpProtocolVersion& rhs) noexcept {
        if (lhs.major != rhs.major) {
            return lhs.major < rhs.major;
        }
        return lhs.minor < rhs.minor;
    }

    /// @brief 版本数值是否有效（Major 非 0）
    /// @details 0.0 视为「未知」占位，不作为合法版本参与比较与能力裁决。
    [[nodiscard]] constexpr bool IsValid() const noexcept { return major != 0U; }

    /// @brief 渲染为 `"X.Y"` 形式，供日志与错误文本使用
    [[nodiscard]] std::string ToString() const;
};

/// @brief 协议层 V1.2（AsamV14 兼容 Profile 的对端基线）
///
/// @details `thirdparty/XCPliteV1.2` 明确配置 Protocol Layer `0x0102`
///          （即 V1.2）、Transport Layer `0x0104`（即 V1.4）。为与该参考实现
///          互操作，V1.2 必须成为**被支持的**版本而非被拒绝的版本。
///
/// @note 本常量的存在**不改变** `kProtocolVersion15` 作为本库能力上限的地位：
///       V1.2 是「需要兼容的旧对端」，不是「本库的目标版本」。
inline constexpr XcpProtocolVersion kProtocolVersion12{1, 2};

/// @brief 协议层 V1.3（保守能力集的基准版本）
inline constexpr XcpProtocolVersion kProtocolVersion13{1, 3};

/// @brief 支持的协议层版本上限：V1.5（方案 D1 冻结；**工程决策**，
///        规范中不存在兼容矩阵，见方案 §1.9，不得声称有外部依据）
inline constexpr XcpProtocolVersion kProtocolVersion15{1, 5};

/// @brief 支持的传输层版本上限：Ethernet TL V1.5（方案 D1 工程支持范围）
inline constexpr XcpProtocolVersion kTransportVersion15{1, 5};

/// @brief 传输层 V1.4（`AsamV14` Wire Profile 的对端基线）
///
/// @details `thirdparty/XCPliteV1.2/src/xcptl_cfg.h` 配置
///          `XCP_TRANSPORT_LAYER_VERSION 0x0104`，即 TL V1.4。这是
///          `TcpWireProfile::AsamV14` / `UdpTransportConfig` 互操作基线的
///          传输层版本，与 `kProtocolVersion12`（PL `0x0102`）配套出现。
///
/// @note 与 `kTransportVersion15` 是**并存的两个受支持版本**，不是上限与
///       下限的关系：TL V1.4 的线路布局（无 LEN 外 Tail）与 V1.5 在
///       `PACKET_ALIGNMENT_8` 下数值等价，但对齐非 8 时**不兼容**，因此
///       二者必须由 Wire Profile 显式区分，绝不能按版本号自动推断布局。
inline constexpr XcpProtocolVersion kTransportVersion14{1, 4};

/**
 * @brief 版本证据来源
 *
 * @details 来源决定该版本值可被信任的程度，也是诊断与冲突裁决的依据。
 *          **来源与数值必须一同保存**：只留数值会让「确认 V1.3」与
 *          「假定 V1.3」变得无法区分（方案 §4.2 的明确要求）。
 */
enum class VersionSource {
    /// @brief 尚未取得任何证据（初始状态，或断连后的清零状态）
    Unknown,

    /// @brief 来自 GET_VERSION 响应（`0xC0,0x00`）——运行时唯一权威来源
    GetVersion,

    /// @brief 来自 CONNECT 响应，**仅有主版本字节**（规范 §3.1.1）
    ConnectMajor,

    /// @brief 来自调用方通过 `VersionPolicy` 的显式配置
    Configured,

    /// @brief 来自可信 A2L 的 IF_DATA 声明（静态证据，弱于运行时）
    A2l,
};

/// @brief 版本证据置信度
///
/// @details 与 `VersionSource` 正交：来源说明「从哪来」，置信度说明「有多确定」。
///          二者不得互相替代。
enum class VersionConfidence {
    /// @brief 无证据，版本未知
    Unknown,

    /// @brief 仅知主版本，**Minor 未确认**
    /// @details 这是 CONNECT-only 场景的必然状态。此时**禁止**猜测 Minor
    ///          （方案 D1：不得仅凭 CONNECT Major 猜测 Minor），能力裁决须走
    ///          `1.x-conservative` 保守策略。
    MajorOnly,

    /// @brief 由调用方显式配置给定
    Configured,

    /// @brief 来自可信 A2L 静态声明
    StaticEvidence,

    /// @brief 由 GET_VERSION 在运行时确认
    RuntimeConfirmed,
};

/**
 * @brief 单个层（协议层或传输层）的版本证据快照
 *
 * @details 一个快照 = 数值 + 来源 + 置信度 + 是否被封顶。协议层与传输层各自
 *          持有**独立的一份**，不得合并。
 */
struct VersionEvidence {
    /// @brief 上报的版本号（来源可能是 GET_VERSION / CONNECT / 配置 / A2L）
    XcpProtocolVersion reported{};

    /// @brief 来源
    VersionSource source{VersionSource::Unknown};

    /// @brief 置信度
    VersionConfidence confidence{VersionConfidence::Unknown};

    /// @brief Minor 是否已确认
    /// @details 为 false 时 `reported.minor` **不具语义**，调用方不得据此做
    ///          精确版本判断，只能按 `1.x-conservative` 能力集运行。
    bool minor_confirmed{false};

    /// @brief 该层是否被施加了版本上限封顶
    /// @details `Capped` 是**额外状态标志而非伪造的来源**（方案 §4.2）：当对端
    ///          上报高于支持上限的版本时，`reported` 保留原值、`effective`
    ///          封顶，并置本标志以记录该事实。
    bool capped{false};

    /// @brief 版本值是否可用（至少有主版本证据）
    [[nodiscard]] bool IsAvailable() const noexcept {
        return source != VersionSource::Unknown && reported.IsValid();
    }

    /// @brief 渲染来源名称（日志与诊断用）
    [[nodiscard]] std::string SourceName() const;

    /// @brief 渲染置信度名称（日志与诊断用）
    [[nodiscard]] std::string ConfidenceName() const;
};

/**
 * @brief 协议层版本快照（只读查询结果）
 *
 * @details 由 `XcpMaster::GetProtocolVersionSnapshot()` 返回。包含上报值与
 *          生效值两个维度：`reported` 是「对端声称的版本」，`effective` 是
 *          「本库实际据此运行的能力上限」。
 */
struct ProtocolVersionSnapshot {
    /// @brief 完整证据（数值 + 来源 + 置信度）
    VersionEvidence evidence{};

    /// @brief 生效版本：能力裁决实际使用的版本（已施加支持上限封顶）
    /// @details Minor 未确认时为 `1.x-conservative` 策略；此时本字段的 Minor
    ///          **不表示对端真实 Minor**，只是保守能力集的标识。
    XcpProtocolVersion effective{};

    /// @brief 是否处于 `1.x-conservative` 保守能力状态
    /// @details 为 true 时表示 Minor 未确认，能力裁决按 V1.3 交集执行，
    ///          **不代表对端就是 V1.3**。
    bool conservative{false};

    /// @brief 与调用方显式配置是否冲突（仅记录，不自动覆盖）
    bool conflicts_with_configured{false};

    /// @brief 与 A2L 静态声明是否冲突（仅记录；影响布局时须阻断相应功能）
    bool conflicts_with_a2l{false};
};

/// @brief 传输层版本快照
///
/// @details 结构与协议层一致但**独立存在**。传输层版本决定线路布局
///          （Tail/Alignment/Packet 边界），故其冲突后果与协议层不同：
///          协议层冲突影响命令语义，传输层冲突影响**字节解析**。
struct TransportVersionSnapshot {
    /// @brief 完整证据（数值 + 来源 + 置信度）
    VersionEvidence evidence{};

    /// @brief 生效版本：布局裁决实际使用的版本（已施加支持上限封顶）
    XcpProtocolVersion effective{};

    /// @brief 是否处于保守状态（Minor 未确认）
    bool conservative{false};

    /// @brief 与调用方显式配置是否冲突
    bool conflicts_with_configured{false};

    /// @brief 与 A2L 声明是否冲突
    /// @details 若冲突影响线路布局（如 PACKET_ALIGNMENT），调用方须阻断
    ///          相应功能而非静默采用其一。
    bool conflicts_with_a2l{false};
};

/**
 * @brief 由协议层版本推导出的**能力集**
 *
 * @details **B-T1 §4.3 引入**。此前实现把「生效版本」直接当作能力判据散落在
 *          各处判断，导致同一个版本问题在多个模块各自解释。本结构把
 *          「某个协议层版本**确定支持什么**」集中成一个可测试的真值表。
 *
 * @par 为什么必须引入
 *   AsamV14 兼容 Profile（XCPlite PL 1.2）对端缺少 V1.3 起加入的部分命令与
 *   传输层 Subcommand。若仅在版本比较处零散判断，极易漏判某一项而生成对端
 *   无法理解的报文。集中为能力集后，**任何**能力判断都必须经由
 *   `CapabilitySetFor(version)`，不可再自行比较版本号。
 *
 * @note 各字段语义为「**确定可用**」：为 false 时表示不得使用该能力，而
 *       不表示对端一定不支持（保守拒绝优于静默发错报文）。
 */
struct CapabilitySet {
    /// @brief 是否支持 GET_VERSION（`0xC0,0x00`，V1.2 起可用）
    /// @details 该命令自 XCP V1.0 即存在，V1.2 对端实测可用（XCPlite 支持），
    ///          故 V1.2 亦为 true。
    bool get_version{false};

    /// @brief 是否支持 Level-1 双字节命令码族（`0xC0` 前缀）
    /// @details V1.2 起可用；本库除 GET_VERSION 外未实现其它 0xC0 子命令。
    bool level1_commands{false};

    /// @brief 是否支持 V1.3 引入的扩展命令族
    /// @details 例如 `GET_DAQ_LIST_INFO`/`GET_DAQ_EVENT_INFO` 等按 DAQ List
    ///          逐项查询的接口；V1.2 对端不应假定具备，需运行时探测。
    bool extended_daq_queries{false};

    /// @brief 是否允许传输层 Optional Subcommand
    /// @details 本库基础 Profile 一律不实现，故恒为 false。
    bool optional_tl_subcommands{false};

    /// @brief 是否支持 DAQ Packed 模式（V1.5 特性）
    /// @details 仅 V1.5 起可用。
    bool daq_packed_mode{false};
};

/**
 * @brief 依据协议层**生效版本**推导能力集
 * @param effective 生效协议层版本（`ProtocolVersionSnapshot::effective`）
 * @return 该版本确定支持的能力集
 *
 * @details 推导规则（自低向高累加，高阶版本包含低阶能力）：
 *   - `effective >= V1.2`：`get_version`、`level1_commands`；
 *   - `effective >= V1.3`：追加 `extended_daq_queries`；
 *   - `effective >= V1.5`：追加 `daq_packed_mode`；
 *   - `optional_tl_subcommands` 恒为 false（本库不实现）。
 *
 * @note 传入无效版本（Major 为 0）时返回**全 false** 的保守能力集：无版本
 *       证据时不得假定任何能力，这与 `VersionPolicy` 的「未配置」语义一致。
 * @note `1.x-conservative` 状态下调用方应传入保守标识版本
 *       （`kProtocolVersion13`），从而自动获得保守能力交集。
 */
[[nodiscard]] CapabilitySet CapabilitySetFor(
    const XcpProtocolVersion& effective) noexcept;

/**
 * @brief 版本策略配置（`XcpMasterOptions` 成员）
 *
 * @details 由调用方显式提供版本先验。**默认全部为「未配置」**——库不得自动
 *          猜测版本（方案 §4.2：禁止自动猜测，不得伪造精确版本）。
 */
struct VersionPolicy {
    /// @brief 调用方显式指定的协议层版本（未设置表示不提供先验）
    /// @details 来源标记为 `Configured`。与 GET_VERSION 冲突时**运行时优先**，
    ///          但冲突须被记录下来（方案 §4.2）。
    std::optional<XcpProtocolVersion> protocol_version;

    /// @brief 调用方显式指定的传输层版本（未设置表示不提供先验）
    std::optional<XcpProtocolVersion> transport_version;

    /**
     * @brief A2L 静态声明的协议层版本（可选先验）
     * @details 来源标记为 `A2l`，置信度 `StaticEvidence`：优先级**低于**调用方
     *          配置、**高于** CONNECT 主版本。
     *
     *          存在的意义是对端不实现 GET_VERSION 时的精度补位：此时只能拿到
     *          CONNECT 主版本，裁决会进入 `1.x-conservative`（Minor 未确认，
     *          能力按交集收缩）。若 A2L 的 `PROTOCOL_LAYER` 已声明 `0x0102`，
     *          就能把能力裁决抬到真实的 V1.2，而不是假装成 `1.0`。
     */
    std::optional<XcpProtocolVersion> a2l_protocol_version;

    /// @brief A2L 静态声明的传输层版本（可选先验，语义同上）
    /// @details 取自 A2L 端点声明的 XCP 传输层版本（如 `XCP_ON_TCP_IP 0x104`
    ///          中的 `0x104`）。
    std::optional<XcpProtocolVersion> a2l_transport_version;

    /// @brief 是否在 CONNECT 后自动探测 GET_VERSION
    /// @details 默认 true。命令为 optional，探测失败不构成连接失败。
    bool probe_get_version{true};

    /// @brief 是否允许 GET_VERSION 探测占用额外往返
    /// @details 供对时延敏感的场景关闭；关闭后版本进入 `ConnectMajor` +
    ///          `MajorOnly` 状态。
    bool allow_version_probe_roundtrip{true};

    /// @brief 自检配置是否自相矛盾
    /// @return 空字符串表示配置合法；否则返回中文错误描述
    /// @details 在 `XcpMaster` 构造期调用，让配置错误尽早暴露。当前校验：
    ///          主版本是否为 0（0.x 视为非法——规范中 XCP 从 1.0 起）、
    ///          显式配置的版本是否低于支持下限、是否同时要求探测又禁用了
    ///          探测往返（自相矛盾）。
    [[nodiscard]] std::string Validate() const;
};

}  // namespace calmcar::xcp

#endif  // LIBXCP_PROTOCOL_VERSION_HPP