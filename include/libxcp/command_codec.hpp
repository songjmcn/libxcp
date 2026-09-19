/**
 * @file command_codec.hpp
 * @brief XCP 命令编码器：高层参数 -> CTO Byte Sequence。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 9 节实现。
 * 无状态、纯函数风格；所有 reserved 字节填 0；多字节字段按 Session Byte Order
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
 * 本阶段由调用方（MemoryAccess / CommandExecutor）负责在发送前校验长度不超过
 * MAX_CTO；编码器本身只做格式转换，不隐式截断。
 */
class CommandCodec {
public:
    /**
     * @brief 构造编码器
     * @param byte_order Session 字节序（CONNECT 后确定）
     */
    explicit CommandCodec(ByteOrder byte_order) noexcept;

    /// @brief 当前使用的字节序
    [[nodiscard]] ByteOrder byteOrder() const noexcept;

    // ---- 命令编码 ----

    /**
     * @brief 编码 CONNECT 命令
     * @param mode 0x00=普通模式, 0x01=用户自定义模式
     * @return CTO: [0xFF][mode]
     */
    [[nodiscard]] Bytes encodeConnect(std::uint8_t mode = 0x00) const;

    /// @brief 编码 DISCONNECT 命令；@return CTO: [0xFE][0x00]
    [[nodiscard]] Bytes encodeDisconnect() const;

    /// @brief 编码 GET_STATUS 命令；@return CTO: [0xFD][0x00]
    [[nodiscard]] Bytes encodeGetStatus() const;

    /// @brief 编码 SYNCH 命令；@return CTO: [0xFC][0x00]
    [[nodiscard]] Bytes encodeSynch() const;

    /// @brief 编码 GET_COMM_MODE_INFO 命令；@return CTO: [0xFB][0x00]
    [[nodiscard]] Bytes encodeGetCommModeInfo() const;

    /**
     * @brief 编码 SET_MTA 命令
     * @param extension 地址扩展（Byte 2）
     * @param address 32 位地址（Byte 3..6，按 Session Byte Order）
     * @return CTO: [0xF6][reserved][extension][addr_b3..b0]
     */
    [[nodiscard]] Bytes encodeSetMta(AddressExtension extension,
                                     Address address) const;

    /**
     * @brief 编码 UPLOAD 命令
     * @param number_of_elements 要读取的元素数（以 AG 为单位，有效范围 1..255）
     * @return CTO: [0xF5][n]
     * @note 普通模式下协议上限为 MAX_CTO/AG - 1，由上层校验；此处仅拒绝 0 和
     * >255。
     */
    [[nodiscard]] Bytes encodeUpload(ElementCount number_of_elements) const;

    /**
     * @brief 编码 SHORT_UPLOAD 命令
     * @param number_of_elements 要读取的元素数（有效范围 1..255）
     * @param extension 地址扩展
     * @param address 32 位地址
     * @return CTO: [0xF4][n][reserved][extension][addr_b3..b0]
     */
    [[nodiscard]] Bytes encodeShortUpload(ElementCount number_of_elements,
                                          AddressExtension extension,
                                          Address address) const;

private:
    ByteOrder byte_order_;  ///< Session 字节序

    /// @brief 按字节序追加 16 位值到缓冲区尾部
    void writeU16(Bytes& buf, std::uint16_t val) const;

    /// @brief 按字节序追加 32 位值到缓冲区尾部
    void writeU32(Bytes& buf, std::uint32_t val) const;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_COMMAND_CODEC_HPP_
