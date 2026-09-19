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
    : m_byte_order_(byte_order) {}

ByteOrder CommandCodec::GetByteOrder() const noexcept { return m_byte_order_; }

void CommandCodec::WriteU16(Bytes& buf, std::uint16_t val) const {
    if (m_byte_order_ == ByteOrder::Intel) {
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
    } else {
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
    }
}

void CommandCodec::WriteU32(Bytes& buf, std::uint32_t val) const {
    if (m_byte_order_ == ByteOrder::Intel) {
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

Bytes CommandCodec::EncodeConnect(std::uint8_t mode) const {
    // CONNECT: [FF][mode]，mode 0x00=普通 / 0x01=用户自定义
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Connect));
    cto.push_back(mode);
    return cto;
}

Bytes CommandCodec::EncodeDisconnect() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Disconnect));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::EncodeGetStatus() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetStatus));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::EncodeSynch() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Synch));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::EncodeGetCommModeInfo() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetCommModeInfo));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::EncodeSetMta(AddressExtension extension,
                                 Address address) const {
    Bytes cto;
    cto.reserve(7);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::SetMta));
    cto.push_back(0x00U);  // reserved
    cto.push_back(extension);
    WriteU32(cto, address);
    return cto;
}

Bytes CommandCodec::EncodeUpload(ElementCount number_of_elements) const {
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        throw detail::MakeInvalidArgument(
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

Bytes CommandCodec::EncodeShortUpload(ElementCount number_of_elements,
                                      AddressExtension extension,
                                      Address address) const {
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        throw detail::MakeInvalidArgument(
            "SHORT_UPLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    Bytes cto;
    cto.reserve(8);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::ShortUpload));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    cto.push_back(0x00U);  // reserved
    cto.push_back(extension);
    WriteU32(cto, address);
    return cto;
}

}  // namespace calmcar::xcp
