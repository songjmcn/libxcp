/**
 * @file response_parser.cpp
 * @brief ResponseParser 的实现。
 *
 * 响应布局依据 docs/XCP_1.3.0_document.md 第 7.5.1 / 12.4 节与设计文档第 17
 * 节字节表。 COMM_MODE_BASIC 位定义（已与 XCP 1.3.0 文档 12.4 示例 FF 15 C0 08
 * 08 00 10 10 交叉验证）： bit0    BYTE_ORDER            0=Intel(小端)
 * 1=Motorola(大端) bit1-2  ADDRESS_GRANULARITY   00=BYTE 01=WORD 10=DWORD
 * 11=保留 bit6    SLAVE_BLOCK_MODE_SUPPORTED bit7 OPTIONAL（GET_COMM_MODE_INFO
 * 可用）
 */

#include "libxcp/response_parser.hpp"

namespace calmcar::xcp {

namespace {

/// @brief CONNECT 响应去掉 PID 后的最小长度：RESOURCE..TransportVersion
constexpr std::size_t kConnectResMinSize = 7;
/// @brief GET_STATUS 响应去掉 PID 后的最小长度
constexpr std::size_t kGetStatusResMinSize = 5;
/// @brief GET_COMM_MODE_INFO 响应去掉 PID 后的最小长度
constexpr std::size_t kGetCommModeInfoResMinSize = 7;
/// @brief GET_SEED / UNLOCK 响应去掉 PID 后的最小长度（[length]/[protection]）
constexpr std::size_t kSeedKeyResMinSize = 1;
/// @brief START_STOP_DAQ_LIST 响应去掉 PID 后的最小长度（[FIRST_PID]）
constexpr std::size_t kStartStopDaqListResMinSize = 1;
/// @brief GET_DAQ_LIST_INFO 响应去掉 PID 后的最小长度
/// （PROPERTIES/MAX_ODT/MAX_ODT_ENTRY/FIXED_EVENT(WORD)）
constexpr std::size_t kGetDaqListInfoResMinSize = 5;
/// @brief GET_DAQ_LIST_MODE 响应去掉 PID 后的最小长度
/// （Current Mode/reserved(WORD)/Event Channel(WORD)/Prescaler/Priority）
constexpr std::size_t kGetDaqListModeResMinSize = 7;
/// @brief GET_DAQ_PACKED_MODE 响应去掉 PID 后的最小长度
/// @details Table 128：mode=0 时 RES = [0xFF][Reserved][DAQ_PACKED_MODE]，
///          剥掉 PID 后 2 字节；mode=1/2（Table 129）追加
///          [DPM_TIMESTAMP_MODE][DPM_SAMPLE_COUNT WORD] 共 5 字节。
constexpr std::size_t kDaqPackedModeResMinSize = 2;
/// @brief GET_DAQ_PACKED_MODE 在 packed 模式下的响应长度（含 PID 时 6）
constexpr std::size_t kDaqPackedModeResPackedSize = 5;
/// @brief BUILD_CHECKSUM 响应去掉 PID 后的最小长度
/// （Checksum type/Reserved(WORD)/Checksum(DWORD)）
constexpr std::size_t kBuildChecksumResMinSize = 7;
/// @brief DTO_CTR_PROPERTIES 响应去掉 PID 后的最小长度
/// @details 规范 Table 164：RES = [0xFF][PROPERTIES BYTE]
///          [Current Related Event Channel WORD][MODE BYTE]，剥掉 PID 后正好
///          是 1 + 2 + 1 = 4 字节（不是 5）
constexpr std::size_t kDtoCtrPropertiesResMinSize = 4;
/// @brief GET_DAQ_CLOCK 响应去掉 PID 后的最小长度
/// @details legacy Table 133：RES = [0xFF][reserved][TRIGGER_INFO]
///          [PAYLOAD_FMT][DWORD]，剥掉 PID 后是 1 + 1 + 1 + 4 = 7 字节；
///          extended 至少要能读到 slave 时钟，由解析函数按 PAYLOAD_FMT 再判。
constexpr std::size_t kGetDaqClockResMinSize = 7;
/// @brief TIME_CORRELATION_PROPERTIES 响应去掉 PID 后的长度
/// @details Table 212：RES = [0xFF][SLAVE_CONFIG][OBSERVABLE_CLOCKS]
///          [SYNC_STATE][CLOCK_INFO][RESERVED][CLUSTER_ID(WORD)]，
///          剥掉 PID 后固定 1+1+1+1+1+2 = **7** 字节（含 PID 才是 8）。
constexpr std::size_t kTimeCorrelationResMinSize = 7;
/// @brief EV_TIME_SYNC legacy 载荷最小长度（reserved/reserved/DWORD）
constexpr std::size_t kTimeSyncEventLegacyMinSize = 6;
/// @brief GET_DAQ_EVENT_INFO 响应去掉 PID 后的最小长度（v0.3；XCPlite 实然
/// xcp.h:819-825）
/// （PROPERTIES/MAX_DAQ_LISTS/NAME_LENGTH/TIME_CYCLE/TIME_UNIT/PRIORITY）
constexpr std::size_t kGetDaqEventInfoResMinSize = 6;
/// @brief GET_DAQ_RESOLUTION_INFO 响应去掉 PID 后的最小长度
/// （DAQ 粒度+上限、STIM 粒度+上限、TIMESTAMP_MODE、TIMESTAMP_TICKS(WORD)）
constexpr std::size_t kGetDaqResolutionInfoResMinSize = 7;
/// @brief GET_DAQ_PROCESSOR_INFO 响应去掉 PID 后的最小长度
/// （PROPERTIES/MAX_DAQ(WORD)/MAX_EVENT_CHANNEL(WORD)/MIN_DAQ/DAQ_KEY_BYTE）
constexpr std::size_t kGetDaqProcessorInfoResMinSize = 7;
/// @brief READ_DAQ 响应去掉 PID 后的最小长度
/// （BITOFFSET/SIZE/EXT/ADDR(DWORD)）
constexpr std::size_t kReadDaqResMinSize = 7;

/// @brief COMM_MODE_BASIC 位掩码
constexpr std::uint8_t kByteOrderMask = 0x01U;
constexpr std::uint8_t kAddressGranularityShift = 1U;
constexpr std::uint8_t kSlaveBlockModeMask = 0x40U;
constexpr std::uint8_t kOptionalMask = 0x80U;

/// @brief Current Session Status 位掩码（GET_STATUS Byte 1）
/// @note 位序依据 ASAM XCP V1.5.0 原始 PDF 第 116 页 Table 49
///       （位号行 Bit7..Bit0 与位名行逐列对齐），并由第 117 页 Table 50
///       的编码表升序排列互为佐证。修正前 kStoreDaqReqMask 误用 bit1，
///       而 bit1 实为 CAL_PAG_CFG_LOST，导致 STORE_DAQ_REQ 误报/漏报。
constexpr std::uint8_t kStoreCalReqMask = 0x01U;
constexpr std::uint8_t kCalPagCfgLostMask = 0x02U;
constexpr std::uint8_t kStoreDaqReqMask = 0x04U;
constexpr std::uint8_t kClearDaqReqMask = 0x08U;
constexpr std::uint8_t kDaqCfgLostMask = 0x10U;
constexpr std::uint8_t kDaqRunningMask = 0x40U;
constexpr std::uint8_t kResumeMask = 0x80U;

}  // namespace

ResponseParser::ResponseParser(ByteOrder byte_order,
                              AddressGranularity ag) noexcept
    : m_byte_order_(byte_order), m_ag_(ag) {}

ByteOrder ResponseParser::GetByteOrder() const noexcept {
    return m_byte_order_;
}

AddressGranularity ResponseParser::GetAddressGranularity() const noexcept {
    return m_ag_;
}

std::optional<std::uint8_t> ResponseParser::ReadU8(
    BytesView data, std::size_t offset) noexcept {
    if (offset >= data.size()) {
        return std::nullopt;
    }
    return data[offset];
}

std::optional<std::uint16_t> ResponseParser::ReadU16(BytesView data,
                                                     std::size_t offset) const {
    const auto lo = ReadU8(data, offset);
    const auto hi = ReadU8(data, offset + 1);
    if (!lo || !hi) {
        return std::nullopt;
    }
    if (m_byte_order_ == ByteOrder::Intel) {
        return static_cast<std::uint16_t>(
            *lo | (static_cast<std::uint16_t>(*hi) << 8));
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(*lo) << 8) |
                                      *hi);
}

