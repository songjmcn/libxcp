/**
 * @file xcp_master.cpp
 * @brief XcpMaster 实现：连接编排、失败清理与内存读取转发。
 */

#include "libxcp/xcp_master.hpp"

#include "libxcp/version_resolver.hpp"
#include "libxcp/log.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace calmcar::xcp {

namespace {

/// @brief Seed/Key 分段的固定头部字节数：PID(1) + Length(1)
constexpr std::size_t kSeedKeyHeaderBytes = 2;
/// @brief Key 的 Length 字段为单字节，最大 255 字节（规范 §9.2 建议上限）
constexpr std::size_t kMaxKeyLength = 255;
/// @brief 可参与 Seed&Key 解锁的合法资源位集合
constexpr ResourceMask kValidUnlockResources =
    static_cast<ResourceMask>(Resource::CalPag) |
    static_cast<ResourceMask>(Resource::Daq) |
    static_cast<ResourceMask>(Resource::Stim) |
    static_cast<ResourceMask>(Resource::Pgm);

/// @brief TIMESTAMP_MODE 低 3 位位宽码 → 字节数（批次20，G3）
/// @details 原始 PDF 第 185 页 Table 147：Timestamp Size ∈ {0,1,2,4} 字节；
///          `011` 被规范明文标为 **Not allowed**，其余码值为未定义组合——
///          一律返回 0（不计入），**不猜**（B-3）。
///          XCPlite TIMESTAMP_MODE=0x0C → size_code=4、fixed=bit3 ✓。
constexpr std::size_t TimestampModeSizeBytes(std::uint8_t size_code) noexcept {
    switch (size_code) {
        case 1U:
            return 1U;
        case 2U:
            return 2U;
        case 4U:
            return 4U;
        default:
            return 0U;
    }
}

/// @brief TIMESTAMP_MODE 的低 3 位是否为**规范允许**的位宽码（E2/F-12）
/// @details 原始 PDF 第 185 页 Table 147：合法码为 `000`(无时间戳)、
///          `001`(1B)、`010`(2B)、`100`(4B)；**`011` 被明文标为
///          "Not allowed"**，`101`/`110`/`111` 未列出（保留）。
///          `000` 合法但表示"无时间戳"，不是时间戳宽度。
/// @param size_code TIMESTAMP_MODE 的 bit2-0
/// @return true = 该码在规范码表中
[[nodiscard]] constexpr bool IsAllowedTimestampSizeCode(
    std::uint8_t size_code) noexcept {
    return size_code == 0U || size_code == 1U || size_code == 2U ||
           size_code == 4U;
}

/// @brief TIMESTAMP_MODE 的高 4 位是否为**规范允许**的时间单位码（E2/F-12）
/// @details 原始 PDF 第 186 页 Table 148 列出 `0000`～`1100` 共 13 个单位；
///          **`1101`～`1111` 未列出，属保留值**，须按非法处理。
/// @param unit_code TIMESTAMP_MODE 的 bit7-4
/// @return true = 该码在规范码表中
[[nodiscard]] constexpr bool IsAllowedTimestampUnitCode(
    std::uint8_t unit_code) noexcept {
    return unit_code <= 0x0CU;
}

/// @brief GRANULARITY_ODT_ENTRY_SIZE_x 的规范允许取值集合（E2/F-12）
/// @details 原始 PDF 第 15 页 §4.1.2 原文："The possible values for
///          GRANULARITY_ODT_ENTRY_SIZE_x are **{1,2,4,8}**."
/// @param granularity_bytes 粒度字节数（响应原值）
/// @return true = 取值合法
[[nodiscard]] constexpr bool IsAllowedGranularity(
    std::uint8_t granularity_bytes) noexcept {
    return granularity_bytes == 1U || granularity_bytes == 2U ||
           granularity_bytes == 4U || granularity_bytes == 8U;
}

/// @brief DaqListSpec 入参预检（批次20 从静态通路抽出，STATIC/DYNAMIC 共用；
///        报错文案与批次14/15 逐字一致）
void PreflightDaqListSpec(const DaqListSpec& spec) {
    if (spec.odts.empty()) {
        throw detail::MakeInvalidArgument("DAQ List 至少要有 1 个 ODT");
    }
    if (spec.odts.size() > 0xFFU) {
        throw detail::MakeInvalidArgument(
            "ODT 数超过 SET_DAQ_PTR 单字节字段上限 255: " +
            std::to_string(spec.odts.size()));
    }
    for (const auto& odt : spec.odts) {
        if (odt.entries.empty()) {
            throw detail::MakeInvalidArgument("ODT 至少要有 1 个 Entry");
        }
        if (odt.entries.size() > 0xFFU) {
            throw detail::MakeInvalidArgument(
                "ODT Entry 数超过 255: " + std::to_string(odt.entries.size()));
        }
        for (const auto& e : odt.entries) {
            if (e.size == 0U) {
                throw detail::MakeInvalidArgument(
                    "WRITE_DAQ 的 Size 不得为 0（以 AG 为单位的元素数）");
            }
        }
    }
    if (spec.pid_off) {
        throw detail::MakeInvalidArgument(
            "PID_OFF 配置被拒绝：当前 DTO 解码器没有 Transport 层列表关联能力");
    }
}

/// @brief spec 布尔标志 → SET_DAQ_LIST_MODE 位（docs L2190-2196；两路共用）
DaqListModeBit DaqModeBitsFromSpec(const DaqListSpec& spec) {
    DaqListModeBit mode = DaqListModeBit::kNone;
    if (spec.stim_direction) {
        mode = mode | DaqListModeBit::kStim;
    }
    if (spec.dto_counter) {
        mode = mode | DaqListModeBit::kDtoCounter;
    }
    if (spec.timestamp) {
        mode = mode | DaqListModeBit::kTimestamp;
    }
    if (spec.pid_off) {
        mode = mode | DaqListModeBit::kPidOff;
    }
    return mode;
}

/// @brief 逐 ODT 的 MAX_DTO 信封预检（两路共用；文案与批次14 逐字一致）
void ValidateDaqListAgainstMaxDto(const DaqListSpec& spec,
                                  std::size_t header_bytes,
                                  std::size_t ag_bytes, std::size_t max_dto) {
    for (std::size_t odt_index = 0; odt_index < spec.odts.size(); ++odt_index) {
        std::size_t payload_bytes = 0U;
        for (const auto& e : spec.odts[odt_index].entries) {
            payload_bytes += static_cast<std::size_t>(e.size) * ag_bytes;
        }
        const std::size_t total_bytes = header_bytes + payload_bytes;
        if (total_bytes > max_dto) {
            throw detail::MakeInvalidArgument(
                "DAQ List " + std::to_string(spec.daq_list) + " 的 ODT " +
                std::to_string(odt_index) + " 超过 MAX_DTO：header(" +
                std::to_string(header_bytes) + ") + payload(" +
                std::to_string(payload_bytes) + ") = " +
                std::to_string(total_bytes) + " > " + std::to_string(max_dto));
        }
    }
}

/// @brief ODT Entry 的粒度与对齐前置校验（E2/F-12 C1～C5）
/// @details 规范依据为**原始 PDF 第 14-15 页 §4.1.2**，原文逐字：
///          - "GRANULARITY_ODT_ENTRY_SIZE_x must not be smaller than AG."
///            `GRANULARITY_ODT_ENTRY_SIZE_x[BYTE] >= AG[BYTE]`
///          - "The possible values for GRANULARITY_ODT_ENTRY_SIZE_x are
///            **{1,2,4,8}**."；"The possible values for ADDRESS_GRANULARITY
///            are **{1,2,4}**."
///          - `GRANULARITY_ODT_ENTRY_SIZE_x[BYTE] mod AG[BYTE] = 0`
///          - 地址：`Address[AG] mod (GRANULARITY_x[BYTE] / AG[BYTE]) = 0`
///          - 长度：`SizeOf(...)[AG] mod (GRANULARITY_x[BYTE] / AG[BYTE]) = 0`
///          - 上限：`SizeOf(...)[AG] <= MAX_ODT_ENTRY_SIZE_x[AG]`
///
///          **单位纪律（与 F-01 同族）**：`Address`、`Size`、
///          `MAX_ODT_ENTRY_SIZE_x` 三者在规范中**均以 AG 为单位**，而
///          `GRANULARITY_x` 是**字节数**。因此必须先验证
///          `GRANULARITY >= AG && GRANULARITY mod AG == 0`，再计算
///          `ratio = GRANULARITY / AG`；**禁止先做整数除法再取模**，
///          否则 `GRANULARITY < AG` 时会出现除零/模零。
///
/// @param spec 待校验的 List 配置（Address/Size 均为 AG 单位）
/// @param granularity_bytes Slave 声明的 GRANULARITY_ODT_ENTRY_SIZE_x（字节）
/// @param max_odt_entry_size Slave 声明的 MAX_ODT_ENTRY_SIZE_x（**AG 单位**）
/// @param ag_bytes 当前会话的地址粒度（字节）
/// @throws XcpException InvalidArgument —— 任一判据不满足
void ValidateOdtEntryGranularity(const DaqListSpec& spec,
                                 std::uint8_t granularity_bytes,
                                 std::uint8_t max_odt_entry_size,
                                 std::size_t ag_bytes) {
    // C1：粒度取值必须落在规范集合 {1,2,4,8}
    if (!IsAllowedGranularity(granularity_bytes)) {
        throw detail::MakeInvalidArgument(
            "Slave 声明的 GRANULARITY_ODT_ENTRY_SIZE 非法：" +
            std::to_string(granularity_bytes) +
            " 字节；规范（PDF 第 15 页 §4.1.2）只允许 {1,2,4,8}");
    }
    // C2：粒度必须能被当前 AG 表示（GRANULARITY >= AG 且整除）
    if (granularity_bytes < ag_bytes || granularity_bytes % ag_bytes != 0U) {
        throw detail::MakeInvalidArgument(
            "GRANULARITY_ODT_ENTRY_SIZE(" +
            std::to_string(granularity_bytes) +
            " 字节) 无法由当前 AG(" + std::to_string(ag_bytes) +
            " 字节) 表示：规范要求 GRANULARITY >= AG 且 GRANULARITY mod AG "
            "== 0（PDF 第 15 页 §4.1.2）");
    }
    // C2 成立后 ratio 必为正，后续取模安全
    const std::size_t ratio =
        static_cast<std::size_t>(granularity_bytes) / ag_bytes;
    for (std::size_t odt_index = 0; odt_index < spec.odts.size();
         ++odt_index) {
        for (std::size_t entry_index = 0;
             entry_index < spec.odts[odt_index].entries.size();
             ++entry_index) {
            const auto& e = spec.odts[odt_index].entries[entry_index];
            // C3：Entry 地址按 AG 换算后须对齐粒度
            if (static_cast<std::size_t>(e.address) % ratio != 0U) {
                throw detail::MakeInvalidArgument(
                    "DAQ List " + std::to_string(spec.daq_list) + " ODT " +
                    std::to_string(odt_index) + " Entry " +
                    std::to_string(entry_index) + " 地址未对齐：地址(AG 单位)=" +
                    std::to_string(e.address) + "，粒度比=" +
                    std::to_string(ratio) +
                    "；规范要求 Address[AG] mod (GRANULARITY / AG) == 0"
                    "（PDF 第 15 页 §4.1.2）");
            }
            // C4：Entry 长度（AG 单位）须对齐粒度
            if (static_cast<std::size_t>(e.size) % ratio != 0U) {
                throw detail::MakeInvalidArgument(
                    "DAQ List " + std::to_string(spec.daq_list) + " ODT " +
                    std::to_string(odt_index) + " Entry " +
                    std::to_string(entry_index) + " 长度未对齐：Size(AG 单位)=" +
                    std::to_string(e.size) + "，粒度比=" +
                    std::to_string(ratio) +
                    "；规范要求 Size[AG] mod (GRANULARITY / AG) == 0"
                    "（PDF 第 15 页 §4.1.2）");
            }
            // C5：Entry 长度不得超 MAX_ODT_ENTRY_SIZE_x（同样以 AG 为单位）
            if (e.size > max_odt_entry_size) {
                throw detail::MakeInvalidArgument(
                    "DAQ List " + std::to_string(spec.daq_list) + " ODT " +
                    std::to_string(odt_index) + " Entry " +
                    std::to_string(entry_index) + " 超出 MAX_ODT_ENTRY_SIZE：" +
                    "Size(AG 单位)=" + std::to_string(e.size) + " > 上限=" +
                    std::to_string(max_odt_entry_size) +
                    "（PDF 第 15 页 §4.1.2）");
            }
        }
    }
}

/// @brief DAQ_KEY_BYTE 的静态合法性校验（E2/O-2，配置前本地拒绝）
/// @details 两条规范依据（**原始 PDF 第 182 页**）：
///          - **Table 142 Address extension bit mask coding**：bit4-5 组合
///            `10` 被明文标为 **"Not allowed"**（合法值仅 `00`/`01`/`11`）；
///          - **Table 143 Identification field type**：bit6-7 用满 `00`～`11`
///            四值，本身无非法组合。
///          因此本函数只需拦下 bit4-5 == `10`。
/// @param key_byte GET_DAQ_PROCESSOR_INFO 的 DAQ_KEY_BYTE 拆解结果
/// @throws XcpException InvalidArgument —— 地址扩展位为规范禁止的组合
void ValidateDaqKeyByte(const DaqKeyByte& key_byte) {
    // RawDaqKeyByteAddressExtensionMode 的原始位值：bit5-4 == 2 即 `10`
    if (key_byte.address_extension_mode == 2U) {
        throw detail::MakeInvalidArgument(
            "Slave 声明的 DAQ_KEY_BYTE 地址扩展位为 2(bit5-4 = 10)，"
            "该组合被规范明文禁止（原始 PDF 第 182 页 Table 142 "
            "\"Not allowed\"）；合法值为 0/1/3");
    }
}

/// @brief OVERLOAD_MSB 启用时的 ODT 编号上限校验（E2/O-2）
/// @details 规范依据（**原始 PDF 第 180 页**原文逐字）：
///          "When the MSB of the PID is used, the maximum number of
///           (absolute or relative) ODT numbers is limited and has to be in
///           the range `0x00 <= ODT_NUMBER(DAQ with overrun_msb) < 0x7C`"
///          触发条件为 DAQ_PROPERTIES 的 bit7-6 == `01`
///          （Table 139：`01` = overload indication in MSB of PID）。
/// @param spec 待校验的 List 配置（ODT 号即其在 odts 中的 0 基下标）
/// @param overload_msb true = Slave 使用 PID 的 MSB 指示过载
/// @throws XcpException InvalidArgument —— ODT 编号达到或超过 0x7C
void ValidateOdtNumberAgainstOverloadMsb(const DaqListSpec& spec,
                                         bool overload_msb) {
    if (!overload_msb) {
        return;
    }
    // ODT 编号为 0 基下标；上限为**严格小于** 0x7C
    constexpr std::size_t kOverloadMsbOdtLimit = 0x7CU;
    if (spec.odts.size() > kOverloadMsbOdtLimit) {
        throw detail::MakeInvalidArgument(
            "DAQ List " + std::to_string(spec.daq_list) + " 有 " +
            std::to_string(spec.odts.size()) +
            " 个 ODT，超过 OVERLOAD_MSB 模式的上限：规范要求 ODT_NUMBER "
            "严格小于 0x7C(124)（原始 PDF 第 180 页）");
    }
    // 显式逐条核对（下标即 ODT 号），避免只靠总数判断
    for (std::size_t i = 0; i < spec.odts.size(); ++i) {
        if (i >= kOverloadMsbOdtLimit) {
            throw detail::MakeInvalidArgument(
                "DAQ List " + std::to_string(spec.daq_list) + " 的 ODT 号 " +
                std::to_string(i) +
                " 超过 OVERLOAD_MSB 模式上限（须严格小于 0x7C）；原始 PDF "
                "第 180 页");
        }
    }
}

}  // namespace

