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
            res.m_command_ = expected_command;
            res.m_data_.assign(body.begin(), body.end());
            return ParsedPacket{res};
        }
        case static_cast<std::uint8_t>(PacketType::Err): {
            // ERR 至少要有 Byte 1 的错误码，否则视为畸形
            if (body.empty()) {
                return std::nullopt;
            }
            NegativeResponse err;
            err.m_raw_error_code_ = body[0];
            err.m_error_code_ = ToErrorCode(body[0]);
            const BytesView extra = body.subspan(1);
            err.m_additional_info_.assign(extra.begin(), extra.end());
            return ParsedPacket{err};
        }
        case static_cast<std::uint8_t>(PacketType::Ev): {
            if (body.empty()) {
                return std::nullopt;
            }
            EventPacket ev;
            ev.m_raw_event_code_ = body[0];
            ev.m_event_code_ = ToEventCode(body[0]);
            const BytesView extra = body.subspan(1);
            ev.m_info_.assign(extra.begin(), extra.end());
            return ParsedPacket{ev};
        }
        case static_cast<std::uint8_t>(PacketType::Serv): {
            if (body.empty()) {
                return std::nullopt;
            }
            ServicePacket serv;
            serv.m_service_code_ = body[0];
            const BytesView extra = body.subspan(1);
            serv.m_data_.assign(extra.begin(), extra.end());
            return ParsedPacket{serv};
        }
        default: {
            // 0x00..0xFB：DAQ DTO，本阶段仅识别不解析
            DtoPacket dto;
            dto.m_pid_ = pid;
            dto.m_data_.assign(body.begin(), body.end());
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
    resp.m_resource_mask_ = res_data[0];

    const std::uint8_t comm_mode_basic = res_data[1];
    resp.m_byte_order_ = ((comm_mode_basic & kByteOrderMask) != 0U)
                             ? ByteOrder::Motorola
                             : ByteOrder::Intel;

    const auto ag_field = static_cast<std::uint8_t>(
        (comm_mode_basic >> kAddressGranularityShift) & 0x03U);
    const auto ag = CommModeBasicToAg(ag_field);
    if (!ag) {
        // AG 位域为保留值 11：非法协商结果
        return std::nullopt;
    }
    resp.m_address_granularity_ = *ag;
    resp.m_slave_block_mode_supported_ =
        (comm_mode_basic & kSlaveBlockModeMask) != 0U;
    resp.m_optional_comm_mode_available_ =
        (comm_mode_basic & kOptionalMask) != 0U;

    resp.m_max_cto_ = res_data[2];
    const auto max_dto = ReadU16(res_data, 3);
    if (!max_dto) {
        return std::nullopt;
    }
    resp.m_max_dto_ = *max_dto;
    resp.m_protocol_layer_version_ = res_data[5];
    resp.m_transport_layer_version_ = res_data[6];
    return resp;
}

std::optional<GetStatusResponse> ResponseParser::ParseGetStatusResponse(
    BytesView res_data) const {
    if (res_data.size() < kGetStatusResMinSize) {
        return std::nullopt;
    }

    GetStatusResponse resp;
    const std::uint8_t session_status = res_data[0];
    resp.m_resume_ = (session_status & kResumeMask) != 0U;
    resp.m_daq_running_ = (session_status & kDaqRunningMask) != 0U;
    resp.m_clear_daq_req_ = (session_status & kClearDaqReqMask) != 0U;
    resp.m_store_daq_req_ = (session_status & kStoreDaqReqMask) != 0U;
    resp.m_store_cal_req_ = (session_status & kStoreCalReqMask) != 0U;
    resp.m_resource_protection_ = res_data[1];
    resp.m_state_number_ = res_data[2];
    const auto config_id = ReadU16(res_data, 3);
    if (!config_id) {
        return std::nullopt;
    }
    resp.m_session_config_id_ = *config_id;
    return resp;
}

std::optional<GetCommModeInfoResponse>
ResponseParser::ParseGetCommModeInfoResponse(BytesView res_data) const {
    if (res_data.size() < kGetCommModeInfoResMinSize) {
        return std::nullopt;
    }

    GetCommModeInfoResponse resp;
    resp.m_comm_mode_optional_ = res_data[1];
    resp.m_max_bs_ = res_data[3];
    resp.m_min_st_ = res_data[4];
    resp.m_queue_size_ = res_data[5];
    const std::uint8_t driver_version = res_data[6];
    resp.m_driver_version_major_ =
        static_cast<std::uint8_t>((driver_version >> 4) & 0x0FU);
    resp.m_driver_version_minor_ =
        static_cast<std::uint8_t>(driver_version & 0x0FU);
    return resp;
}

}  // namespace calmcar::xcp