std::optional<std::uint32_t> ResponseParser::ReadU32(BytesView data,
                                                     std::size_t offset) const {
    // 逐字节读，越界即 nullopt（畸形包判错，绝不越界读）
    const auto b0 = ReadU8(data, offset);
    const auto b1 = ReadU8(data, offset + 1);
    const auto b2 = ReadU8(data, offset + 2);
    const auto b3 = ReadU8(data, offset + 3);
    if (!b0 || !b1 || !b2 || !b3) {
        return std::nullopt;
    }
    if (m_byte_order_ == ByteOrder::Intel) {
        return static_cast<std::uint32_t>(*b0) |
               (static_cast<std::uint32_t>(*b1) << 8) |
               (static_cast<std::uint32_t>(*b2) << 16) |
               (static_cast<std::uint32_t>(*b3) << 24);
    }
    return (static_cast<std::uint32_t>(*b0) << 24) |
           (static_cast<std::uint32_t>(*b1) << 16) |
           (static_cast<std::uint32_t>(*b2) << 8) |
           static_cast<std::uint32_t>(*b3);
}

std::optional<std::uint64_t> ResponseParser::ReadU64(BytesView data,
                                                     std::size_t offset) const {
    // DLONG 时钟时间戳：逐字节拼装，越界即 nullopt
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto byte = ReadU8(data, offset + i);
        if (!byte) {
            return std::nullopt;
        }
        const std::size_t shift =
            m_byte_order_ == ByteOrder::Intel ? i : (7 - i);
        value |= static_cast<std::uint64_t>(*byte) << (shift * 8);
    }
    return value;
}

