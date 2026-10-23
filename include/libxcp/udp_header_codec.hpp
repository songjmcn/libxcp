/**
 * @file udp_header_codec.hpp
 * @brief UDP 侧 Transport Header 编解码 —— **兼容别名层**。
 *
 * @par 本文件自 B-T1 起的定位变更
 *   原本体实现已泛化为以太网通用实现并迁至
 *   `libxcp/ethernet_header_codec.hpp`（UDP 与 TCP 共享 LEN/CTR/Tail 算法）。
 *   本头文件**只保留历史名称与前向声明**，不复制任何算法，确保 UDP 侧既有
 *   调用点与二进制行为一字不变。
 *
 *   迁移对照（新代码请直接使用 `ethernet_header_codec.hpp`）：
 *   | 旧名（本文件，保留）              | 新名（通用层）|
 *   |-----------------------------------|------------------------------------|
 *   | `kUdpHeaderSize`                  | `kEthernetHeaderSize`              |
 *   | `kUdpMaxDatagramSize`             | `kUdpMaxDatagramSize`（同名迁移）  |
 *   | `kUdpMaxXcpPacket`                | `kUdpMaxXcpPacket`（同名迁移）     |
 *   | `UdpFrame`                        | `EthernetFrame`                    |
 *   | `UdpHeader`                       | `EthernetHeader`                   |
 *   | `UdpFrameView`                    | `EthernetFrameView`                |
 *   | `EncodeUdpFrame()`                | `EncodeUdpFrame()`（本文件内联转发）|
 *   | `EncodeUdpFrameAligned()`         | `EncodeUdpFrameAligned()`（同左）  |
 *   | `DecodeUdpDatagram()`             | `DecodeUdpDatagram()`（同左）      |
 *   | `DecodeUdpDatagramAligned()`      | `DecodeUdpDatagramAligned()`（同左）|
 *
 * @note UDP 专属的长度上界语义保持不变：`kUdpMaxXcpPacket` = 65503
 *       （IPv4 Datagram 物理限制），**不是**通用层上界
 *       `kEthernetMaxXcpPacket`(65535)。UDP 侧仍必须用前者校验，否则会生成
 *       超过 IPv4 Datagram 容量的 Frame。
 */

#ifndef CALMCAR_XCP_UDP_HEADER_CODEC_HPP_
#define CALMCAR_XCP_UDP_HEADER_CODEC_HPP_

#include <optional>
#include <vector>

#include "libxcp/ethernet_header_codec.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/// @brief UDP Transport Header 固定长度（字节）：LEN(2) + CTR(2)
/// @note 兼容别名，指向通用层的 `kEthernetHeaderSize`。
constexpr std::size_t kUdpHeaderSize = kEthernetHeaderSize;

/// @brief IPv4 UDP Payload 最大理论长度（65535 - 8 Byte UDP Header - 20 Byte IP
/// Header）
/// @note 定义在通用层头文件中，此处经 included header 直接可见。
///       保留本注释以维持旧文档锚点。

/// @brief 编码后的单个 XCP on Ethernet Frame（UDP 视角的旧名）
/// @note 兼容别名，与 `EthernetFrame` 是**同一类型**，可互相赋值。
using UdpFrame = EthernetFrame;

/// @brief 解码后的 UDP Header 字段（UDP 视角的旧名）
/// @note 兼容别名，与 `EthernetHeader` 是同一类型。
using UdpHeader = EthernetHeader;

/// @brief Datagram 内单个 XCP Frame 的只读视图（UDP 视角的旧名）
/// @note 兼容别名，与 `EthernetFrameView` 是同一类型。
using UdpFrameView = EthernetFrameView;

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
 * @note 转发到 `EncodeEthernetFrameAligned()`，但先按 **UDP 专属上界**
 *       校验，保持 UDP 侧既有的 65503 限制不变。
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