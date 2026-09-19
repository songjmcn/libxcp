/**
 * @file command_codec.hpp
 * @brief XCP 命令编码器：高层参数 -> CTO Byte Sequence。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 9 节实现。
 * 无状态，纯函数风格；所有 reserved 字节填 0；多字节字段按 Session Byte Order
 * 编码。
 */

#ifndef CALMCAR_XCP_COMMAND_CODEC_HPP_
#define CALMCAR_XCP_COMMAND_CODEC_HPP_

#include <cstdint>

#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief XCP 命令编码器
 *
 * 将命令参数编码为 CTO Byte Sequence。编码使用当前 Session 的 Byte Order
 * （CONNECT 协商后确定）。所有 reserved 字节填 0。
 */
class CommandCodec {
public:
    explicit CommandCodec(ByteOrder byte_order) noexcept;

    /// @brief 当前使用的字节序
    [[nodiscard]] ByteOrder GetByteOrder() const noexcept;

    // ---- 命令编码 ----

    [[nodiscard]] Bytes EncodeConnect(std::uint8_t mode = 0x00) const;
    [[nodiscard]] Bytes EncodeDisconnect() const;
    [[nodiscard]] Bytes EncodeGetStatus() const;
    [[nodiscard]] Bytes EncodeSynch() const;
    [[nodiscard]] Bytes EncodeGetCommModeInfo() const;
    [[nodiscard]] Bytes EncodeSetMta(AddressExtension extension,
                                     Address address) const;
    [[nodiscard]] Bytes EncodeUpload(ElementCount number_of_elements) const;
    [[nodiscard]] Bytes EncodeShortUpload(ElementCount number_of_elements,
                                          AddressExtension extension,
                                          Address address) const;

private:
    ByteOrder m_byte_order_;

    void WriteU16(Bytes& buf, std::uint16_t val) const;
    void WriteU32(Bytes& buf, std::uint32_t val) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_COMMAND_CODEC_HPP_