std::optional<ParsedPacket> ResponseParser::Parse(
    BytesView packet, CommandCode expected_command) const {
    if (packet.empty()) {
        // 空 Packet 无法分类，视为畸形
        return std::nullopt;
    }

    const std::uint8_t pid = packet[0];
    const BytesView body = packet.subspan(1);

    switch (pid) {
        case static_cast<std::uint8_t>(PacketType::Res): {
            PositiveResponse res;
            res.command = expected_command;
            res.data.assign(body.begin(), body.end());
            return ParsedPacket{res};
        }
        case static_cast<std::uint8_t>(PacketType::Err): {
            // ERR 至少要有 Byte 1 的错误码，否则视为畸形
            if (body.empty()) {
                return std::nullopt;
            }
            NegativeResponse err;
            err.raw_error_code = body[0];
            err.error_code = ToErrorCode(body[0]);
            const BytesView extra = body.subspan(1);
            err.additional_info.assign(extra.begin(), extra.end());
            return ParsedPacket{err};
        }
        case static_cast<std::uint8_t>(PacketType::Ev): {
            if (body.empty()) {
                return std::nullopt;
            }
            EventPacket ev;
            ev.raw_event_code = body[0];
            ev.event_code = ToEventCode(body[0]);
            const BytesView extra = body.subspan(1);
            ev.info.assign(extra.begin(), extra.end());
            return ParsedPacket{ev};
        }
        case static_cast<std::uint8_t>(PacketType::Serv): {
            if (body.empty()) {
                return std::nullopt;
            }
            ServicePacket serv;
            serv.service_code = body[0];
            const BytesView extra = body.subspan(1);
            serv.data.assign(extra.begin(), extra.end());
            return ParsedPacket{serv};
        }
        default: {
            // 0x00..0xFB：DAQ DTO。批次14（R12 定稿）交付**完整帧**——
            // data[0] 仍是 PID，与 pid 字段等值。下游 IDaqLayout::Decode 按
            // dto[0] 取 EPK，按其 envelope 契约跳过可变头后才是净荷；
            // 这里若剥掉 PID，就会把净荷首字节误当 PID（批次11 已警告）。
            DtoPacket dto;
            dto.pid = pid;
            dto.data.assign(packet.begin(), packet.end());
            return ParsedPacket{dto};
        }
    }
}

std::optional<ConnectResponse> ResponseParser::ParseConnectResponse(
    BytesView res_data) const {
    if (res_data.size() < kConnectResMinSize) {
        return std::nullopt;
    }

    ConnectResponse resp;
    resp.resource_mask = res_data[0];

    const std::uint8_t comm_mode_basic = res_data[1];
    resp.byte_order = ((comm_mode_basic & kByteOrderMask) != 0U)
                          ? ByteOrder::Motorola
                          : ByteOrder::Intel;

    const auto ag_field = static_cast<std::uint8_t>(
        (comm_mode_basic >> kAddressGranularityShift) & 0x03U);
    const auto ag = CommModeBasicToAg(ag_field);
    if (!ag) {
        // AG 位域为保留值 11：非法协商结果
        return std::nullopt;
    }
    resp.address_granularity = *ag;
    resp.slave_block_mode_supported =
        (comm_mode_basic & kSlaveBlockModeMask) != 0U;
    resp.optional_comm_mode_available = (comm_mode_basic & kOptionalMask) != 0U;

    resp.max_cto = res_data[2];
    const auto max_dto = ReadU16(res_data, 3);
    if (!max_dto) {
        return std::nullopt;
    }
    resp.max_dto = *max_dto;
    resp.protocol_layer_version = res_data[5];
    resp.transport_layer_version = res_data[6];
    return resp;
}

std::optional<GetStatusResponse> ResponseParser::ParseGetStatusResponse(
    BytesView res_data) const {
    if (res_data.size() < kGetStatusResMinSize) {
        return std::nullopt;
    }

    GetStatusResponse resp;
    const std::uint8_t session_status = res_data[0];
    resp.resume = (session_status & kResumeMask) != 0U;
    resp.daq_running = (session_status & kDaqRunningMask) != 0U;
    resp.clear_daq_req = (session_status & kClearDaqReqMask) != 0U;
    resp.store_daq_req = (session_status & kStoreDaqReqMask) != 0U;
    resp.store_cal_req = (session_status & kStoreCalReqMask) != 0U;
    // DAQ_CFG_LOST(bit4) 与 CAL_PAG_CFG_LOST(bit1) 当前未纳入 GetStatusResponse，
    // 仅在此处据原始 PDF 第 116 页 Table 49 登记掩码，避免后续实现再次误用位号。
    static_cast<void>(kDaqCfgLostMask);
    static_cast<void>(kCalPagCfgLostMask);
    resp.resource_protection = res_data[1];
    resp.state_number = res_data[2];
    const auto config_id = ReadU16(res_data, 3);
    if (!config_id) {
        return std::nullopt;
    }
    resp.session_config_id = *config_id;
    return resp;
}

std::optional<GetCommModeInfoResponse>
ResponseParser::ParseGetCommModeInfoResponse(BytesView res_data) const {
    if (res_data.size() < kGetCommModeInfoResMinSize) {
        return std::nullopt;
    }

    GetCommModeInfoResponse resp;
    resp.comm_mode_optional = res_data[1];
    resp.max_bs = res_data[3];
    resp.min_st = res_data[4];
    resp.queue_size = res_data[5];
    const std::uint8_t driver_version = res_data[6];
    resp.driver_version_major =
        static_cast<std::uint8_t>((driver_version >> 4) & 0x0FU);
    resp.driver_version_minor =
        static_cast<std::uint8_t>(driver_version & 0x0FU);
    return resp;
}