XcpMaster::XcpMaster(std::unique_ptr<IXcpTransport> transport,
                     XcpMasterOptions options)
    : m_transport_(std::move(transport)), m_options_(std::move(options)) {
    if (!m_transport_) {
        throw detail::MakeInvalidArgument(
            "XcpMaster 需要非空的 IXcpTransport 实例");
    }
    // 版本策略自检：把"配置本身自相矛盾"这类问题在构造期暴露，
    // 而不是等到 Connect 之后以模糊的探测失败呈现给调用方。
    const std::string policy_error = m_options_.version_policy.Validate();
    if (!policy_error.empty()) {
        throw detail::MakeInvalidArgument(policy_error);
    }
    m_executor_ = std::make_unique<CommandExecutor>(
        *m_transport_, m_session_, m_options_.timeouts,
        m_options_.event_listener);
    m_memory_access_ = std::make_unique<MemoryAccess>(*m_executor_, m_session_);
}

ProtocolVersionSnapshot XcpMaster::GetProtocolVersionSnapshot() const {
    return m_protocol_version_;
}

TransportVersionSnapshot XcpMaster::GetTransportVersionSnapshot() const {
    return m_transport_version_;
}

std::vector<std::string> XcpMaster::VersionConflicts() const {
    return m_version_conflicts_;
}

bool XcpMaster::IsCapabilityUnsupported(
    UnsupportedCapability capability) const {
    // 批次 G：本版本只承诺 IPv4 UDP 单播 Profile，且 0xC0 族只实现了
    // GET_VERSION。以下四项是**确定不提供**的能力，不随会话状态变化——
    // 它们是本库的静态能力边界，而非"依赖对端查询结果"的动态能力。
    //
    // 特别说明 DaqPackedMode：原始 PDF 确认 SET/GET_DAQ_PACKED_MODE
    // （0xC0,0x01 / 0xC0,0x02）是 V1.5 新特性，且本实现连命令码都未登记
    // （见 include/libxcp/protocol_types.hpp 的 CommandCode 注释：按
    // "只登记已实现的码"纪律）。因此这里判 Block 是**如实反映现状**，
    // 而不是新增限制。
    switch (capability) {
        case UnsupportedCapability::DaqPackedMode:
        case UnsupportedCapability::Level1OtherSubcommand:
        case UnsupportedCapability::Ipv6Transport:
        case UnsupportedCapability::OptionalTlSubcommand:
            return true;
    }
    // 枚举全覆盖，理论上不可达；保留以便新增能力项时编译器能提示
    return true;
}

