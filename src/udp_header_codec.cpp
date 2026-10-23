/**
 * @file udp_header_codec.cpp
 * @brief UDP 侧 Header 编解码 —— **兼容转发层**。
 *
 * @par 本文件自 B-T1 起的定位变更
 *   算法本体已迁至 `src/ethernet_header_codec.cpp`（UDP/TCP 共享）。本文件
 *   只做两件事：
 *     1. 维持 UDP **专属**的 65503 字节 Packet 上界校验（IPv4 Datagram 物理
 *        限制），该限制**不得**被通用层上界 65535 取代；
 *     2. 把旧入口原样转发到通用层实现。
 *   从而 UDP 既有调用点的行为与错误信息一字不变。
 */

#include "libxcp/udp_header_codec.hpp"

#include <string>

namespace calmcar::xcp {

namespace {

/// @brief 校验 XCP Packet 长度是否可编码进单个 **UDP** Frame
/// @param packet_size 原始 XCP Packet 字节数
/// @throws XcpException(InvalidArgument) 超出 UDP 单 Frame 上限
///         `kUdpMaxXcpPacket`(65503)
/// @details 与通用层的 `CheckEthernetPacketLen()` 分开维护：通用层上界是
///          LEN 字段能表示的最大值 65535，而 UDP 还额外受 IPv4 Datagram
///          容量限制，实际上界是 65503。UDP 路径必须用本函数。
void ValidateUdpPacketSize(std::size_t packet_size) {
    if (packet_size > kUdpMaxXcpPacket) {
        throw detail::MakeInvalidArgument(
            "XCP Packet 超过单个 Frame 上限 " +
            std::to_string(kUdpMaxXcpPacket) +
            " 字节: " + std::to_string(packet_size));
    }
    if (packet_size == 0U) {
        // 用户裁决（m00560）：按计划 §4.1 拒绝空 Packet。本库不再延续
        // 迁移前"编码器允许 LEN=0"的旧行为，编码与解码口径现已统一。
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度不得为 0：每个 Frame 至少携带 1 字节 PID");
    }
    if (packet_size > 0xFFFFU) {
        // LEN 为 16 位字段，理论上被上一条限制覆盖；此处保留防御性检查
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度超出 LEN 字段可表示范围");
    }
}

}  // namespace

UdpFrame EncodeUdpFrame(BytesView xcp_packet, DatagramCtr ctr) {
    // 保持旧的 8-bit 语义：显式指定 Bit8，绝不依据会话状态推断
    return EncodeUdpFrameAligned(xcp_packet, ctr, PacketAlignment::Bit8);
}

UdpFrame EncodeUdpFrameAligned(BytesView xcp_packet, DatagramCtr ctr,
                               PacketAlignment alignment) {
    // 先按 UDP 专属上界（65503）校验，再交给通用层编码
    ValidateUdpPacketSize(xcp_packet.size());
    return EncodeEthernetFrameAligned(xcp_packet, ctr, alignment);
}

std::optional<std::vector<UdpFrameView>> DecodeUdpDatagram(
    BytesView datagram) noexcept {
    return DecodeUdpDatagramAligned(datagram, PacketAlignment::Bit8);
}

std::optional<std::vector<UdpFrameView>> DecodeUdpDatagramAligned(
    BytesView datagram, PacketAlignment alignment) noexcept {
    // 解码没有 UDP 与 TCP 的语义差异：Datagram 边界即 Frame 边界，
    // 通用层的「整体拒绝」语义正是 UDP 所需，故直接转发。
    return DecodeEthernetDatagramAligned(datagram, alignment);
}

}  // namespace calmcar::xcp