std::optional<GetVersionResponse> ResponseParser::ParseGetVersionResponse(
    BytesView res_data) const {
    // GET_VERSION RES（原始 PDF 第 114 页 Table 45，剥 0xFF 后索引 -1）：
    // [Reserved@0][Major PL@1][Minor PL@2][Major TL@3][Minor TL@4]，共 5 字节。
    // 全为单字节字段，故不使用 m_byte_order_（字节序对本响应无影响）。
    if (res_data.size() < 5U) {
        return std::nullopt;
    }
    GetVersionResponse resp;
    resp.reserved = res_data[0];
    resp.protocol_major = res_data[1];
    resp.protocol_minor = res_data[2];
    resp.transport_major = res_data[3];
    resp.transport_minor = res_data[4];
    return resp;
}

std::optional<GetSeedResponse> ResponseParser::ParseGetSeedResponse(
    BytesView res_data) const {
    if (res_data.size() < kSeedKeyResMinSize) {
        return std::nullopt;
    }
    GetSeedResponse resp;
    // Length 字段保留原值：First 帧=Seed 总长、Remainder 帧=剩余长、
    // 0=资源未保护（语义由编排层按已发 Mode 解读）
    resp.length = res_data[0];
    const BytesView seed = res_data.subspan(1);
    resp.seed.assign(seed.begin(), seed.end());
    return resp;
}

std::optional<UnlockResponse> ResponseParser::ParseUnlockResponse(
    BytesView res_data) const {
    if (res_data.size() < kSeedKeyResMinSize) {
        return std::nullopt;
    }
    UnlockResponse resp;
    resp.resource_protection = res_data[0];
    return resp;
}

std::optional<StartStopDaqListResponse>
ResponseParser::ParseStartStopDaqListResponse(BytesView res_data) const {
    // START_STOP_DAQ_LIST RES: [FF][FIRST_PID]（xcp.h:734-735）
    // FIRST_PID 只在 Start/Select 成功后有意义；Absolute ODT Number 模式下
    // 绝对 ODT 号 = FIRST_PID + 相对 ODT 号（docs L2222-2226）
    if (res_data.size() < kStartStopDaqListResMinSize) {
        return std::nullopt;
    }
    StartStopDaqListResponse resp;
    resp.first_pid = res_data[0];
    return resp;
}

std::optional<GetDaqListInfoResponse> ResponseParser::ParseGetDaqListInfo(
    BytesView res_data) const {
    // GET_DAQ_LIST_INFO RES: [FF][PROPERTIES][MAX_ODT][MAX_ODT_ENTRY]
    //                        [FIXED_EVENT(WORD)]（xcp.h:810-814）
    // 注意：**没有 FIRST_PID**（docs L2452-2457 的返回字段只有这四项）
    if (res_data.size() < kGetDaqListInfoResMinSize) {
        return std::nullopt;
    }
    GetDaqListInfoResponse resp;
    resp.properties = static_cast<DaqListPropertyBit>(res_data[0]);
    resp.max_odt = res_data[1];
    resp.max_odt_entries = res_data[2];
    const auto fixed_event = ReadU16(res_data, 3);
    if (!fixed_event) {
        return std::nullopt;
    }
    resp.fixed_event = *fixed_event;
    return resp;
}

std::optional<DaqPackedModeResponse> ResponseParser::ParseDaqPackedMode(
    BytesView res_data) const {
    // 结构来源：原始 PDF Table 128（mode=0）/ Table 129（mode=1/2）。
    // 两种载荷长度不同，按 mode 分支，不做长度猜测。
    if (res_data.size() < kDaqPackedModeResMinSize) {
        return std::nullopt;
    }
    const auto mode = ToDaqPackedMode(res_data[1]);
    if (!mode) {
        return std::nullopt;
    }

    DaqPackedModeResponse resp;
    resp.mode = *mode;
    if (resp.mode == DaqPackedMode::NotPacked) {
        return resp;
    }

    if (res_data.size() < kDaqPackedModeResPackedSize) {
        return std::nullopt;
    }
    const auto ts_mode = ToDaqPackedTimestampMode(res_data[2]);
    if (!ts_mode) {
        return std::nullopt;
    }
    resp.timestamp_mode = *ts_mode;
    const auto sample_count = ReadU16(res_data, 3);
    if (!sample_count.has_value()) {
        return std::nullopt;
    }
    resp.sample_count = *sample_count;
    return resp;
}

std::optional<GetDaqListModeResponse> ResponseParser::ParseGetDaqListMode(
    BytesView res_data) const {
    // GET_DAQ_LIST_MODE RES: [FF][Current Mode][reserved(WORD)]
    //   [Event Channel(WORD)][Prescaler][Priority]（规范 Table 150）
    // 注意：`mode` 是 Slave 当前状态位（含 SELECTED/RUNNING/RESUME），
    //       与 SET_DAQ_LIST_MODE 请求的 MODE 位语义不同，调用方不得原样回填。
    if (res_data.size() < kGetDaqListModeResMinSize) {
        return std::nullopt;
    }
    GetDaqListModeResponse resp;
    resp.mode = res_data[0];
    const auto reserved = ReadU16(res_data, 1);
    const auto event_channel = ReadU16(res_data, 3);
    if (!reserved || !event_channel) {
        return std::nullopt;
    }
    resp.reserved = *reserved;
    resp.event_channel = *event_channel;
    resp.prescaler = res_data[5];
    resp.priority = res_data[6];
    return resp;
}

