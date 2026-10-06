/**
 * @file protocol_types.cpp
 * @brief protocol_types.hpp 中非 constexpr 函数的实现。
 */

#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp {

std::optional<PacketType> ClassifyPacket(std::uint8_t first_byte) noexcept {
    // Slave -> Master PID 空间：0xFC..0xFF 为 SERV/EV/ERR/RES，0x00..0xFB 为
    // DAQ DTO。
    switch (static_cast<PacketType>(first_byte)) {
        case PacketType::Res:
        case PacketType::Err:
        case PacketType::Ev:
        case PacketType::Serv:
            return static_cast<PacketType>(first_byte);
        default:
            // 0x00..0xFB：DAQ DTO，本阶段仅识别不解析
            return std::nullopt;
    }
}

std::string_view ErrorCodeName(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::CmdSynch:
            return "ERR_CMD_SYNCH";
        case ErrorCode::CmdBusy:
            return "ERR_CMD_BUSY";
        case ErrorCode::DaqActive:
            return "ERR_DAQ_ACTIVE";
        case ErrorCode::PgmActive:
            return "ERR_PGM_ACTIVE";
        case ErrorCode::CmdUnknown:
            return "ERR_CMD_UNKNOWN";
        case ErrorCode::CmdSyntax:
            return "ERR_CMD_SYNTAX";
        case ErrorCode::OutOfRange:
            return "ERR_OUT_OF_RANGE";
        case ErrorCode::WriteProtected:
            return "ERR_WRITE_PROTECTED";
        case ErrorCode::AccessDenied:
            return "ERR_ACCESS_DENIED";
        case ErrorCode::AccessLocked:
            return "ERR_ACCESS_LOCKED";
        case ErrorCode::PageNotValid:
            return "ERR_PAGE_NOT_VALID";
        case ErrorCode::ModeNotValid:
            return "ERR_MODE_NOT_VALID";
        case ErrorCode::SegmentNotValid:
            return "ERR_SEGMENT_NOT_VALID";
        case ErrorCode::Sequence:
            return "ERR_SEQUENCE";
        case ErrorCode::DaqConfig:
            return "ERR_DAQ_CONFIG";
        case ErrorCode::MemoryOverflow:
            return "ERR_MEMORY_OVERFLOW";
        case ErrorCode::Generic:
            return "ERR_GENERIC";
        case ErrorCode::Verify:
            return "ERR_VERIFY";
        case ErrorCode::ResourceTemporaryNotAccessible:
            return "ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE";
        case ErrorCode::SubcmdUnknown:
            return "ERR_SUBCMD_UNKNOWN";
    }
    // 未知枚举值（例如 Slave 返回厂商自定义错误码）：保留原值由调用方处理
    return "ERR_UNKNOWN";
}

std::optional<ErrorCode> ToErrorCode(std::uint8_t raw) noexcept {
    switch (static_cast<ErrorCode>(raw)) {
        case ErrorCode::CmdSynch:
        case ErrorCode::CmdBusy:
        case ErrorCode::DaqActive:
        case ErrorCode::PgmActive:
        case ErrorCode::CmdUnknown:
        case ErrorCode::CmdSyntax:
        case ErrorCode::OutOfRange:
        case ErrorCode::WriteProtected:
        case ErrorCode::AccessDenied:
        case ErrorCode::AccessLocked:
        case ErrorCode::PageNotValid:
        case ErrorCode::ModeNotValid:
        case ErrorCode::SegmentNotValid:
        case ErrorCode::Sequence:
        case ErrorCode::DaqConfig:
        case ErrorCode::MemoryOverflow:
        case ErrorCode::Generic:
        case ErrorCode::Verify:
        case ErrorCode::ResourceTemporaryNotAccessible:
        case ErrorCode::SubcmdUnknown:
            return static_cast<ErrorCode>(raw);
    }
    return std::nullopt;
}