void XcpMaster::ThrowIfUnsupported(UnsupportedCapability capability) const {
    if (!IsCapabilityUnsupported(capability)) {
        return;
    }
    // 异常消息给出能力名 + 替代路径，避免调用方只看到"不支持"而无从下手
    std::string name;
    std::string hint;
    switch (capability) {
        case UnsupportedCapability::DaqPackedMode:
            name = "DAQ Packed Mode（SET/GET_DAQ_PACKED_MODE）";
            hint = "该能力为 V1.5 新特性且本实现未提供；请使用既有"
                   " SET_DAQ_LIST_MODE + WRITE_DAQ 的非 Packed 路径";
            break;
        case UnsupportedCapability::Level1OtherSubcommand:
            name = "Level-1 命令族子命令";
            hint = "本实现只提供 GET_VERSION（0xC0,0x00）；其余 0xC0 族"
                   "子命令未实现，禁止内容探测式尝试";
            break;
        case UnsupportedCapability::Ipv6Transport:
            name = "IPv6 传输";
            hint = "本版本基础 Profile 只承诺 IPv4 UDP 单播；"
                   "请使用 IPv4 地址";
            break;
        case UnsupportedCapability::OptionalTlSubcommand:
            name = "Optional TL Subcommand";
            hint = "本版本基础 Profile 不实现这四个传输层子命令";
            break;
    }
    LIBXCP_LOG_ERROR("capability unsupported: {}", name);
    throw detail::MakeUnsupportedFeature(
        "本库明确不支持该能力：" + name + "。" + hint);
}

XcpMaster::~XcpMaster() {
    // 析构路径不抛异常：尽力而为地关闭通道，本地状态随之释放
    try {
        if (m_session_.IsConnected()) {
            try {
                m_executor_->ExecuteDisconnect();
            } catch (...) {
                LIBXCP_LOG_WARNING(
                    "XCP disconnect failed during master destruction; "
                    "resetting local session");
                m_session_.Reset();
            }
        }
        m_transport_->Close();
    } catch (...) {
        LIBXCP_LOG_WARNING(
            "XCP transport cleanup failed during master destruction");
    }
    // Executor 持有 Transport 引用，必须先于 Transport 销毁
    m_memory_access_.reset();
    m_executor_.reset();
}

void XcpMaster::Connect() {
    // 已连接时拒绝重复连接（避免在活跃会话上重放 CONNECT）
    if (m_session_.IsConnected()) {
        LIBXCP_LOG_ERROR("XCP Connect rejected: Session is already connected");
        throw detail::MakeInvalidState("Session 已处于 Connected 状态");
    }
    LIBXCP_LOG_INFO(
        "XCP Connect started: resetting local Session and DAQ state");
    m_session_.Reset();  // 清理 Failed/残留状态
    // 新会话：本端 WRITE_DAQ 账本随之作废（Session 的 DAQ 运行态在
    // EstablishConnection 里一并归零）
    m_daq_ledger_.clear();
    m_daq_processor_info_queried_ = false;
    m_daq_processor_properties_.reset();
    m_daq_resolution_info_queried_ = false;
    m_daq_timestamp_bytes_.reset();
    m_pag_processor_info_queried_ = false;
    m_pag_processor_info_.reset();
    // 批次 B1：版本证据是**会话态**，新会话必须清零。否则从 V1.5 对端切到
    // V1.3 对端时会沿用旧裁决，造成"能力超出对端实际支持"的静默越界。
    m_protocol_version_ = ProtocolVersionSnapshot{};
    m_transport_version_ = TransportVersionSnapshot{};
    m_version_conflicts_.clear();

    // Transport.Open 的监听器即 CommandExecutor；Open 之后接收线程立即开始回调
    m_transport_->Open(m_executor_->AsListener());
    // 批次 B1：复位执行器的**通道级**关闭状态。Disconnect 会 Close Transport，
    // 从而由 OnTransportClosed() 把 m_transport_closed_ 置位；若 Open 之后不
    // 清回，重连后第一条命令会在 WaitForResponse 里立刻看到"通道已关闭"而
    // 提前返回，CONNECT 超时、恢复又因会话处于 Connecting 而抛 InvalidState
    // ——即"一次 Disconnect 后再也无法 Connect"。该复位必须紧跟 Open，
    // 不能依赖调用方自觉。
    m_executor_->OnTransportReopened();

    auto cleanup_on_failure = [this] {
        // CONNECT、参数校验或 GET_STATUS 失败时清除部分状态并关闭通道（计划
        // §5.1）
        m_session_.Reset();
        try {
            m_transport_->Close();
        } catch (...) {
            LIBXCP_LOG_WARNING(
                "XCP connection failure cleanup could not close transport");
        }
    };

    try {
        const ConnectResponse connect = m_executor_->ExecuteConnect(0x00);

        // 仅在 CONNECT 表明 Optional 信息可用时才查询扩展通信模式；
        // Slave 返回 ERR_CMD_UNKNOWN 时 ExecuteGetCommModeInfo 内部降级为
        // nullopt。
        if (connect.optional_comm_mode_available) {
            (void)m_executor_->ExecuteGetCommModeInfo();
        }

        // GET_STATUS 为 Mandatory，失败即视为连接失败
        (void)m_executor_->ExecuteGetStatus();
        LIBXCP_LOG_INFO("XCP Connect: mandatory GET_STATUS confirmed");

        // 批次 B1：版本裁决。顺序上放在 GET_STATUS 之后、PAG 探测之前——
        // GET_STATUS 是 Mandatory 且不依赖版本，先完成它可以让"对端根本
        // 不可用"这类问题优先暴露，而不是被版本探测的错误掩盖。
        ResolveVersionAfterConnect(connect);

        // 变量标定批次：CONNECT 协商出 CAL/PAG 资源时探测 Paging 子系统
        // （GET_PAG_PROCESSOR_INFO，Optional）。Slave 不支持（nullopt）或
        // 查询超时等错误只降级缓存、不阻断连接——Page 命令本身仍是逐条
        // Optional，由调用方按需触发懒查（QueryPagProcessorInfo）。
        if (HasCalPagResource()) {
            m_pag_processor_info_queried_ = true;
            try {
                m_pag_processor_info_ =
                    m_executor_->ExecuteGetPagProcessorInfo();
                if (m_pag_processor_info_) {
                    m_session_.SetPagProcessorInfo(*m_pag_processor_info_);
                }
            } catch (const XcpException&) {
                LIBXCP_LOG_WARNING(
                    "Optional GET_PAG_PROCESSOR_INFO failed; continuing "
                    "connection");
                m_pag_processor_info_.reset();
            }
        }
        LIBXCP_LOG_INFO("XCP Connect completed; Session connected");
    } catch (const XcpException&) {
        LIBXCP_LOG_WARNING(
            "XCP Connect aborted: resetting Session and closing transport");
        cleanup_on_failure();
        throw;
    } catch (const std::exception& e) {
        cleanup_on_failure();
        LIBXCP_LOG_ERROR("XCP connection failed with non-protocol exception");
        throw detail::MakeTransportError("连接过程中发生非协议异常", e.what());
    }
}

CommandExecutor::GetVersionProbe XcpMaster::ProbeVersion() {
    // 探测不改变会话状态：即使失败也只是"版本未确认"，连接照旧可用。
    const CommandExecutor::GetVersionProbe probe =
        m_executor_->ExecuteGetVersion();
    // 探测成功必须真正**生效**：否则调用方拿到 Available 却在快照里
    // 仍看到"未确认"，接口语义自相矛盾（构造期关了自动探测的场景尤其
    // 依赖这条路径补测）。失败则保持原证据不变——不得降级为假定版本。
    if (probe.IsAvailable()) {
        ApplyVersionProbe(probe);
    }
    return probe;
}

void XcpMaster::ResolveVersionAfterConnect(const ConnectResponse& connect) {
    // 参数保留在签名里以自文档化"本裁决发生在 CONNECT 之后、且依赖其响应"，
    // 实际读取走 `Session::Parameters()`（会话已由 EstablishConnection 落盘，
    // 单一数据源可避免两处取值不一致）。显式标注避免 unused 警告。
    static_cast<void>(connect);

    // 探测条件：配置允许 + 配置的探测开关。注意 GET_VERSION 是 optional
    // 命令，对端不认识时必须能优雅降级。
    const bool should_probe = m_options_.autodetect_version &&
                              m_options_.version_policy.probe_get_version &&
                              m_options_.version_policy
                                  .allow_version_probe_roundtrip;

    CommandExecutor::GetVersionProbe probe;
    if (should_probe) {
        probe = m_executor_->ExecuteGetVersion();
        if (probe.IsAvailable()) {
            // 关日志时 LIBXCP_LOG_INFO 不求值参数，故变量随开关一同声明，
            // 避免 -Wunused-variable（仓库既有约定，见 session.cpp:166）。
#if LIBXCP_ENABLE_LOG
            const GetVersionResponse& vr = *probe.response;
            LIBXCP_LOG_INFO(
                "XCP GET_VERSION resolved: protocol={}.{} transport={}.{}",
                static_cast<unsigned>(vr.protocol_major),
                static_cast<unsigned>(vr.protocol_minor),
                static_cast<unsigned>(vr.transport_major),
                static_cast<unsigned>(vr.transport_minor));
#endif
        } else {
            // 关键日志：明确区分"未确认"与"就是老版本"，避免运维把超时
            // 误读成对端版本低（方案 B1 硬要求）。
            LIBXCP_LOG_INFO(
                "XCP GET_VERSION unavailable (outcome={}); version remains "
                "unconfirmed at connect-major level",
                static_cast<int>(probe.outcome));
        }
    } else {
        // 未探测：显式说明原因，便于诊断"为什么版本只有主版本"
        LIBXCP_LOG_INFO(
            "XCP GET_VERSION probe skipped by configuration; version stays "
            "at connect-major level");
    }

    ApplyVersionProbe(probe);
}