std::optional<BuildChecksumResponse> ResponseParser::ParseBuildChecksum(
    BytesView res_data) const {
    // BUILD_CHECKSUM RES: [FF][Checksum type][Reserved(WORD)][Checksum(DWORD)]
    // （原始 PDF 第 148 页 Table 74）；校验结果恒为 DWORD。
    if (res_data.size() < kBuildChecksumResMinSize) {
        return std::nullopt;
    }
    BuildChecksumResponse resp;
    resp.raw_checksum_type = res_data[0];
    // 未知类型不判失败：保留原字节，checksum_type 留空，上层按“无法验证”处理
    resp.checksum_type = ToChecksumType(resp.raw_checksum_type);
    const auto reserved = ReadU16(res_data, 1);
    const auto checksum = ReadU32(res_data, 3);
    if (!reserved || !checksum) {
        return std::nullopt;
    }
    resp.reserved = *reserved;
    resp.checksum = *checksum;
    return resp;
}

std::optional<DtoCtrPropertiesResponse> ResponseParser::ParseDtoCtrProperties(
    BytesView res_data) const {
    // DTO_CTR_PROPERTIES RES: [FF][PROPERTIES][Related Event(WORD)][MODE]
    // （docs §7.5.4.15 Table 164）；被查询的事件通道号不在响应中回显。
    if (res_data.size() < kDtoCtrPropertiesResMinSize) {
        return std::nullopt;
    }
    DtoCtrPropertiesResponse resp;
    resp.properties = res_data[0];
    const auto related = ReadU16(res_data, 1);
    if (!related) {
        return std::nullopt;
    }
    resp.related_event_channel = *related;
    resp.mode = res_data[3];
    return resp;
}

std::optional<GetDaqClockResponse> ResponseParser::ParseGetDaqClock(
    BytesView res_data, bool extended) const {
    // GET_DAQ_CLOCK RES（去掉 0xFF）：
    //   legacy  Table 133: [reserved][TRIGGER_INFO][PAYLOAD_FMT][DWORD]
    //   extended Table 134: [reserved][TRIGGER_INFO][PAYLOAD_FMT]
    //                       [slave ts][opt grandm ts][opt ecu ts][opt SYNC_STATE]
    if (res_data.size() < kGetDaqClockResMinSize) {
        return std::nullopt;
    }
    GetDaqClockResponse resp;
    resp.extended = extended;
    resp.trigger_info = res_data[1];
    resp.payload_fmt = res_data[2];
    constexpr std::size_t kHeaderBytes = 3;  // reserved + TRIGGER_INFO + PAYLOAD_FMT
    std::size_t offset = kHeaderBytes;

    if (!extended) {
        const auto value = ReadU32(res_data, offset);
        if (!value) {
            return std::nullopt;
        }
        resp.slave_clock = *value;
        resp.slave_clock_bytes = 4;
        return resp;
    }

    // extended：slave 时钟按 FMT_XCP_SLV 的宽度读取
    const auto slave_fmt = XcpSlaveClockFormatOf(resp.payload_fmt);
    if (!slave_fmt || *slave_fmt == ClockPayloadFormat::Absent) {
        // 保留值 3，或 extended 却没给 slave 时钟：都无法解释布局
        return std::nullopt;
    }
    if (*slave_fmt == ClockPayloadFormat::Dword) {
        const auto value = ReadU32(res_data, offset);
        if (!value) {
            return std::nullopt;
        }
        resp.slave_clock = *value;
        resp.slave_clock_bytes = 4;
        offset += 4;
    } else {
        const auto value = ReadU64(res_data, offset);
        if (!value) {
            return std::nullopt;
        }
        resp.slave_clock = *value;
        resp.slave_clock_bytes = 8;
        offset += 8;
    }

    // grandmaster：格式为 Absent 表示不在载荷中
    const auto grandm_fmt = GrandmasterClockFormatOf(resp.payload_fmt);
    if (!grandm_fmt) {
        return std::nullopt;
    }
    if (*grandm_fmt != ClockPayloadFormat::Absent) {
        if (*grandm_fmt == ClockPayloadFormat::Dword) {
            const auto value = ReadU32(res_data, offset);
            if (!value) {
                return std::nullopt;
            }
            resp.grandmaster_clock = *value;
            offset += 4;
        } else {
            const auto value = ReadU64(res_data, offset);
            if (!value) {
                return std::nullopt;
            }
            resp.grandmaster_clock = *value;
            offset += 8;
        }
    }

    const auto ecu_fmt = EcuClockFormatOf(resp.payload_fmt);
    if (!ecu_fmt) {
        return std::nullopt;
    }
    if (*ecu_fmt != ClockPayloadFormat::Absent) {
        if (*ecu_fmt == ClockPayloadFormat::Dword) {
            const auto value = ReadU32(res_data, offset);
            if (!value) {
                return std::nullopt;
            }
            resp.ecu_clock = *value;
            offset += 4;
        } else {
            const auto value = ReadU64(res_data, offset);
            if (!value) {
                return std::nullopt;
            }
            resp.ecu_clock = *value;
            offset += 8;
        }
    }

    // SYNC_STATE：只要还有字节就是它；没有则保持 nullopt，
    // 由上层按“规范要求有可同步时钟就必须带”自行判定是否可疑。
    if (offset < res_data.size()) {
        resp.sync_state = res_data[offset];
    }
    return resp;
}

