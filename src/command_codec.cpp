/**
 * @file command_codec.cpp
 * @brief CommandCodec 的实现：8 条命令的 CTO 编码与字节序处理。
 *
 * 报文布局依据 docs/XCP_1.3.0_document.md 第 7.5.1 节与
 * code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 9.1 / 17.1 节的字节表。
 */

#include "libxcp/command_codec.hpp"

#include <string>

namespace calmcar::xcp {

namespace {

/// @brief UPLOAD / SHORT_UPLOAD 的元素数字段为单字节，有效取值 1..255
constexpr ElementCount kMaxElementsPerField = 0xFFU;

}  // namespace

CommandCodec::CommandCodec(ByteOrder byte_order) noexcept
    : byte_order_(byte_order) {}

ByteOrder CommandCodec::byteOrder() const noexcept { return byte_order_; }

void CommandCodec::writeU16(Bytes& buf, std::uint16_t val) const {
    if (byte_order_ == ByteOrder::Intel) {
        // 小端：低字节在前
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
    } else {
        // 大端：高字节在前
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
    }
}

void CommandCodec::writeU32(Bytes& buf, std::uint32_t val) const {
    if (byte_order_ == ByteOrder::Intel) {
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 16) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 24) & 0xFFU));
    } else {
        buf.push_back(static_cast<std::uint8_t>((val >> 24) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 16) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
    }
}

Bytes CommandCodec::encodeConnect(std::uint8_t mode) const {
    // CONNECT: [FF][mode]，mode 0x00=普通 / 0x01=用户自定义
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Connect));
    cto.push_back(mode);
    return cto;
}

Bytes CommandCodec::encodeDisconnect() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Disconnect));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::encodeGetStatus() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetStatus));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::encodeSynch() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Synch));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::encodeGetCommModeInfo() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetCommModeInfo));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::encodeSetMta(AddressExtension extension,
                                 Address address) const {
    // SET_MTA: [F6][reserved=0x00][EXT][ADD 4 字节按 Session Byte Order]
    Bytes cto;
    cto.reserve(7);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::SetMta));
    cto.push_back(0x00U);  // reserved
    cto.push_back(extension);
    writeU32(cto, address);
    return cto;
}

Bytes CommandCodec::encodeUpload(ElementCount number_of_elements) const {
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        throw detail::makeInvalidArgument(
            "UPLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    // UPLOAD: [F5][n]
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Upload));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    return cto;
}

Bytes CommandCodec::encodeShortUpload(ElementCount number_of_elements,
                                      AddressExtension extension,
                                      Address address) const {
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        throw detail::makeInvalidArgument(
            "SHORT_UPLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    // SHORT_UPLOAD: [F4][n][reserved=0x00][EXT][ADD 4 字节按 Session Byte
    // Order]
    Bytes cto;
    cto.reserve(8);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::ShortUpload));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    cto.push_back(0x00U);  // reserved
    cto.push_back(extension);
    writeU32(cto, address);
    return cto;
}

}  // namespace calmcar::xcp