void XcpMaster::ApplyVersionProbe(
    const CommandExecutor::GetVersionProbe& probe) {
    VersionResolver::Inputs inputs;
    // CONNECT 只给出主版本字节（原始 PDF 第 12 页 §3.1.1），作为最低一档证据。
    // 注意：这里不能从快照反推，必须直接取会话参数——快照本身正是待裁决对象。
    // 未连接时 `Parameters().connect` 为默认值（版本字节 0），裁决器会把
    // 主版本 0 视为"无证据"而不是"版本 0.0"，因此不会伪造版本。
    const SessionParameters params = m_session_.Parameters();
    inputs.connect_protocol_major = params.connect.protocol_layer_version;
    inputs.connect_transport_major = params.connect.transport_layer_version;
    inputs.policy = m_options_.version_policy;
    inputs.probe = probe;

    const VersionResolver::Result result = VersionResolver::Resolve(inputs);

    if (m_options_.require_get_version && !result.get_version_available) {
        // 调用方显式要求精确版本却拿不到：这是配置与现实的冲突，必须失败。
        // 放在裁决之后抛错，以便日志先记录完整裁决上下文。
        // 用 InvalidState：这是"本地配置要求与对端现实不符"，不是收到坏包，
        // 因此不适合 MalformedPacket/ProtocolError（那两个语义是"对端违规"）。
        throw detail::MakeInvalidState(
            "require_get_version 为真但 GET_VERSION 不可用（对端可能未实现该 "
            "optional 命令或响应超时）；版本保持未确认");
    }

    m_protocol_version_ = result.protocol;
    m_transport_version_ = result.transport;
    m_version_conflicts_ = result.conflicts;

    // 冲突与封顶一律显式告警，不做静默处理（m03674 裁定）
#if LIBXCP_ENABLE_LOG
    for (const std::string& conflict : m_version_conflicts_) {
        LIBXCP_LOG_WARNING("XCP version conflict: {}", conflict);
    }
#endif
    if (result.protocol.conservative) {
        LIBXCP_LOG_INFO(
            "XCP protocol version running in 1.x-conservative mode: minor "
            "unconfirmed, capability decisions use V1.3 intersection");
    }
    if (result.transport.conservative) {
        LIBXCP_LOG_INFO(
            "XCP transport version running in conservative mode: minor "
            "unconfirmed, layout decisions use the lowest supported level");
    }
}

void XcpMaster::CancelPendingOperation() {
    // 直接转发给执行器：取消是执行器层的等待状态问题，XcpMaster 不持有
    // 额外状态。无挂起操作时为空操作（幂等）。
    if (m_executor_) {
        m_executor_->CancelPendingOperation();
    }
}

void XcpMaster::Disconnect() {
    // 未连接：本地幂等，仅确保通道关闭
    if (m_session_.State() == SessionState::Disconnected) {
        LIBXCP_LOG_INFO(
            "XCP Disconnect: already disconnected; closing transport");
        // 幂等路径同样作废版本证据：会话已不在，快照不得继续声称精确版本
        m_protocol_version_ = ProtocolVersionSnapshot{};
        m_transport_version_ = TransportVersionSnapshot{};
        m_version_conflicts_.clear();
        m_transport_->Close();
        return;
    }
    LIBXCP_LOG_INFO("XCP Disconnect started state={}",
                    SessionStateName(m_session_.State()));

    // 批次14（T14-07）：断连前尽力补发 START_STOP_SYNCH(stop all)。
    // 不停就断开会留下"逻辑上仍在跑表"的 Slave 状态；但 STOP 失败也不得
    // 阻断释放路径（会话即将作废，且 Transport.Close 本身就是终止手段），
    // 因此这里只吞掉 STOP 的异常，让 DISCONNECT 的错误继续如实上抛。
    if (m_session_.HasRunningDaqList()) {
        try {
            m_executor_->ExecuteStartStopSynch(DaqSynchAction::StopAll);
            m_session_.ClearStartedDaqLists();
        } catch (const XcpException&) {
            // 见上：断连清理路径不做二次抛错
            LIBXCP_LOG_WARNING(
                "XCP stop-all failed during disconnect; continuing cleanup");
        }
    }

    std::optional<XcpException> failure;
    try {
        m_executor_->ExecuteDisconnect();
    } catch (const XcpException& e) {
        // 即使 DISCONNECT 返回 ERR_CMD_BUSY 或 Transport
        // 错误，也释放本地资源（计划 §5.4）
        failure = e;
        m_session_.Reset();
    }

    try {
        m_transport_->Close();
    } catch (const XcpException& e) {
        if (!failure) {
            failure = e;
        }
    }

    // 批次 B1：版本证据是**会话态**，会话结束即作废。
    // 若不在此清零，`GetProtocolVersionSnapshot()` 在断连后会继续报告上一次
    // 会话的 `GetVersion` 精确版本——调用方会据此对一个"已无会话"的连接
    // 做出能力判断，属于静默越界。Connect() 起始处另有一次清零用于兜底
    // （覆盖"从未 Disconnect 过、直接重连"的路径），两处共同保证不残留。
    m_protocol_version_ = ProtocolVersionSnapshot{};
    m_transport_version_ = TransportVersionSnapshot{};
    m_version_conflicts_.clear();

    if (failure) {
        LIBXCP_LOG_ERROR(
            "XCP Disconnect completed cleanup with reported failure");
        throw *failure;
    }
    LIBXCP_LOG_INFO("XCP Disconnect completed");
}

bool XcpMaster::IsConnected() const { return m_session_.IsConnected(); }

Bytes XcpMaster::ReadMemoryBytes(Address address, AddressExtension extension,
                                 ByteCount byte_count) {
    return m_memory_access_->ReadBytes(address, extension, byte_count);
}

Bytes XcpMaster::ReadMemory(Address address, AddressExtension extension,
                            ElementCount element_count) {
    return m_memory_access_->ReadElements(address, extension, element_count);
}

void XcpMaster::WriteMemoryBytes(Address address, AddressExtension extension,
                                 BytesView data) {
    // 批次14（R9）：写回通路，与 ReadMemoryBytes 对称；失败一律上抛，
    // 绝不静默吞错或只写一半还报成功
    m_memory_access_->WriteBytes(address, extension, data);
}

// ---------------------------------------------------------------------------
// DAQ 编排与 WRITE_DAQ 账本（批次14，T14-08）
// ---------------------------------------------------------------------------

void XcpMaster::ConfigureDaqList(const DaqListSpec& spec) {
    // ---- 1) 入参预检 + 解码能力校验（批次20 抽出共享函数，文案不变）
    PreflightDaqListSpec(spec);
    if (!m_daq_processor_info_queried_) {
        m_daq_processor_info_queried_ = true;
        const auto processor = m_executor_->ExecuteGetDaqProcessorInfo();
        if (processor.has_value()) {
            // 批次 E2：保留完整快照（F-05 需 MIN_DAQ、O-2 需 KEY_BYTE）
            m_daq_processor_info_ = processor;
            m_daq_processor_properties_ = processor->properties;
        }
    }
    // O-2：地址扩展位为规范禁止组合（bit5-4 = 10）时立即拒绝。
    // 放在缓存块外，确保任何时候只要有取证快照都会被校验（不只首次查询）。
    if (m_daq_processor_info_.has_value()) {
        ValidateDaqKeyByte(m_daq_processor_info_->key_byte);
    }
    if (m_daq_processor_properties_.has_value() &&
        HasDaqProcessorProperty(
            static_cast<DaqProcessorPropertyBit>(*m_daq_processor_properties_),
            DaqProcessorPropertyBit::kConfigType)) {
        throw detail::MakeUnsupportedFeature(
            "Slave 声明 DAQ_PROPERTY_CONFIG_TYPE=DYNAMIC，静态通路不适用；"
            "请改用 ConfigureDaqListsDynamic 做动态整表编排");
    }

    const std::size_t ag_bytes = AgToBytes(m_session_.GetAddressGranularity());
    // G3 修正（批次20）：spec.timestamp 开启时把 Slave 经
    // GET_DAQ_RESOLUTION_INFO 声明的时间戳宽度计入信封预检；未声明按 0 计
    // ——不猜（B-3），对不声明时间戳的 Slave 行为与旧口径完全一致。
    std::size_t header_bytes = 1U + (spec.dto_counter ? 1U : 0U);
    // 批次 E2/F-12：无论是否启用时间戳都先完成一次 GET_DAQ_RESOLUTION_INFO
    // 取证——C1～C5 的粒度与上限判据依赖该响应，若只在 spec.timestamp 时取证
    // 则无时间戳配置会完全绕过 F-12 校验（本批次修复的正是这类漏检）。
    const std::size_t timestamp_bytes = DaqTimestampBytesCached();
    if (spec.timestamp) {
        header_bytes += timestamp_bytes;
    }
    ValidateDaqListAgainstMaxDto(spec, header_bytes, ag_bytes,
                                 m_session_.MaxDto());
    // 批次 E2/F-12：ODT Entry 粒度与对齐前置校验（C1～C5）。
    // 仅当 Slave 经 GET_DAQ_RESOLUTION_INFO 声明了粒度时才校验——不猜（B-3），
    // 对不声明该 Optional 命令的 Slave 行为与旧口径完全一致。
    if (m_daq_resolution_info_.has_value() &&
        m_daq_resolution_info_->granularity_daq != 0U) {
        ValidateOdtEntryGranularity(spec, m_daq_resolution_info_->granularity_daq,
                                    m_daq_resolution_info_->max_odt_entry_size_daq,
                                    ag_bytes);
    }
    // 批次 E2/O-2：OVERLOAD_MSB 启用时 ODT 编号须严格小于 0x7C
    ValidateOdtNumberAgainstOverloadMsb(spec, IsOverloadMsbEnabled());

    // 仅记录编排元信息，不输出 Entry 的地址或数据。
    LIBXCP_LOG_INFO("DAQ static configure started list={} odts={}",
                    spec.daq_list, spec.odts.size());
    // ---- 2) 下发；任何一步失败都回滚账本（B-6：解码只信完整账本） ----
    const std::size_t ledger_mark = m_daq_ledger_.size();
    auto rollback = [this, ledger_mark] {
        m_daq_ledger_.erase(
            m_daq_ledger_.begin() + static_cast<std::ptrdiff_t>(ledger_mark),
            m_daq_ledger_.end());
    };
    try {
        // 先清列表：PREDEFINED 列表由 Slave 拒写（ERR_WRITE_PROTECTED，
        // docs L2168），本方法不会把 PREDEFINED 当可配置列表来改
        m_executor_->ExecuteClearDaqList(spec.daq_list);
        m_session_.BumpDaqConfigGeneration();

        // 逐 Entry 显式 SET_DAQ_PTR + WRITE_DAQ：不依赖 Slave 的指针自增
        // （docs L2172 写过末位后指针未定义）
        std::uint8_t odt_no = 0U;
        for (const auto& odt : spec.odts) {
            std::uint8_t entry_no = 0U;
            for (const auto& e : odt.entries) {
                m_executor_->ExecuteSetDaqPtr(spec.daq_list, odt_no, entry_no);
                m_executor_->ExecuteWriteDaq(e.bit_offset, e.size, e.extension,
                                             e.address);
                DaqLedgerEntry rec;
                rec.daq_list = spec.daq_list;
                rec.odt_number = odt_no;
                rec.odt_entry = entry_no;
                rec.address = e.address;
                rec.extension = e.extension;
                rec.size = e.size;
                rec.bit_offset = e.bit_offset;
                m_daq_ledger_.push_back(rec);
                ++entry_no;
            }
            ++odt_no;
        }

        // 设置列表模式（方向 / DTO 计数器 / 时间戳 / PID_OFF / 事件通道）
        DaqListModeBit mode = DaqListModeBit::kNone;
        if (spec.stim_direction) {
            mode = mode | DaqListModeBit::kStim;
        }
        if (spec.dto_counter) {
            mode = mode | DaqListModeBit::kDtoCounter;
        }
        if (spec.timestamp) {
            mode = mode | DaqListModeBit::kTimestamp;
        }
        if (spec.pid_off) {
            mode = mode | DaqListModeBit::kPidOff;
        }
        m_executor_->ExecuteSetDaqListMode(mode, spec.daq_list,
                                           spec.event_channel, spec.prescaler,
                                           spec.priority);
    } catch (const XcpException&) {
        rollback();
        LIBXCP_LOG_WARNING(
            "DAQ static configure failed list={}; local ledger rolled back",
            spec.daq_list);
        throw;
    } catch (const std::exception& e) {
        rollback();
        LIBXCP_LOG_WARNING(
            "DAQ static configure failed list={}; local ledger rolled back",
            spec.daq_list);
        throw detail::MakeTransportError("DAQ 配置过程中发生非协议异常",
                                         e.what());
    }
    LIBXCP_LOG_INFO(
        "DAQ static configure completed list={} entries={} generation={}",
        spec.daq_list, m_daq_ledger_.size() - ledger_mark,
        m_session_.DaqConfigGeneration());
}

