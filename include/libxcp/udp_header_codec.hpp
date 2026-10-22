/**
 * @file udp_header_codec.hpp
 * @brief XCP on Ethernet Transport Header（4 字节 LEN/CTR）与 Tail
 * （PACKET_ALIGNMENT FILL）纯函数编解码。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 6 节实现。
 *
 * @par 规范基线（TL V1.5 原始 PDF：
 *      `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`）
 *   - §4.3.1（PDF 第 9 页）：Header 由 LEN 与 CTR 组成，二者恒为 Intel
 *     （小端）WORD；"LEN is the number of bytes in the original XCP Packet."
 *     —— **LEN 不含 Tail**。
 *   - §4.3.2（PDF 第 9 页）："the tail consists of a control field containing
 *     0,1,2 or 3 FILL bytes."
 *   - §4.3.2.1：单个 XCP 消息独占 Frame 时 "The tail is optional."。
 *   - §4.3.2.2（PDF 第 10 页）：多消息串联时，前一消息的 Tail 用于让后继
 *     XCP Packet 起始地址满足 PACKET_ALIGNMENT；FILL 内容 "do not care"。
 *
 * @note 旧基线（XCP 1.1 Part 3）称 "XCP on Ethernet 无 Tail"。该说法在
 *       PACKET_ALIGNMENT_8 下与 V1.5 一致（tail 恒为 0），故
 *       `EncodeUdpFrame()` / `DecodeUdpDatagram()` 保留为 **8-bit 语义**；
 *       需要 16/32-bit 对齐的路径必须显式调用带 `Aligned` 后缀的入口，
 *       不得依赖任何默认推断（方案 §1.9A.4）。
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

/// @brief 编码后的单个 XCP on Ethernet Frame
/// （Header + 一个 XCP Packet [+ Tail FILL]）
struct UdpFrame {
    Bytes data;  ///< LEN(u16le) + CTR(u16le) + XCP Packet + [FILL...]
    /// @brief 本 Frame 尾部 FILL 字节数（0..3）；8-bit 对齐时恒为 0
    std::uint8_t tail_bytes{0};
};

/// @brief 解码后的 UDP Header 字段
struct UdpHeader {
    DatagramLen len{};  ///< 原始 XCP Packet 字节数（**不含 Tail**）
    DatagramCtr ctr{};  ///< 该 XCP Frame 的独立计数器
};

/**
 * @brief Datagram 内单个 XCP Frame 的只读视图
 * @note xcp_packet 指向 DecodeUdpDatagram()
 * 入参的底层存储，生命周期不超过该视图。
 */
struct UdpFrameView {
    UdpHeader header;      ///< Frame 的 Transport Header
    BytesView xcp_packet;  ///< Frame 内的 XCP Packet（不拥有存储）
    /// @brief 本 Frame 尾随的 FILL 字节数（0..3），见 TL V1.5 §4.3.2.2
    std::uint8_t tail_bytes{0};
};

/**
 * @brief 编码一个 XCP on Ethernet Frame（8-bit 对齐语义，Tail 恒为 0）
 * @param xcp_packet 原始 XCP Packet（不含 Transport Header）
 * @param ctr 该 Frame 的发送计数器值
 * @return 编码后的 Frame（LEN + CTR + Packet）
 * @throws XcpException(InvalidArgument) xcp_packet.size() > kUdpMaxXcpPacket
 * (65503)
 *
 * @note 本函数等价于 `EncodeUdpFrameAligned(xcp_packet, ctr,
 *       PacketAlignment::Bit8)`。需要 16/32-bit 对齐时请改用
 *       `EncodeUdpFrameAligned()`——本函数**不会**根据任何会话状态推断对齐。
 */
[[nodiscard]] UdpFrame EncodeUdpFrame(BytesView xcp_packet, DatagramCtr ctr);