std::optional<TimeCorrelationPropertiesResponse>
ResponseParser::ParseTimeCorrelationProperties(BytesView res_data) const {
    // TIME_CORRELATION_PROPERTIES RES（去掉 0xFF）：[SLAVE_CONFIG]
    //   [OBSERVABLE_CLOCKS][SYNC_STATE][CLOCK_INFO][RESERVED][CLUSTER_ID(WORD)]
    // （docs §7.5.6.1 Table 212，剥掉 PID 后固定 8 字节）
    if (res_data.size() < kTimeCorrelationResMinSize) {
        return std::nullopt;
    }
    TimeCorrelationPropertiesResponse resp;
    resp.slave_config = res_data[0];
    resp.observable_clocks = res_data[1];
    resp.sync_state = res_data[2];
    resp.clock_info = res_data[3];
    resp.reserved = res_data[4];
    const auto cluster = ReadU16(res_data, 5);
    if (!cluster) {
        return std::nullopt;
    }
    resp.cluster_id = *cluster;
    return resp;
}

std::optional<TimeSyncEvent> ResponseParser::ParseTimeSyncEvent(
    BytesView res_data, TimeSyncLayout layout) const {
    TimeSyncEvent event;
    if (layout == TimeSyncLayout::Legacy) {
        // Table 242: [reserved][reserved][DWORD]，没有 TRIGGER_INFO
        if (res_data.size() < kTimeSyncEventLegacyMinSize) {
            return std::nullopt;
        }
        const auto value = ReadU32(res_data, 2);
        if (!value) {
            return std::nullopt;
        }
        event.extended = false;
        event.slave_clock = *value;
        return event;
    }

    // 两种 extended 都需要至少 [TRIGGER_INFO][第二字节][DWORD]
    if (res_data.size() < kTimeSyncEventLegacyMinSize) {
        return std::nullopt;
    }
    event.extended = true;
    event.trigger_info = res_data[0];

    if (layout == TimeSyncLayout::ExtendedMaxCto8) {
        // Table 247: [TRIGGER_INFO][counter 或 0][DWORD]，无 PAYLOAD_FMT
        const auto value = ReadU32(res_data, 2);
        if (!value) {
            return std::nullopt;
        }
        event.slave_clock = *value;
        if (res_data[1] != 0U) {
            event.counter = res_data[1];
        }
        return event;
    }

    // Table 248: [TRIGGER_INFO][PAYLOAD_FMT][slave ts][opt grandm][opt ecu]
    //            [opt CLUSTER_ID + counter][opt SYNC_STATE]
    event.payload_fmt = res_data[1];
    const auto slave_fmt = XcpSlaveClockFormatOf(event.payload_fmt);
    if (!slave_fmt || *slave_fmt == ClockPayloadFormat::Absent) {
        return std::nullopt;
    }
    std::size_t offset = 2;
    if (*slave_fmt == ClockPayloadFormat::Dword) {
        const auto value = ReadU32(res_data, offset);
        if (!value) {
            return std::nullopt;
        }
        event.slave_clock = *value;
        offset += 4;
    } else {
        const auto value = ReadU64(res_data, offset);
        if (!value) {
            return std::nullopt;
        }
        event.slave_clock = *value;
        offset += 8;
    }

    const auto grandm_fmt = GrandmasterClockFormatOf(event.payload_fmt);
    if (!grandm_fmt) {
        return std::nullopt;
    }
    if (*grandm_fmt != ClockPayloadFormat::Absent) {
        if (*grandm_fmt == ClockPayloadFormat::Dword) {
            const auto value = ReadU32(res_data, offset);
            if (!value) {
                return std::nullopt;
            }
            event.grandmaster_clock = *value;
            offset += 4;
        } else {
            const auto value = ReadU64(res_data, offset);
            if (!value) {
                return std::nullopt;
            }
            event.grandmaster_clock = *value;
            offset += 8;
        }
    }

    const auto ecu_fmt = EcuClockFormatOf(event.payload_fmt);
    if (!ecu_fmt) {
        return std::nullopt;
    }
    if (*ecu_fmt != ClockPayloadFormat::Absent) {
        if (*ecu_fmt == ClockPayloadFormat::Dword) {
            const auto value = ReadU32(res_data, offset);
            if (!value) {
                return std::nullopt;
            }
            event.ecu_clock = *value;
            offset += 4;
        } else {
            const auto value = ReadU64(res_data, offset);
            if (!value) {
                return std::nullopt;
            }
            event.ecu_clock = *value;
            offset += 8;
        }
    }

    if (HasClusterIdentifier(event.payload_fmt)) {
        const auto cluster = ReadU16(res_data, offset);
        if (!cluster) {
            return std::nullopt;
        }
        event.cluster_id = *cluster;
        offset += 2;
        const auto counter = ReadU8(res_data, offset);
        if (!counter) {
            return std::nullopt;
        }
        event.counter = *counter;
        offset += 1;
    }

    if (offset < res_data.size()) {
        event.sync_state = res_data[offset];
    }
    return event;
}

