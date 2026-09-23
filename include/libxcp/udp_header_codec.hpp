/**
 * @file udp_header_codec.hpp
 * @brief XCP on Ethernet Transport Header（4 字节 LEN/CTR）纯函数编解码。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 6 节实现。
 * 规范依据 docs/ASAM_XCP_Part3_XCP_on_Ethernet_TCP_UDP_1.1.md 第 1.3 节：
 *   - Header 由 Control Field 组成，含 LEN 与 CTR，二者恒为 Intel（小端）WORD；
 *   - XCP on Ethernet 无 Tail；
 *   - 一个 UDP Datagram 可连续打包多个完整 XCP Frame，任一 Frame 不得跨
 * Datagram 边界。
 */

#ifndef CALMCAR_XCP_UDP_HEADER_CODEC_HPP_
#define CALMCAR_XCP_UDP_HEADER_CODEC_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/// @brief UDP Transport Header 固定长度（字节）：LEN(2) + CTR(2)
constexpr std::size_t kUdpHeaderSize = 4;

/// @brief IPv4 UDP Payload 最大理论长度（65535 - 8 Byte UDP Header - 20 Byte IP
/// Header）
constexpr std::size_t kUdpMaxDatagramSize = 65507;

/**
 * @brief 单个 XCP Frame 中原始 XCP Packet 的最大理论长度
 * @details 单 Frame 独占 UDP Datagram 时：65507 - 4 Byte Header = 65503。
 */
constexpr std::size_t kUdpMaxXcpPacket = kUdpMaxDatagramSize - kUdpHeaderSize;

/// @brief 编码后的单个 XCP on Ethernet Frame（Header + 一个 XCP Packet）
struct UdpFrame {
    Bytes data;  ///< LEN(u16le) + CTR(u16le) + XCP Packet
};

/// @brief 解码后的 UDP Header 字段
struct UdpHeader {
    DatagramLen len{};  ///< 原始 XCP Packet 字节数
    DatagramCtr ctr{};  ///< 该 XCP Frame 的独立计数器
};

/**
 * @brief Datagram 内单个 XCP Frame 的只读视图
 * @note xcp_packet 指向 DecodeUdpDatagram()
 * 入参的底层存储，生命周期不超过该视图。
 */
struct UdpFrameView {
    UdpHeader header;
    BytesView xcp_packet;
};

/**
 * @brief 编码一个 XCP on Ethernet Frame
 * @param xcp_packet 原始 XCP Packet（不含 Transport Header）
 * @param ctr 该 Frame 的发送计数器值
 * @return 编码后的 Frame（LEN + CTR + Packet）
 * @throws XcpException(InvalidArgument) xcp_packet.size() > kUdpMaxXcpPacket
 * (65503)
 */
[[nodiscard]] UdpFrame EncodeUdpFrame(BytesView xcp_packet, DatagramCtr ctr);

/**
 * @brief 解码一个 UDP Datagram 中连续打包的全部 XCP Frame
 * @param datagram 完整 UDP Payload
 * @return 全部 Frame 视图；下列情况返回
 * std::nullopt（整体失败，不交付部分内容）： 空 Datagram、Header 不完整、LEN 为
 * 0、LEN 越界、末尾残留字节
 * @note XCP 1.1 Part 3 允许一个 UDP Datagram 包含多个完整 Frame，
 *       但任何单个 Frame 都不得跨 Datagram 边界。
 */
[[nodiscard]] std::optional<std::vector<UdpFrameView>> DecodeUdpDatagram(
    BytesView datagram) noexcept;

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_UDP_HEADER_CODEC_HPP_