std::uint8_t XcpMaster::StartDaqList(std::uint16_t daq_list) {
    const bool configured = std::any_of(
        m_daq_ledger_.begin(), m_daq_ledger_.end(),
        [daq_list](const DaqLedgerEntry& e) { return e.daq_list == daq_list; });
    if (!configured) {
        LIBXCP_LOG_ERROR(
            "XCP StartDaqList rejected: DAQ list {} is not configured",
            daq_list);
        throw detail::MakeInvalidState(
            "DAQ List " + std::to_string(daq_list) +
            " 未在本端登记（ConfigureDaqList 未成功），拒绝启动");
    }
    const StartStopDaqListResponse resp =
        m_executor_->ExecuteStartStopDaqList(DaqListAction::Start, daq_list);
    // FIRST_PID → PID 回填（Absolute ODT Number：绝对 ODT 号 = FIRST_PID +
    // 相对 ODT 号，docs L2225）。识别字段类型不是 Absolute 时该推导不成立，
    // 由 B-16 的 identification_field_type Error 比对拦下（docs L2228），
    // 本层不做"另一种编码下也当作 PID"的猜测。
    for (auto& e : m_daq_ledger_) {
        if (e.daq_list == daq_list) {
            e.pid = static_cast<std::uint8_t>(resp.first_pid + e.odt_number);
        }
    }
    m_session_.MarkDaqListStarted(daq_list);
    LIBXCP_LOG_INFO("DAQ list started list={} first_pid={} running_lists={}",
                    daq_list, resp.first_pid,
                    m_session_.StartedDaqLists().size());
    return resp.first_pid;
}

void XcpMaster::StopDaqList(std::uint16_t daq_list) {
    (void)m_executor_->ExecuteStartStopDaqList(DaqListAction::Stop, daq_list);
    m_session_.MarkDaqListStopped(daq_list);
    LIBXCP_LOG_INFO("DAQ list stopped list={} running_lists={}", daq_list,
                    m_session_.StartedDaqLists().size());
}

void XcpMaster::StopDaq() {
    m_executor_->ExecuteStartStopSynch(DaqSynchAction::StopAll);
    // 配置仍在 Slave 里，账本保留；只清运行态（重新 Start 不需要重写）
    m_session_.ClearStartedDaqLists();
    LIBXCP_LOG_INFO("DAQ stop-all confirmed; running_lists=0");
}

void XcpMaster::ClearDaqList(std::uint16_t daq_list) {
    m_executor_->ExecuteClearDaqList(daq_list);
    m_session_.BumpDaqConfigGeneration();
    m_session_.MarkDaqListStopped(daq_list);
    m_daq_ledger_.erase(
        std::remove_if(m_daq_ledger_.begin(), m_daq_ledger_.end(),
                       [daq_list](const DaqLedgerEntry& e) {
                           return e.daq_list == daq_list;
                       }),
        m_daq_ledger_.end());
    LIBXCP_LOG_INFO("DAQ list cleared list={} generation={} ledger_entries={}",
                    daq_list, m_session_.DaqConfigGeneration(),
                    m_daq_ledger_.size());
}

std::size_t XcpMaster::DaqTimestampBytesCached() {
    // 一次性取证（G3）：GET_DAQ_RESOLUTION_INFO 属 Optional 面——Slave 不支持
    // （CMD_UNKNOWN→nullopt）时按 0 字节计，不猜宽度（B-3）；结果缓存，
    // Connect() 重置（D16）。
    if (!m_daq_resolution_info_queried_) {
        m_daq_resolution_info_queried_ = true;
        const auto resolution = m_executor_->ExecuteGetDaqResolutionInfo();
        if (resolution.has_value()) {
            // 批次 E2：保留完整快照（F-12 需粒度与 MAX_ODT_ENTRY_SIZE）
            m_daq_resolution_info_ = resolution;
            const std::size_t bytes =
                TimestampModeSizeBytes(resolution->timestamp_mode.size_code);
            if (bytes > 0U) {
                m_daq_timestamp_bytes_ = static_cast<std::uint8_t>(bytes);
            }
            // 批次 E2/F-12：TIMESTAMP_MODE 的码表合法性（Table 147/148）。
            // `011` 被规范明文标为 Not allowed；Unit 的 `1101`～`1111` 为保留值。
            if (!IsAllowedTimestampSizeCode(
                    resolution->timestamp_mode.size_code)) {
                throw detail::MakeInvalidArgument(
                    "Slave 声明的 TIMESTAMP_MODE 位宽码为 " +
                    std::to_string(
                        resolution->timestamp_mode.size_code) +
                    "，该组合被规范禁止（原始 PDF 第 185 页 Table 147："
                    "`011` = Not allowed；合法值 0/1/2/4 字节）");
            }
            if (!IsAllowedTimestampUnitCode(
                    resolution->timestamp_mode.unit_code)) {
                throw detail::MakeInvalidArgument(
                    "Slave 声明的 TIMESTAMP_MODE 时间单位码为 " +
                    std::to_string(resolution->timestamp_mode.unit_code) +
                    "，属规范保留值（原始 PDF 第 186 页 Table 148 仅列出 "
                    "0000～1100）");
            }
        }
    }
    return m_daq_timestamp_bytes_.value_or(0U);
}

bool XcpMaster::IsOverloadMsbEnabled() const {
    // O-2：DAQ_PROPERTIES 的 bit7-6 == `01` 表示过载指示使用 PID 的 MSB
    // （原始 PDF 第 179 页 Table 139）。未取证时返回 false——不猜（B-3）。
    if (!m_daq_processor_properties_.has_value()) {
        return false;
    }
    const std::uint8_t props = *m_daq_processor_properties_;
    return ((props & 0xC0U) >> 6) == 0x01U;
}

void XcpMaster::ClearDynamicTableBestEffort() {
    // FREE 之后事务失败：Slave 半表不可复用、旧表已不存在——本地账本必须
    // 同步清零（Decode 只信完整账本，B-6），代际同步作废。再 FREE 一次尽力
    // 复位；二次失败吞掉，绝不掩盖首个异常。
    m_daq_ledger_.clear();
    m_session_.ClearStartedDaqLists();
    m_session_.BumpDaqConfigGeneration();
    LIBXCP_LOG_WARNING(
        "DAQ dynamic configure failed: local ledger and running lists cleared; "
        "generation={}; attempting best-effort FREE_DAQ",
        m_session_.DaqConfigGeneration());
    try {
        m_executor_->ExecuteFreeDaq();
    } catch (...) {
        // 尽力而为：下次 ConfigureDaqListsDynamic 开头的 FREE 会再次收口
        LIBXCP_LOG_WARNING(
            "Best-effort FREE_DAQ failed while clearing dynamic table");
    }
}