std::optional<GetDaqEventInfoResponse> ResponseParser::ParseGetDaqEventInfo(
    BytesView res_data) const {
    // GET_DAQ_EVENT_INFO RES（去掉 0xFF）：[PROPERTIES][MAX_DAQ_LISTS]
    //   [NAME_LENGTH][TIME_CYCLE][TIME_UNIT][PRIORITY]（XCPlite 实然
    //   xcp.h:819-825，CRM_LEN=7 含 0xFF → 数据 6 字节）。
    // 注意：事件通道号**不在响应中回显**（调用方持有入参 event_channel）。
    if (res_data.size() < kGetDaqEventInfoResMinSize) {
        return std::nullopt;
    }
    GetDaqEventInfoResponse resp;
    resp.properties = res_data[0];
    resp.max_daq_lists = res_data[1];
    resp.name_length = res_data[2];
    resp.time_cycle = res_data[3];
    resp.time_unit = res_data[4];
    resp.priority = res_data[5];
    return resp;
}

std::optional<GetDaqProcessorInfoResponse>
ResponseParser::ParseGetDaqProcessorInfo(BytesView res_data) const {
    // GET_DAQ_PROCESSOR_INFO RES: [FF][PROPERTIES][MAX_DAQ(WORD)]
    //   [MAX_EVENT_CHANNEL(WORD)][MIN_DAQ][DAQ_KEY_BYTE]（xcp.h:790-795）
    if (res_data.size() < kGetDaqProcessorInfoResMinSize) {
        return std::nullopt;
    }
    GetDaqProcessorInfoResponse resp;
    resp.properties = res_data[0];
    const auto max_daq = ReadU16(res_data, 1);
    const auto max_event = ReadU16(res_data, 3);
    if (!max_daq || !max_event) {
        return std::nullopt;
    }
    resp.max_daq = *max_daq;
    resp.max_event_channel = *max_event;
    resp.min_daq = res_data[5];
    // DAQ_KEY_BYTE 只拆位（optimisation / address_extension / identification）
    resp.key_byte = ParseDaqKeyByte(res_data[6]);
    return resp;
}

std::optional<GetDaqResolutionInfoResponse>
ResponseParser::ParseGetDaqResolutionInfo(BytesView res_data) const {
    // GET_DAQ_RESOLUTION_INFO RES: [FF][GRANULARITY_DAQ][MAX_SIZE_DAQ]
    //   [GRANULARITY_STIM][MAX_SIZE_STIM][TIMESTAMP_MODE][TICKS(WORD)]
    //   （xcp.h:799-805；docs L2346-2356）
    if (res_data.size() < kGetDaqResolutionInfoResMinSize) {
        return std::nullopt;
    }
    GetDaqResolutionInfoResponse resp;
    resp.granularity_daq = res_data[0];
    resp.max_odt_entry_size_daq = res_data[1];
    resp.granularity_stim = res_data[2];
    resp.max_odt_entry_size_stim = res_data[3];
    // TIMESTAMP_MODE 只拆位不换算（时间单位码表缺失，R13 外部阻塞）
    resp.timestamp_mode = ParseDaqTimestampMode(res_data[4]);
    const auto ticks = ReadU16(res_data, 5);
    if (!ticks) {
        return std::nullopt;
    }
    resp.timestamp_ticks = *ticks;
    return resp;
}

std::optional<ReadDaqResponse> ResponseParser::ParseReadDaq(
    BytesView res_data) const {
    // READ_DAQ RES: [FF][BITOFFSET][SIZE][EXT][ADDR(DWORD)]
    // （xcp.h:782-786；docs L2257-2261）
    if (res_data.size() < kReadDaqResMinSize) {
        return std::nullopt;
    }
    ReadDaqResponse resp;
    resp.bit_offset = res_data[0];  // 0xFF = 无位偏（kDaqBitOffsetNone）
    resp.size = res_data[1];
    resp.address_extension = res_data[2];
    const auto address = ReadU32(res_data, 3);
    if (!address) {
        return std::nullopt;
    }
    resp.address = *address;
    return resp;
}

std::optional<GetIdResponse> ResponseParser::ParseGetId(
    BytesView res_data) const {
    // GET_ID RES（原始 PDF 第 123 页 Table 60，剥 0xFF 后索引 -1）：
    // [MODE@0][Reserved WORD@1..2][LENGTH DWORD@3..6][DATA@7..]
    // 说明：LENGTH 为 DWORD 与实现一致，审计所称「XCPlite 方言」不成立。
    if (res_data.size() < 7U) {
        return std::nullopt;
    }
    GetIdResponse resp;
    resp.raw_mode = res_data[0];
    // F-03b：Table 61 位掩码——bit 0 = TRANSFER_MODE，bit 1 =
    // COMPRESSED_ENCRYPTED，bit 7..2 = 保留（x）。必须拆位而非整字节比较，
    // 否则 Mode=0x02/0x03（置了压缩位）会被误当作普通 UPLOAD 通路。
    resp.transfer_mode = static_cast<std::uint8_t>(resp.raw_mode & 0x01U);
    resp.compressed_encrypted = (resp.raw_mode & 0x02U) != 0U;
    resp.reserved_bits_clear = (resp.raw_mode & 0xFCU) == 0U;
    const auto length = ReadU32(res_data, 3);
    if (!length) {
        return std::nullopt;
    }
    resp.length = *length;
    // F-03c：Table 60 下方原文「The following rule applies: Length mod AG = 0」。
    // 只记录判定结果，不在解析层抛错——由调用方按上下文决定拒绝方式。
    const std::uint32_t ag_bytes =
        static_cast<std::uint32_t>(AgToBytes(m_ag_));
    resp.length_aligned_to_ag = ag_bytes != 0U && (resp.length % ag_bytes) == 0U;
    if (res_data.size() > 7U) {
        const std::size_t avail = res_data.size() - 7U;
        const std::size_t take =
            avail < resp.length ? avail : static_cast<std::size_t>(resp.length);
        resp.identification_data.assign(
            res_data.begin() + 7,
            res_data.begin() + 7 + static_cast<std::ptrdiff_t>(take));
    }
    return resp;
}

