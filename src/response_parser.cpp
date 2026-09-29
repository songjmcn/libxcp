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
constexpr std::uint8_t kStoreCalReqMask = 0x01U;
constexpr std::uint8_t kStoreDaqReqMask = 0x02U;
constexpr std::uint8_t kClearDaqReqMask = 0x08U;
constexpr std::uint8_t kDaqRunningMask = 0x40U;
constexpr std::uint8_t kResumeMask = 0x80U;

}  // namespace

ResponseParser::ResponseParser(ByteOrder byte_order) noexcept
    : m_byte_order_(byte_order) {}

ByteOrder ResponseParser::GetByteOrder() const noexcept {
    return m_byte_order_;
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
    // GET_ID RES（XCPlite 实然 xcp.h:521-525）：剥 0xFF 后
    // [MODE@0][reserved×2][LENGTH DWORD@3..6][DATA@7..]；规范 §7.5.1.6
    // 的 LENGTH 为 WORD 且偏移不同——实然优先（批次21 21-2）。
    if (res_data.size() < 7U) {
        return std::nullopt;
    }
    GetIdResponse resp;
    resp.transfer_mode = res_data[0];
    const auto length = ReadU32(res_data, 3);
    if (!length) {
        return std::nullopt;
    }
    resp.length = *length;
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

}  // namespace calmcar::xcp