void XcpMaster::ConfigureDaqListsDynamic(
    const std::vector<DaqListSpec>& specs) {
    // 保留原有 XCPlite 调用语义：旧的一参数接口强制请求时间戳。
    auto legacy_specs = specs;
    for (auto& spec : legacy_specs) {
        spec.timestamp = true;
    }
    ConfigureDaqListsDynamicImpl(legacy_specs, 1U, true);
}

void XcpMaster::ConfigureDaqListsDynamic(const std::vector<DaqListSpec>& specs,
                                         std::size_t dto_identification_bytes) {
    ConfigureDaqListsDynamicImpl(specs, dto_identification_bytes, false);
}

void XcpMaster::ConfigureDaqListsDynamicImpl(
    const std::vector<DaqListSpec>& specs, std::size_t dto_identification_bytes,
    bool legacy_timestamp) {
    // ---- 1) 入参预检（非法不发命令；整表 FREE 之前一切本地可拦的错误
    //         都不该让 Slave 的既有表白白陪葬）----
    if (dto_identification_bytes == 0U || dto_identification_bytes > 4U) {
        throw detail::MakeInvalidArgument(
            "DTO 识别字段头宽须为 1..4 字节，实际为 " +
            std::to_string(dto_identification_bytes));
    }
    if (specs.empty()) {
        throw detail::MakeInvalidArgument(
            "动态 DAQ 编排至少需要一个 List（ALLOC_DAQ 的 n 不得为 0，"
            "xcplite.c:1148）");
    }
    if (specs.size() > 0xFFFFU) {
        throw detail::MakeInvalidArgument(
            "ALLOC_DAQ 的 n 超出 WORD 上限 65535: " +
            std::to_string(specs.size()));
    }
    // ---- 2) 能力门：动态整表只放行正向声明 DYNAMIC 的 Slave（B-16 不猜；
    //         XCPlite PROPERTIES=0x11 实证，D9/G1）----
    // 注：取证先于编号校验——F-05 需要 MIN_DAQ 才能判定合法编号区间。
    if (!m_daq_processor_info_queried_) {
        m_daq_processor_info_queried_ = true;
        const auto processor = m_executor_->ExecuteGetDaqProcessorInfo();
        if (processor.has_value()) {
            // 批次 E2：保留完整快照（F-05 需 MIN_DAQ、O-2 需 KEY_BYTE）
            m_daq_processor_info_ = processor;
            m_daq_processor_properties_ = processor->properties;
        }
    }
    // O-2：地址扩展位为规范禁止组合（bit5-4 = 10）时立即拒绝。
    // 放在缓存块外，确保任何时候只要有取证快照都会被校验（不只首次查询）。
    if (m_daq_processor_info_.has_value()) {
        ValidateDaqKeyByte(m_daq_processor_info_->key_byte);
    }
    // F-05 修复（批次 E2）：不再假定了 MIN_DAQ==0、编号必从 0 起。
    // 规范原文（原始 PDF 第 180 页）：
    //   "MIN_DAQ is the number of predefined DAQ lists. For predefined DAQ
    //    lists, DAQ_LIST_NUMBER is in the range [0,1,..MIN_DAQ-1].
    //    MAX_DAQ-MIN_DAQ is the number of configurable DAQ lists. For
    //    configurable DAQ lists, DAQ_LIST_NUMBER is in the range
    //    [MIN_DAQ,MIN_DAQ+1,..MAX_DAQ-1]. If DAQ_CONFIG_TYPE = dynamic,
    //    MAX_DAQ equals MIN_DAQ+DAQ_COUNT."
    // MIN_DAQ 未取证时按 0 保守处理——不猜（B-3），与旧行为一致。
    const std::uint16_t min_daq =
        m_daq_processor_info_.has_value()
            ? static_cast<std::uint16_t>(m_daq_processor_info_->min_daq)
            : 0U;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        // 规范约束：可配置列表编号须为 [MIN_DAQ, MIN_DAQ + n - 1]
        // （动态模式下 MAX_DAQ == MIN_DAQ + DAQ_COUNT，故上限由本次分配数决定）
        const std::uint16_t expected =
            static_cast<std::uint16_t>(min_daq + i);
        if (specs[i].daq_list != expected) {
            throw detail::MakeInvalidArgument(
                "ALLOC_DAQ 的列表编号须为连续的 [MIN_DAQ, MIN_DAQ+n-1]："
                "specs[" +
                std::to_string(i) + "] 的 daq_list 应为 " +
                std::to_string(expected) + "（MIN_DAQ=" +
                std::to_string(min_daq) + "），实际为 " +
                std::to_string(specs[i].daq_list));
        }
        PreflightDaqListSpec(specs[i]);
    }
    if (!m_daq_processor_properties_.has_value() ||
        !HasDaqProcessorProperty(
            static_cast<DaqProcessorPropertyBit>(*m_daq_processor_properties_),
            DaqProcessorPropertyBit::kConfigType)) {
        throw detail::MakeUnsupportedFeature(
            "Slave 未声明 DAQ_PROPERTY_CONFIG_TYPE=DYNAMIC（或不支持 "
            "GET_DAQ_PROCESSOR_INFO），动态整表通路不适用；STATIC 表请改用 "
            "ConfigureDaqList");
    }
    // 信封预算严格使用调用方已验证的识别头宽及每个 List 的 timestamp 位；
    // 无时间戳时不消费 timestamp resolution。请求时间戳但无法取证宽度时拒绝。
    const std::size_t cached_timestamp_bytes = DaqTimestampBytesCached();
    const std::size_t ag_bytes = AgToBytes(m_session_.GetAddressGranularity());
    for (const auto& spec : specs) {
        if (spec.timestamp && cached_timestamp_bytes == 0U &&
            !legacy_timestamp) {
            throw detail::MakeUnsupportedFeature(
                "DAQ List 请求时间戳，但 GET_DAQ_RESOLUTION_INFO "
                "未提供可用宽度");
        }
        const std::size_t header_bytes =
            dto_identification_bytes + (spec.dto_counter ? 1U : 0U) +
            (spec.timestamp || legacy_timestamp ? cached_timestamp_bytes : 0U);
        ValidateDaqListAgainstMaxDto(spec, header_bytes, ag_bytes,
                                     m_session_.MaxDto());
        // 批次 E2/F-12：ODT Entry 粒度与对齐前置校验（C1～C5）。
        // 仅当 Slave 声明了粒度才校验——不猜（B-3）。
        if (m_daq_resolution_info_.has_value() &&
            m_daq_resolution_info_->granularity_daq != 0U) {
            ValidateOdtEntryGranularity(
                spec, m_daq_resolution_info_->granularity_daq,
                m_daq_resolution_info_->max_odt_entry_size_daq, ag_bytes);
        }
        // 批次 E2/O-2：OVERLOAD_MSB 启用时 ODT 编号须严格小于 0x7C
        ValidateOdtNumberAgainstOverloadMsb(spec, IsOverloadMsbEnabled());
    }

    LIBXCP_LOG_INFO("DAQ dynamic configure started lists={} dto_id_bytes={}",
                    specs.size(), dto_identification_bytes);
    // ---- 3) 整表重建事务：FREE → ALLOC_DAQ → ALLOC_ODT* → ALLOC_ODT_ENTRY*
    //         → (SET_DAQ_PTR+WRITE_DAQ)* → SET_DAQ_LIST_MODE*。时序硬门
    //         （xcplite.c:1143-1186）：ALLOC_DAQ 一次给足总数；一个 List 的
    //         全部 ODT 分配完才能开始分 Entry。失败：整表 FREE 复位+账本清零。
    m_executor_->ExecuteFreeDaq();
    // FREE 成功即全表作废（含此前静态通路的登记）：代际推进、运行态清零
    m_daq_ledger_.clear();
    m_session_.ClearStartedDaqLists();
    m_session_.BumpDaqConfigGeneration();
    LIBXCP_LOG_INFO(
        "DAQ FREE_DAQ confirmed; old ledger/running lists cleared "
        "generation={}",
        m_session_.DaqConfigGeneration());

    try {
        m_executor_->ExecuteAllocDaq(static_cast<std::uint16_t>(specs.size()));
        for (const auto& spec : specs) {
            m_executor_->ExecuteAllocOdt(
                spec.daq_list, static_cast<std::uint8_t>(spec.odts.size()));
        }
        for (const auto& spec : specs) {
            std::uint8_t odt_no = 0U;
            for (const auto& odt : spec.odts) {
                m_executor_->ExecuteAllocOdtEntry(
                    spec.daq_list, odt_no,
                    static_cast<std::uint8_t>(odt.entries.size()));
                ++odt_no;
            }
        }
        for (const auto& spec : specs) {
            std::uint8_t odt_no = 0U;
            for (const auto& odt : spec.odts) {
                std::uint8_t entry_no = 0U;
                for (const auto& e : odt.entries) {
                    m_executor_->ExecuteSetDaqPtr(spec.daq_list, odt_no,
                                                  entry_no);
                    m_executor_->ExecuteWriteDaq(e.bit_offset, e.size,
                                                 e.extension, e.address);
                    DaqLedgerEntry rec;
                    rec.daq_list = spec.daq_list;
                    rec.odt_number = odt_no;
                    rec.odt_entry = entry_no;
                    rec.address = e.address;
                    rec.extension = e.extension;
                    rec.size = e.size;
                    rec.bit_offset = e.bit_offset;
                    // rec.pid 留 nullopt：XCPlite 识别字段是
                    // ODT(rel)+0xAA(FIL)+DAQ16（DAQ_KEY=0xC0），
                    // FIRST_PID/Absolute 推导不成立（docs L2228，D19）
                    m_daq_ledger_.push_back(rec);
                    ++entry_no;
                }
                ++odt_no;
            }
        }
        for (const auto& spec : specs) {
            // List mode 精确按 spec 下发；XCPlite 兼容 profile 显式设置
            // timestamp， generic profile 可安全选择无时间戳布局。
            const DaqListModeBit mode = DaqModeBitsFromSpec(spec);
            m_executor_->ExecuteSetDaqListMode(mode, spec.daq_list,
                                               spec.event_channel,
                                               spec.prescaler, spec.priority);
        }
    } catch (const XcpException&) {
        ClearDynamicTableBestEffort();
        throw;
    } catch (const std::exception& e) {
        ClearDynamicTableBestEffort();
        throw detail::MakeTransportError("动态 DAQ 编排过程中发生非协议异常",
                                         e.what());
    }
    LIBXCP_LOG_INFO(
        "DAQ dynamic configure completed lists={} ledger_entries={} "
        "generation={}",
        specs.size(), m_daq_ledger_.size(), m_session_.DaqConfigGeneration());
}

