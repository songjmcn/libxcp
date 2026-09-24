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
    /// @brief 构造编码器
    /// @param byte_order Session 字节序（CONNECT
    /// 后确定），决定多字节字段编码方向
    explicit CommandCodec(ByteOrder byte_order) noexcept;

    /// @brief 当前使用的字节序
    [[nodiscard]] ByteOrder GetByteOrder() const noexcept;

    // ---- 命令编码 ----

    /// @brief 编码 CONNECT 命令
    /// @param mode 0x00=普通, 0x01=用户自定义
    /// @return CTO: [0xFF][mode]
    [[nodiscard]] Bytes EncodeConnect(std::uint8_t mode = 0x00) const;

    /// @brief 编码 DISCONNECT 命令
    /// @return CTO: [0xFE][0x00]
    [[nodiscard]] Bytes EncodeDisconnect() const;

    /// @brief 编码 GET_STATUS 命令
    /// @return CTO: [0xFD][0x00]
    [[nodiscard]] Bytes EncodeGetStatus() const;

    /// @brief 编码 SYNCH 命令（超时恢复的第一步，Slave 以 ERR_CMD_SYNCH 确认）
    /// @return CTO: [0xFC][0x00]
    [[nodiscard]] Bytes EncodeSynch() const;

    /// @brief 编码 GET_COMM_MODE_INFO 命令
    /// @return CTO: [0xFB][0x00]
    [[nodiscard]] Bytes EncodeGetCommModeInfo() const;

    /// @brief 编码 SET_MTA 命令（设置 Slave 隐含内存地址）
    /// @param extension 地址扩展（8 位）
    /// @param address 32 位地址，按 Session 字节序编码
    /// @return CTO: [0xF6][reserved][extension][addr_b0..b3]
    [[nodiscard]] Bytes EncodeSetMta(AddressExtension extension,
                                     Address address) const;

    /// @brief 编码 UPLOAD 命令（从当前隐含 MTA 读取）
    /// @param number_of_elements 要读取的元素数（以 AG 为单位），1..0xFF
    /// @return CTO: [0xF5][number_of_elements]
    /// @throws XcpException(InvalidArgument) 元素数为 0 或超过单字段 0xFF 上限
    [[nodiscard]] Bytes EncodeUpload(ElementCount number_of_elements) const;

    /// @brief 编码 SHORT_UPLOAD 命令（一次命令完成定址读取，不改动 MTA）
    /// @param number_of_elements 要读取的元素数（以 AG 为单位），1..0xFF
    /// @param extension 地址扩展（8 位）
    /// @param address 32 位地址，按 Session 字节序编码
    /// @return CTO:
    /// [0xF4][number_of_elements][reserved][extension][addr_b0..b3]
    /// @throws XcpException(InvalidArgument) 元素数为 0 或超过单字段 0xFF 上限
    [[nodiscard]] Bytes EncodeShortUpload(ElementCount number_of_elements,
                                          AddressExtension extension,
                                          Address address) const;

    /// @brief 编码 GET_SEED 命令（读取解锁 Seed 的指定分段）
    /// @param resource 要解锁的资源（协议要求恰为单个资源位）
    /// @param mode First=首段（响应含 Seed 总长度）；Remainder=续取后续分段
    /// @return CTO: [0xF8][mode][resource]
    /// @note 报文为 Mode 在前、Resource 在后；全部单字节字段，
    ///       与 Session Byte Order 无关。
    [[nodiscard]] Bytes EncodeGetSeed(Resource resource, SeedMode mode) const;

    /// @brief 编码 UNLOCK 命令（发送 Key 的一个分段）
    /// @param length_field Length 字段：首帧填 Key 总长度，后续帧填剩余长度
    /// @param key_segment 本帧携带的 Key 字节（分段上限 MAX_CTO-2
    /// 由编排层保证）
    /// @return CTO: [0xF7][length][key...]
    /// @throws XcpException(InvalidArgument) length_field 小于本帧字节数
    [[nodiscard]] Bytes EncodeUnlock(std::uint8_t length_field,
                                     BytesView key_segment) const;

private:
    /// @brief 构造时确定的 Session 字节序
    ByteOrder m_byte_order_;

    /// @brief 按 Session 字节序向缓冲区写入 16 位值
    void WriteU16(Bytes& buf, std::uint16_t val) const;
    /// @brief 按 Session 字节序向缓冲区写入 32 位值
    void WriteU32(Bytes& buf, std::uint32_t val) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_COMMAND_CODEC_HPP_