/**
 * @file xcp_master.cpp
 * @brief XcpMaster 实现：连接编排、失败清理与内存读取转发。
 */

#include "libxcp/xcp_master.hpp"

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
/// @details docs §7.5.4.10：Timestamp Size ∈ {0,1,2,4} 字节；其余码值为
///          未定义组合——返回 0（不计入），**不猜**（B-3）。
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

}  // namespace

XcpMaster::XcpMaster(std::unique_ptr<IXcpTransport> transport,
                     CommandTimeouts timeouts, IEventListener* event_listener)
    : m_transport_(std::move(transport)) {
    if (!m_transport_) {
        throw detail::MakeInvalidArgument(
            "XcpMaster 需要非空的 IXcpTransport 实例");
    }
    m_executor_ = std::make_unique<CommandExecutor>(*m_transport_, m_session_,
                                                    timeouts, event_listener);
    m_memory_access_ = std::make_unique<MemoryAccess>(*m_executor_, m_session_);
}

XcpMaster::~XcpMaster() {
    // 析构路径不抛异常：尽力而为地关闭通道，本地状态随之释放
    try {
        if (m_session_.IsConnected()) {
            try {
                m_executor_->ExecuteDisconnect();
            } catch (...) {
                m_session_.Reset();
            }
        }
        m_transport_->Close();
    } catch (...) {
    }
    // Executor 持有 Transport 引用，必须先于 Transport 销毁
    m_memory_access_.reset();
    m_executor_.reset();
}

void XcpMaster::Connect() {
    // 已连接时拒绝重复连接（避免在活跃会话上重放 CONNECT）
    if (m_session_.IsConnected()) {
        throw detail::MakeInvalidState("Session 已处于 Connected 状态");
    }
    m_session_.Reset();  // 清理 Failed/残留状态
    // 新会话：本端 WRITE_DAQ 账本随之作废（Session 的 DAQ 运行态在
    // EstablishConnection 里一并归零）
    m_daq_ledger_.clear();
    m_daq_processor_info_queried_ = false;
    m_daq_processor_properties_.reset();
    m_daq_resolution_info_queried_ = false;
    m_daq_timestamp_bytes_.reset();

    // Transport.Open 的监听器即 CommandExecutor；Open 之后接收线程立即开始回调
    m_transport_->Open(m_executor_->AsListener());

    auto cleanup_on_failure = [this] {
        // CONNECT、参数校验或 GET_STATUS 失败时清除部分状态并关闭通道（计划
        // §5.1）
        m_session_.Reset();
        try {
            m_transport_->Close();
        } catch (...) {
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
    } catch (const XcpException&) {
        cleanup_on_failure();
        throw;
    } catch (const std::exception& e) {
        cleanup_on_failure();
        throw detail::MakeTransportError("连接过程中发生非协议异常", e.what());
    }
}

void XcpMaster::Disconnect() {
    // 未连接：本地幂等，仅确保通道关闭
    if (m_session_.State() == SessionState::Disconnected) {
        m_transport_->Close();
        return;
    }

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

    if (failure) {
        throw *failure;
    }
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
            m_daq_processor_properties_ = processor->properties;
        }
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
    if (spec.timestamp) {
        header_bytes += DaqTimestampBytesCached();
    }
    ValidateDaqListAgainstMaxDto(spec, header_bytes, ag_bytes,
                                 m_session_.MaxDto());

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
        throw;
    } catch (const std::exception& e) {
        rollback();
        throw detail::MakeTransportError("DAQ 配置过程中发生非协议异常",
                                         e.what());
    }
}