void XcpMaster::StartDaqSync() {
    // 账本去重取 List 集合（配置顺序即 0..n-1）
    std::vector<std::uint16_t> lists;
    for (const auto& e : m_daq_ledger_) {
        if (std::find(lists.begin(), lists.end(), e.daq_list) == lists.end()) {
            lists.push_back(e.daq_list);
        }
    }
    if (lists.empty()) {
        LIBXCP_LOG_ERROR(
            "XCP StartDaqSync rejected: no DAQ lists are configured");
        throw detail::MakeInvalidState(
            "没有任何已配置的 DAQ List（账本为空），拒绝同步启动");
    }
    LIBXCP_LOG_INFO("DAQ synchronized start selecting lists={}", lists.size());
    // 逐表 SELECT → START_STOP_SYNCH(START)：XCPlite 下单表 START(mode=1)
    // 被 TEST_CHECKS 拒为 CRC_MODE_NOT_VALID（D12），SELECT+SYNCH 是唯一
    // 实证可走的启动路径。不做 FIRST_PID 回填（识别字段非 Absolute，pid
    // 保持 nullopt，B-16）。
    for (const std::uint16_t list : lists) {
        (void)m_executor_->ExecuteStartStopDaqList(DaqListAction::Select, list);
    }
    m_executor_->ExecuteStartStopSynch(DaqSynchAction::StartSelected);
    for (const std::uint16_t list : lists) {
        m_session_.MarkDaqListStarted(list);
    }
    LIBXCP_LOG_INFO("DAQ synchronized start confirmed running_lists={}",
                    lists.size());
}

Bytes XcpMaster::FetchA2lViaUpload() {
    // 批次21 21-2：IDT 4 = IDT_ASAM_UPLOAD（XCPlite xcp.h:252）。对端响应
    // MODE=0x00 + LENGTH（DWORD，文件字节数），内容只能靠后续 UPLOAD 从
    // FILE MTA 顺序取；对端无 A2L 上传（fopen 失败/文件空）时上报 LENGTH=0。
    if (!m_session_.IsConnected()) {
        throw detail::MakeInvalidState(
            "FetchA2lViaUpload 需要先 Connect Slave");
    }
    const GetIdResponse id = m_executor_->ExecuteGetId(0x04U);
    // F-03b：Table 61 的 Mode 是位域——bit 1 = COMPRESSED_ENCRYPTED、
    // bit 0 = TRANSFER_MODE。必须先拒绝「压缩/加密」这一正交标志：库未实现
    // 外部解压/解密算法，若只判 transfer_mode 而放行，Mode=0x02/0x03 会被
    // 当作普通 UPLOAD 通路，把密文当 A2L 文本交给解析器。
    if (id.compressed_encrypted) {
        throw detail::MakeUnsupportedFeature(
            "GET_ID(IDT=4) 置位 COMPRESSED_ENCRYPTED（Mode bit1，raw=" +
            std::to_string(id.raw_mode) +
            "）：标识数据经压缩/加密，本库未实现外部解压解密算法，拒绝按未"
            "处理的原始 A2L 处理");
    }
    // 保留位（bit 7..2）非零：规范标为 x，属未知扩展，不得静默按当前语义处理。
    if (!id.reserved_bits_clear) {
        throw detail::MakeMalformedPacket(
            "GET_ID(IDT=4) 响应 Mode 保留位非零（raw=" +
            std::to_string(id.raw_mode) +
            "）；按原始 PDF 第 124 页 Table 61，bit 7..2 为保留位");
    }
    if (id.transfer_mode != 0x00U) {
        throw detail::MakeUnsupportedFeature(
            "GET_ID(IDT=4) 返回 MODE=" + std::to_string(id.transfer_mode) +
            "（非 0），非 UPLOAD 文件通路，本批不编排");
    }
    // F-03c：Table 60 下方原文「The following rule applies: Length mod AG = 0」。
    // 长度不满足 AG 整倍时不得继续编排——后续 UPLOAD 的元素数换算
    // （Number of Data Elements UPLOAD [AG] = Length / AG，Table 61 原文）
    // 会因整除截断而少读尾部字节，且 Slave 本身违反规范。
    if (!id.length_aligned_to_ag) {
        throw detail::MakeMalformedPacket(
            "GET_ID(IDT=4) 上报长度 " + std::to_string(id.length) +
            " 不满足 Length mod AG = 0（AG=" +
            std::to_string(AgToBytes(m_session_.GetAddressGranularity())) +
            " 字节）；按原始 PDF 第 123 页 Table 60 属违规响应");
    }
    if (id.length == 0U) {
        throw detail::MakeUnsupportedFeature(
            "GET_ID(IDT=4) 上报长度 0：Slave 未启用 A2L 上传或 .a2l 文件缺失");
    }
    // 对端 FILE 分支是纯顺序读（xcpappl.c:575-596，无 fseek）：必须按上报
    // LENGTH 严格连续分块；单块 ≤ MAX_CTO-1（RES 占 1 字节）且 ≤ 255
    // （UPLOAD 元素数为单字节字段）。中途失败对端已 closeFile，直接上抛，
    // 调用方需重发 GET_ID 重开文件再整读。
    const std::size_t chunk_max =
        (std::min)(static_cast<std::size_t>(m_session_.MaxCto()) - 1U,
                   static_cast<std::size_t>(255U));
    Bytes file;
    file.reserve((std::min)(static_cast<std::size_t>(id.length),
                            std::size_t{1U << 20}));
    std::size_t remaining = static_cast<std::size_t>(id.length);
    while (remaining > 0U) {
        const auto chunk =
            static_cast<ElementCount>((std::min)(remaining, chunk_max));
        Bytes part = m_executor_->ExecuteUpload(chunk);
        file.insert(file.end(), part.begin(), part.end());
        remaining -= chunk;
    }
    return file;
}

const std::vector<DaqLedgerEntry>& XcpMaster::DaqLedger() const noexcept {
    return m_daq_ledger_;
}

std::uint32_t XcpMaster::DaqConfigGeneration() const {
    return m_session_.DaqConfigGeneration();
}

// ---- DAQ 运行时取证薄转发（批次14；零策略，只把 CommandExecutor 的可选命令
//      暴露给门面调用方，见 xcp_master.hpp 中对应注释）----

std::optional<GetDaqProcessorInfoResponse> XcpMaster::QueryDaqProcessorInfo() {
    return m_executor_->ExecuteGetDaqProcessorInfo();
}

std::optional<GetDaqResolutionInfoResponse>
XcpMaster::QueryDaqResolutionInfo() {
    return m_executor_->ExecuteGetDaqResolutionInfo();
}

std::optional<GetDaqListInfoResponse> XcpMaster::QueryDaqListInfo(
    std::uint16_t daq_list) {
    return m_executor_->ExecuteGetDaqListInfo(daq_list);
}

std::optional<GetDaqEventInfoResponse> XcpMaster::QueryDaqEventInfo(
    std::uint16_t event_channel) {
    return m_executor_->ExecuteGetDaqEventInfo(event_channel);
}

std::optional<ReadDaqResponse> XcpMaster::ReadDaqEntryAt(
    std::uint16_t daq_list, std::uint8_t odt_number, std::uint8_t odt_entry) {
    // 先定位再回读：不依赖隐含指针的自增状态（docs L2172 指针不可查询）
    m_executor_->ExecuteSetDaqPtr(daq_list, odt_number, odt_entry);
    return m_executor_->ExecuteReadDaq();
}

SessionParameters XcpMaster::GetSessionParameters() const {
    return m_session_.Parameters();
}

SessionState XcpMaster::GetSessionState() const { return m_session_.State(); }

GetStatusResponse XcpMaster::QueryStatus() {
    return m_executor_->ExecuteGetStatus();
}