std::string_view EventCodeName(EventCode code) noexcept {
    switch (code) {
        case EventCode::ResumeMode:
            return "EV_RESUME_MODE";
        case EventCode::ClearDaq:
            return "EV_CLEAR_DAQ";
        case EventCode::StoreDaq:
            return "EV_STORE_DAQ";
        case EventCode::StoreCal:
            return "EV_STORE_CAL";
        case EventCode::CmdPending:
            return "EV_CMD_PENDING";
        case EventCode::DaqOverload:
            return "EV_DAQ_OVERLOAD";
        case EventCode::SessionTerminated:
            return "EV_SESSION_TERMINATED";
        case EventCode::TimeSync:
            return "EV_TIME_SYNC";
        case EventCode::StimTimeout:
            return "EV_STIM_TIMEOUT";
        case EventCode::Sleep:
            return "EV_SLEEP";
        case EventCode::WakeUp:
            return "EV_WAKE_UP";
        case EventCode::EcuStateChange:
            return "EV_ECU_STATE_CHANGE";
        case EventCode::User:
            return "EV_USER";
        case EventCode::Transport:
            return "EV_TRANSPORT";
    }
    return "EV_UNKNOWN";
}

std::optional<EventCode> ToEventCode(std::uint8_t raw) noexcept {
    switch (static_cast<EventCode>(raw)) {
        case EventCode::ResumeMode:
        case EventCode::ClearDaq:
        case EventCode::StoreDaq:
        case EventCode::StoreCal:
        case EventCode::CmdPending:
        case EventCode::DaqOverload:
        case EventCode::SessionTerminated:
        case EventCode::TimeSync:
        case EventCode::StimTimeout:
        case EventCode::Sleep:
        case EventCode::WakeUp:
        case EventCode::EcuStateChange:
        case EventCode::User:
        case EventCode::Transport:
            return static_cast<EventCode>(raw);
    }
    return std::nullopt;
}

std::optional<AddressGranularity> CommModeBasicToAg(
    std::uint8_t field_value) noexcept {
    // COMM_MODE_BASIC bit1-2：00=BYTE, 01=WORD, 10=DWORD, 11=保留
    switch (field_value & 0x03U) {
        case 0x00U:
            return AddressGranularity::Byte;
        case 0x01U:
            return AddressGranularity::Word;
        case 0x02U:
            return AddressGranularity::DWord;
        default:
            return std::nullopt;
    }
}

std::uint8_t AgToCommModeBasicField(AddressGranularity ag) noexcept {
    switch (ag) {
        case AddressGranularity::Byte:
            return 0x00U;
        case AddressGranularity::Word:
            return 0x01U;
        case AddressGranularity::DWord:
            return 0x02U;
    }
    // 非法 AG 值：按 BYTE 处理，调用方应在上层完成校验
    return 0x00U;
}

std::string_view SessionStateName(SessionState state) noexcept {
    switch (state) {
        case SessionState::Disconnected:
            return "Disconnected";
        case SessionState::Connecting:
            return "Connecting";
        case SessionState::Connected:
            return "Connected";
        case SessionState::Disconnecting:
            return "Disconnecting";
        case SessionState::Recovering:
            return "Recovering";
        case SessionState::Failed:
            return "Failed";
    }
    return "Unknown";
}

std::optional<XcpAddress40> XcpAddress40::Advance(
    ElementCount elements, AddressGranularity ag) const noexcept {
    // 先做乘法溢出检查（ElementCount * AG 字节数），再检查地址回绕
    const auto bytes = static_cast<std::uint64_t>(elements) *
                       static_cast<std::uint64_t>(AgToBytes(ag));
    const auto sum = static_cast<std::uint64_t>(address) + bytes;
    if (sum > 0xFFFFFFFFULL) {
        return std::nullopt;
    }
    return XcpAddress40{static_cast<Address>(sum), extension};
}

bool operator==(const XcpAddress40& lhs, const XcpAddress40& rhs) noexcept {
    return lhs.address == rhs.address && lhs.extension == rhs.extension;
}

}  // namespace calmcar::xcp