std::uint8_t XcpMaster::StartDaqList(std::uint16_t daq_list) {
    const bool configured = std::any_of(
        m_daq_ledger_.begin(), m_daq_ledger_.end(),
        [daq_list](const DaqLedgerEntry& e) { return e.daq_list == daq_list; });
    if (!configured) {
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
    return resp.first_pid;
}

void XcpMaster::StopDaqList(std::uint16_t daq_list) {
    (void)m_executor_->ExecuteStartStopDaqList(DaqListAction::Stop, daq_list);
    m_session_.MarkDaqListStopped(daq_list);
}

void XcpMaster::StopDaq() {
    m_executor_->ExecuteStartStopSynch(DaqSynchAction::StopAll);
    // 配置仍在 Slave 里，账本保留；只清运行态（重新 Start 不需要重写）
    m_session_.ClearStartedDaqLists();
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
}

std::size_t XcpMaster::DaqTimestampBytesCached() {
    // 一次性取证（G3）：GET_DAQ_RESOLUTION_INFO 属 Optional 面——Slave 不支持
    // （CMD_UNKNOWN→nullopt）时按 0 字节计，不猜宽度（B-3）；结果缓存，
    // Connect() 重置（D16）。
    if (!m_daq_resolution_info_queried_) {
        m_daq_resolution_info_queried_ = true;
        const auto resolution = m_executor_->ExecuteGetDaqResolutionInfo();
        if (resolution.has_value()) {
            const std::size_t bytes =
                TimestampModeSizeBytes(resolution->timestamp_mode.size_code);
            if (bytes > 0U) {
                m_daq_timestamp_bytes_ = static_cast<std::uint8_t>(bytes);
            }
        }
    }
    return m_daq_timestamp_bytes_.value_or(0U);
}

void XcpMaster::ClearDynamicTableBestEffort() {
    // FREE 之后事务失败：Slave 半表不可复用、旧表已不存在——本地账本必须
    // 同步清零（Decode 只信完整账本，B-6），代际同步作废。再 FREE 一次尽力
    // 复位；二次失败吞掉，绝不掩盖首个异常。
    m_daq_ledger_.clear();
    m_session_.ClearStartedDaqLists();
    m_session_.BumpDaqConfigGeneration();
    try {
        m_executor_->ExecuteFreeDaq();
    } catch (...) {
        // 尽力而为：下次 ConfigureDaqListsDynamic 开头的 FREE 会再次收口
    }
}

void XcpMaster::ConfigureDaqListsDynamic(
    const std::vector<DaqListSpec>& specs) {
    // ---- 1) 入参预检（非法不发命令；整表 FREE 之前一切本地可拦的错误
    //         都不该让 Slave 的既有表白白陪葬）----
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
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].daq_list != static_cast<std::uint16_t>(i)) {
            throw detail::MakeInvalidArgument(
                "ALLOC_DAQ 按分配顺序从 0 编号（MIN_DAQ=0，Slave 侧 daq_count "
                "自增）：specs[" +
                std::to_string(i) + "] 的 daq_list 必须等于其下标 " +
                std::to_string(i) + "，实际为 " +
                std::to_string(specs[i].daq_list));
        }
        PreflightDaqListSpec(specs[i]);
    }

    // ---- 2) 能力门：动态整表只放行正向声明 DYNAMIC 的 Slave（B-16 不猜；
    //         XCPlite PROPERTIES=0x11 实证，D9/G1）----
    if (!m_daq_processor_info_queried_) {
        m_daq_processor_info_queried_ = true;
        const auto processor = m_executor_->ExecuteGetDaqProcessorInfo();
        if (processor.has_value()) {
            m_daq_processor_properties_ = processor->properties;
        }
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
    // ---- 2b) G3 修正：动态信封识别字段后跟随 Slave 声明的时间戳。XCPlite
    //          强制 TIMESTAMP 模式位（D11）⇒ Slave 必回带时间戳，据此预检
    //          ODT 预算；未声明（Optional 面缺失）按 0 字节计，不猜（B-3）。
    const std::size_t timestamp_bytes = DaqTimestampBytesCached();
    const std::size_t ag_bytes = AgToBytes(m_session_.GetAddressGranularity());
    for (const auto& spec : specs) {
        const std::size_t header_bytes =
            1U + (spec.dto_counter ? 1U : 0U) + timestamp_bytes;
        ValidateDaqListAgainstMaxDto(spec, header_bytes, ag_bytes,
                                     m_session_.MaxDto());
    }

    // ---- 3) 整表重建事务：FREE → ALLOC_DAQ → ALLOC_ODT* → ALLOC_ODT_ENTRY*
    //         → (SET_DAQ_PTR+WRITE_DAQ)* → SET_DAQ_LIST_MODE*。时序硬门
    //         （xcplite.c:1143-1186）：ALLOC_DAQ 一次给足总数；一个 List 的
    //         全部 ODT 分配完才能开始分 Entry。失败：整表 FREE 复位+账本清零。
    m_executor_->ExecuteFreeDaq();
    // FREE 成功即全表作废（含此前静态通路的登记）：代际推进、运行态清零
    m_daq_ledger_.clear();
    m_session_.ClearStartedDaqLists();
    m_session_.BumpDaqConfigGeneration();

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
            // mode 位按 spec 构造并强制 kTimestamp（XCPlite 缺位回
            // CRC_CMD_SYNTAX，D11）；stim/dto_counter/pid_off 本地预拒
            const DaqListModeBit mode =
                DaqModeBitsFromSpec(spec) | DaqListModeBit::kTimestamp;
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
        throw detail::MakeInvalidState(
            "没有任何已配置的 DAQ List（账本为空），拒绝同步启动");
    }
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
    if (id.transfer_mode != 0x00U) {
        throw detail::MakeUnsupportedFeature(
            "GET_ID(IDT=4) 返回 MODE=" + std::to_string(id.transfer_mode) +
            "（非 0），非 UPLOAD 文件通路，本批不编排");
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

    // ---- 2) GET_SEED 首段，获得 Seed 总长度 ----
    const GetSeedResponse first =
        m_executor_->ExecuteGetSeed(resource, SeedMode::First);
    if (first.length == 0U) {
        // Length=0：资源未保护，无需 UNLOCK（规范 §7.5.1.8），
        // 不调用回调、不发送 UNLOCK
        return UnlockResult{true, std::nullopt};
    }

    // ---- 3) 分段收集 Seed（Remainder 帧）----
    Bytes seed(first.seed.begin(), first.seed.end());
    if (seed.empty()) {
        throw detail::MakeMalformedPacket("GET_SEED 声明 Length " +
                                          std::to_string(first.length) +
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

    // ---- 4) 调用方算法计算 Key（异常原样传播）----
    Bytes key = calculator(resource, BytesView{seed});
    if (key.empty()) {
        throw detail::MakeInvalidArgument(
            "SeedKeyCalculator 返回空 Key，无法执行 UNLOCK");
    }
    if (key.size() > kMaxKeyLength) {
        throw detail::MakeInvalidArgument("SeedKeyCalculator 返回的 Key 长度 " +
                                          std::to_string(key.size()) +
                                          " 字节超过 Length 字段上限 255");
    }

    // ---- 5) 分段发送 UNLOCK：首帧 Length=Key 总长，后续帧=剩余长度 ----
    std::size_t offset = 0;
    UnlockResponse last{};
    do {
        const std::size_t remaining = key.size() - offset;
        const std::size_t segment = std::min(remaining, max_segment);
        const auto length_field =
            static_cast<std::uint8_t>(offset == 0U ? key.size() : remaining);
        last = m_executor_->ExecuteUnlock(
            length_field, BytesView{key}.subspan(offset, segment));
        offset += segment;
    } while (offset < key.size());

    return UnlockResult{false, last.resource_protection};
}

}  // namespace calmcar::xcp