/**
 * @brief 按指定 PACKET_ALIGNMENT 编码一个 XCP on Ethernet Frame
 * @param xcp_packet 原始 XCP Packet（不含 Transport Header）
 * @param ctr 该 Frame 的发送计数器值
 * @param alignment 会话协商到的对齐位宽
 * @return 编码后的 Frame（LEN + CTR + Packet + 0..3 字节 FILL）
 * @throws XcpException(InvalidArgument) xcp_packet.size() > kUdpMaxXcpPacket
 *         (65503)，或 `alignment == PacketAlignment::Unknown`
 *
 * @details 依据 TL V1.5 §4.3.2.2：Tail 长度取
 *          `TailBytesForAlignment(alignment, kUdpHeaderSize + packet.size())`，
 *          使**后继** XCP Packet 的起始偏移满足 alignment。FILL 内容规范定为
 *          "do not care"，本库固定填 0x00。
 *
 * @note `Unknown` 被拒绝而非按 8-bit 处理：无证据时不得假定对齐，
 *       否则会静默生成与对端不兼容的报文（方案 §1.9A.4）。
 */
[[nodiscard]] UdpFrame EncodeUdpFrameAligned(BytesView xcp_packet,
                                             DatagramCtr ctr,
                                             PacketAlignment alignment);

/**
 * @brief 解码一个 UDP Datagram 中连续打包的全部 XCP Frame（8-bit 对齐语义）
 * @param datagram 完整 UDP Payload
 * @return 全部 Frame 视图；下列情况返回
 * std::nullopt（整体失败，不交付部分内容）： 空 Datagram、Header 不完整、LEN 为
 * 0、LEN 越界、末尾残留字节
 * @note XCP 1.1 Part 3 允许一个 UDP Datagram 包含多个完整 Frame，
 *       但任何单个 Frame 都不得跨 Datagram 边界。
 *
 * @note 本函数等价于 `DecodeUdpDatagramAligned(datagram,
 *       PacketAlignment::Bit8)`。**不校验也不跳过 Tail**——8-bit 对齐下
 *       Tail 恒为 0，残留字节即视为畸形并返回 `std::nullopt`。
 */
[[nodiscard]] std::optional<std::vector<UdpFrameView>> DecodeUdpDatagram(
    BytesView datagram) noexcept;

/**
 * @brief 按指定 PACKET_ALIGNMENT 解码 UDP Datagram 中的全部 XCP Frame
 * @param datagram 完整 UDP Payload
 * @param alignment 会话协商到的对齐位宽
 * @return 全部 Frame 视图；畸形输入整体返回 `std::nullopt`
 *
 * @details 与 8-bit 版本的差异：每个 Frame 在 XCP Packet 之后会跳过
 *          `TailBytesForAlignment(alignment, kUdpHeaderSize + len)` 个 FILL
 *          字节，再继续解析后继 Frame。该长度完全由 LEN 与 alignment 推出，
 *          无需读取 FILL 内容（规范定为 "do not care"）。
 *
 * @par 整体拒绝（返回 std::nullopt）的畸形情形
 *   - `datagram` 为空；
 *   - 剩余字节不足以容纳完整 4 字节 Header；
 *   - `LEN == 0`（规范要求 LEN 为原 XCP Packet 字节数，0 无意义）；
 *   - `Header + LEN + Tail` 超出 Datagram 剩余长度（截断）；
 *   - 跳过 Tail 后仍有无法构成完整 Frame 的残尾；
 *   - `alignment == PacketAlignment::Unknown`。
 *
 * @note 上述"整体拒绝"语义与 8-bit 版本一致：绝不对半截 Datagram
 *       交付部分 Frame，避免上层把截断报文误当作完整 DAQ 批次。
 */
[[nodiscard]] std::optional<std::vector<UdpFrameView>> DecodeUdpDatagramAligned(
    BytesView datagram, PacketAlignment alignment) noexcept;

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_UDP_HEADER_CODEC_HPP_