std::optional<GetCalPageResponse> ResponseParser::ParseGetCalPage(
    BytesView res_data) const {
    // GET_CAL_PAGE RES: [FF][reserved][reserved][PAGE_NUMBER]
    // （xcp.h CRM_GET_CAL_PAGE_LEN=4；docs §7.5.3.2 返回逻辑 Page Number）
    if (res_data.size() < 3U) {
        return std::nullopt;
    }
    GetCalPageResponse resp;
    const auto page = ReadU8(res_data, 2);
    if (!page) {
        return std::nullopt;
    }
    resp.page = *page;
    return resp;
}

std::optional<GetPagProcessorInfoResponse>
ResponseParser::ParseGetPagProcessorInfo(BytesView res_data) const {
    // GET_PAG_PROCESSOR_INFO RES: [FF][MAX_SEGMENT][PAG_PROPERTIES]
    // （xcp.h CRM_GET_PAG_PROCESSOR_INFO_LEN=3；docs §7.5.3.3）
    if (res_data.size() < 2U) {
        return std::nullopt;
    }
    GetPagProcessorInfoResponse resp;
    resp.max_segment = res_data[0];
    resp.properties = static_cast<PagPropertyBit>(res_data[1]);
    return resp;
}

std::optional<GetSegmentInfoResponse> ResponseParser::ParseGetSegmentInfo(
    BytesView res_data, SegmentInfoMode mode) const {
    // GET_SEGMENT_INFO RES（docs §7.5.3.4，变长按 Mode 分支）：
    //   Mode 0/2: [DWORD@0..3]（xcp.h CRM_GET_SEGMENT_INFO_MODE0_LEN=8 →
    //             剥 FF 后数据 4 字节 DWORD，剩余填充字节忽略）
    //   Mode 1:   [MAX_PAGES][ADDRESS_EXTENSION][MAX_MAPPING]
    //             [COMPRESSION][ENCRYPTION]（xcp.h LEN=6 → 数据 5 字节）
    GetSegmentInfoResponse resp;
    resp.mode = mode;
    switch (mode) {
        case SegmentInfoMode::BasicInfo:
        case SegmentInfoMode::MappingInfo: {
            if (res_data.size() < 4U) {
                return std::nullopt;
            }
            const auto value = ReadU32(res_data, 0);
            if (!value) {
                return std::nullopt;
            }
            SegmentBasicInfo basic;
            basic.value = *value;
            if (mode == SegmentInfoMode::MappingInfo) {
                SegmentMappingInfo mapping;
                mapping.value = *value;
                resp.data = mapping;
            } else {
                resp.data = basic;
            }
            return resp;
        }
        case SegmentInfoMode::StandardProperties: {
            if (res_data.size() < 5U) {
                return std::nullopt;
            }
            SegmentStandardProperties props;
            props.max_pages = res_data[0];
            props.address_extension = res_data[1];
            props.max_mapping = res_data[2];
            props.compression_method = res_data[3];
            props.encryption_method = res_data[4];
            resp.data = props;
            return resp;
        }
    }
    // 请求 Mode 非法（编码层已拦截 >2；此处防御性拒绝）
    return std::nullopt;
}

std::optional<GetPageInfoResponse> ResponseParser::ParseGetPageInfo(
    BytesView res_data) const {
    // GET_PAGE_INFO RES: [FF][PAGE_PROPERTIES][INIT_SEGMENT]
    // （xcp.h CRM_GET_PAGE_INFO_LEN=3；docs §7.5.3.5）
    if (res_data.size() < 2U) {
        return std::nullopt;
    }
    GetPageInfoResponse resp;
    resp.properties = ParsePageProperties(res_data[0]);
    resp.init_segment = res_data[1];
    return resp;
}

std::optional<GetSegmentModeResponse> ResponseParser::ParseGetSegmentMode(
    BytesView res_data) const {
    // GET_SEGMENT_MODE RES: [FF][reserved][MODE]
    // （xcp.h CRM_GET_SEGMENT_MODE_LEN=3；docs §7.5.3.7）
    if (res_data.size() < 2U) {
        return std::nullopt;
    }
    GetSegmentModeResponse resp;
    resp.mode = static_cast<SegmentModeBit>(res_data[1]);
    return resp;
}

}  // namespace calmcar::xcp