UnlockResult XcpMaster::Unlock(Resource resource,
                               const SeedKeyCalculator& calculator) {
    // ---- 1) 本地预检（计划 §4.5：非法参数不发送）----
    const auto resource_mask = static_cast<ResourceMask>(resource);
    // 必须恰为单个资源位：非 0、无多余置位、且属于 CAL/PAG|DAQ|STIM|PGM
    const bool is_single_bit =
        resource_mask != 0U && (resource_mask & (resource_mask - 1U)) == 0U;
    if (!is_single_bit || (resource_mask & ~kValidUnlockResources) != 0U) {
        throw detail::MakeInvalidArgument(
            "Unlock 仅支持单个资源位（CAL_PAG/DAQ/STIM/PGM 之一），收到掩码 " +
            std::to_string(resource_mask));
    }
    if (!calculator) {
        throw detail::MakeInvalidArgument(
            "Unlock 的 SeedKeyCalculator 回调不能为空");
    }

    // Seed/Key 每帧最大载荷 = MAX_CTO - 2（协议已保证 MAX_CTO >= 8）
    const auto max_segment =
        static_cast<std::size_t>(m_session_.MaxCto()) - kSeedKeyHeaderBytes;

    // ---- 2) 分段 Seed&Key 事务（含 GET_SEED 与 UNLOCK 两段）----
    // F-10：整个事务放进重试循环。执行器在分段超时后会按事务语义复位
    // （GET_SEED 段：清状态 + SYNCH；UNLOCK 段：直接报告结果未知），
    // 本层只能从 GET_SEED(Mode=0) 完整重启，绝不重放中途的续段。
    constexpr int kMaxUnlockSequenceRestarts = 1;
    for (int sequence_restart = 0;; ++sequence_restart) {
        try {
            // ---- 2a) GET_SEED 首段，获得 Seed 总长度 ----
            const GetSeedResponse first =
                m_executor_->ExecuteGetSeed(resource, SeedMode::First);
            if (first.length == 0U) {
                // Length=0：资源未保护，无需 UNLOCK（规范 §7.5.1.8），
                // 不调用回调、不发送 UNLOCK
                m_executor_->ResetSeedKeyTransaction();
                return UnlockResult{true, std::nullopt};
            }

            // ---- 2b) 分段收集 Seed（Remainder 帧）----
            Bytes seed(first.seed.begin(), first.seed.end());
            if (seed.empty()) {
                throw detail::MakeMalformedPacket(
                    "GET_SEED 声明 Length " + std::to_string(first.length) +
                    " 但首段 Seed 为空");
            }
            if (seed.size() > first.length) {
                throw detail::MakeMalformedPacket(
                    "GET_SEED 首段 Seed 字节数 " + std::to_string(seed.size()) +
                    " 超过声明总长度 " + std::to_string(first.length));
            }
            while (seed.size() < first.length) {
                const GetSeedResponse part =
                    m_executor_->ExecuteGetSeed(resource, SeedMode::Remainder);
                if (part.seed.empty()) {
                    // 空续段无法推进进度，终止以防死循环
                    throw detail::MakeMalformedPacket(
                        "GET_SEED 续段返回空 Seed，无法收满声明总长度 " +
                        std::to_string(first.length));
                }
                if (seed.size() + part.seed.size() > first.length) {
                    throw detail::MakeMalformedPacket(
                        "GET_SEED 续段累计字节数超过声明总长度 " +
                        std::to_string(first.length));
                }
                seed.insert(seed.end(), part.seed.begin(), part.seed.end());
            }

            // ---- 2c) 调用方算法计算 Key（异常原样传播）----
            Bytes key = calculator(resource, BytesView{seed});
            if (key.empty()) {
                throw detail::MakeInvalidArgument(
                    "SeedKeyCalculator 返回空 Key，无法执行 UNLOCK");
            }
            if (key.size() > kMaxKeyLength) {
                throw detail::MakeInvalidArgument(
                    "SeedKeyCalculator 返回的 Key 长度 " +
                    std::to_string(key.size()) + " 字节超过 Length 字段上限 255");
            }

            // ---- 2d) 分段发送 UNLOCK：首帧 Length=总长，后续帧=剩余 ----
            std::size_t offset = 0;
            UnlockResponse last{};
            do {
                const std::size_t remaining = key.size() - offset;
                const std::size_t segment = std::min(remaining, max_segment);
                const auto length_field = static_cast<std::uint8_t>(
                    offset == 0U ? key.size() : remaining);
                const bool is_final = (offset + segment) >= key.size();
                last = m_executor_->ExecuteUnlock(
                    length_field, BytesView{key}.subspan(offset, segment),
                    is_final);
                offset += segment;
            } while (offset < key.size());

            // 事务完成：清除分段状态，避免影响后续 Seed&Key 调用
            m_executor_->ResetSeedKeyTransaction();
            return UnlockResult{false, last.resource_protection};
        } catch (const XcpException& e) {
            // 仅 GET_SEED 段超时可安全地从 Mode=0 完整重启一次（执行器已
            // 证据：Slave 侧 Seed 序列未完成，重发 Mode=0 即可复位）。
            // UNLOCK 段与末段的失败结果未知，一律上交调用方，绝不重放。
            const bool is_get_seed_timeout =
                e.Category() == ErrorCategory::RecoveryFailed &&
                e.GetCommandCode() == CommandCode::GetSeed;
            if (!is_get_seed_timeout ||
                sequence_restart >= kMaxUnlockSequenceRestarts) {
                m_executor_->ResetSeedKeyTransaction();
                throw;
            }
            LIBXCP_LOG_WARNING(
                "XCP seed&key transaction restarting from GET_SEED(Mode=0) "
                "restart={}",
                sequence_restart + 1);
            // 回到 2a：重新从 GET_SEED(Mode=0) 收满 Seed 并重算 Key
        }
    }
}

// ---------------------------------------------------------------------------
// 变量标定批次：Calibration / Page Switching（docs §7.5.2.5 / §7.5.3）
// ---------------------------------------------------------------------------

bool XcpMaster::HasCalPagResource() const {
    const auto params = m_session_.Parameters();
    return HasResource(params.connect.resource_mask, Resource::CalPag);
}

std::optional<GetPagProcessorInfoResponse> XcpMaster::QueryPagProcessorInfo() {
    if (!IsConnected()) {
        throw detail::MakeInvalidState("查询 PAG 处理器信息前必须先建立 XCP 连接");
    }
    if (!HasCalPagResource()) {
        throw detail::MakeUnsupportedFeature(
            "Slave 的 CONNECT RESOURCE 未声明 CAL/PAG 资源，Page Switching "
            "不可用");
    }
    // 一次性取证缓存：Connect() 已探测过则直接返回；nullopt 也可能来自
    // "尚未探测"（构造后未 Connect 的路径），懒补一次
    if (m_pag_processor_info_queried_) {
        return m_pag_processor_info_;
    }
    m_pag_processor_info_queried_ = true;
    m_pag_processor_info_ = m_executor_->ExecuteGetPagProcessorInfo();
    if (m_pag_processor_info_) {
        m_session_.SetPagProcessorInfo(*m_pag_processor_info_);
    }
    return m_pag_processor_info_;
}

/// @brief 标定命令统一入口预检：已连接 + Slave 声明 CAL/PAG 资源
namespace {
void RequireCalPag(bool connected, bool has_cal_pag) {
    if (!connected) {
        throw detail::MakeInvalidState("执行标定命令前必须先建立 XCP 连接");
    }
    if (!has_cal_pag) {
        throw detail::MakeUnsupportedFeature(
            "Slave 的 CONNECT RESOURCE 未声明 CAL/PAG 资源，标定命令不可用");
    }
}
}  // namespace

void XcpMaster::SetCalPage(CalPageModeBit mode, std::uint8_t segment,
                           std::uint8_t page) {
    RequireCalPag(m_session_.IsConnected(), HasCalPagResource());
    m_executor_->ExecuteSetCalPage(mode, segment, page);
}

std::optional<GetCalPageResponse> XcpMaster::GetCalPage(
    CalPageAccessMode access_mode, std::uint8_t segment) {
    RequireCalPag(m_session_.IsConnected(), HasCalPagResource());
    return m_executor_->ExecuteGetCalPage(access_mode, segment);
}

std::optional<GetSegmentInfoResponse> XcpMaster::GetSegmentInfo(
    SegmentInfoMode mode, std::uint8_t segment, SegmentInfoSelector info,
    std::uint8_t mapping_index) {
    RequireCalPag(m_session_.IsConnected(), HasCalPagResource());
    return m_executor_->ExecuteGetSegmentInfo(mode, segment, info,
                                              mapping_index);
}

std::optional<GetPageInfoResponse> XcpMaster::GetPageInfo(
    std::uint8_t segment, std::uint8_t page) {
    RequireCalPag(m_session_.IsConnected(), HasCalPagResource());
    return m_executor_->ExecuteGetPageInfo(segment, page);
}

void XcpMaster::SetSegmentFreeze(bool freeze, std::uint8_t segment) {
    RequireCalPag(m_session_.IsConnected(), HasCalPagResource());
    m_executor_->ExecuteSetSegmentMode(
        freeze ? SegmentModeBit::kFreeze : SegmentModeBit::kNone, segment);
}

std::optional<GetSegmentModeResponse> XcpMaster::GetSegmentMode(
    std::uint8_t segment) {
    RequireCalPag(m_session_.IsConnected(), HasCalPagResource());
    return m_executor_->ExecuteGetSegmentMode(segment);
}

void XcpMaster::CopyCalPage(const CopyCalPageRequest& request) {
    RequireCalPag(m_session_.IsConnected(), HasCalPagResource());
    m_executor_->ExecuteCopyCalPage(request);
}

void XcpMaster::ModifyBits(Address address, AddressExtension extension,
                           std::uint8_t shift, std::uint16_t and_mask,
                           std::uint16_t xor_mask) {
    RequireCalPag(m_session_.IsConnected(), HasCalPagResource());
    m_memory_access_->ModifyBits(address, extension, shift, and_mask,
                                 xor_mask);
}

}  // namespace calmcar::xcp
