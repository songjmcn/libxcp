/**
 * @file udp_header_codec.cpp
 * @brief UdpHeaderCodec 实现：LEN/CTR 固定小端，与 Session Byte Order
 * 完全解耦；Tail（PACKET_ALIGNMENT FILL）见 TL V1.5 §4.3.2。
 */

#include "libxcp/udp_header_codec.hpp"

#include <string>

namespace calmcar::xcp {

namespace {

/// @brief 校验 XCP Packet 长度是否可编码进单个 Frame
/// @param packet_size 原始 XCP Packet 字节数
/// @throws XcpException(InvalidArgument) 超出单 Frame 上限或 LEN 可表示范围
void ValidatePacketSize(std::size_t packet_size) {
    if (packet_size > kUdpMaxXcpPacket) {
        throw detail::MakeInvalidArgument(
            "XCP Packet 超过单个 Frame 上限 " +
            std::to_string(kUdpMaxXcpPacket) +
            " 字节: " + std::to_string(packet_size));
    }
    if (packet_size > 0xFFFFU) {
        // LEN 为 16 位字段，理论上被上一条限制覆盖；此处保留防御性检查
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度超出 LEN 字段可表示范围");
    }
}

}  // namespace

UdpFrame EncodeUdpFrame(BytesView xcp_packet, DatagramCtr ctr) {
    return EncodeUdpFrameAligned(xcp_packet, ctr, PacketAlignment::Bit8);
}

UdpFrame EncodeUdpFrameAligned(BytesView xcp_packet, DatagramCtr ctr,
                               PacketAlignment alignment) {
    ValidatePacketSize(xcp_packet.size());

    // 无对齐依据时拒绝编码：不得静默按 8-bit 处理（方案 §1.9A.4）
    if (alignment == PacketAlignment::Unknown) {
        throw detail::MakeInvalidArgument(
            "PACKET_ALIGNMENT 未知，无法编码 UDP Frame："
            "需要 A2L 或显式配置提供对齐位宽");
    }

    // Tail 长度由「Header + Packet」推出，使后继 Packet 起始偏移满足对齐；
    // LEN 只计原 XCP Packet，不含 Tail（TL V1.5 §4.3.1.1）
    const std::uint8_t tail = TailBytesForAlignment(
        alignment, kUdpHeaderSize + xcp_packet.size());

    UdpFrame frame;
    frame.tail_bytes = tail;
    frame.data.reserve(kUdpHeaderSize + xcp_packet.size() + tail);

    const auto len = static_cast<DatagramLen>(xcp_packet.size());
    // LEN：固定 Intel（低字节在前），不受 Session Byte Order 影响
    frame.data.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    frame.data.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFU));
    // CTR：同样固定 Intel
    frame.data.push_back(static_cast<std::uint8_t>(ctr & 0xFFU));
    frame.data.push_back(static_cast<std::uint8_t>((ctr >> 8) & 0xFFU));
    // 原始 XCP Packet
    frame.data.insert(frame.data.end(), xcp_packet.begin(), xcp_packet.end());
    // Tail FILL：规范定为 "do not care"，本库固定填 0x00 便于比对
    frame.data.insert(frame.data.end(), tail, static_cast<std::uint8_t>(0x00));
    return frame;
}

std::optional<std::vector<UdpFrameView>> DecodeUdpDatagram(
    BytesView datagram) noexcept {
    return DecodeUdpDatagramAligned(datagram, PacketAlignment::Bit8);
}

std::optional<std::vector<UdpFrameView>> DecodeUdpDatagramAligned(
    BytesView datagram, PacketAlignment alignment) noexcept {
    // 空 Datagram 或不足一个 Header：无法解析
    if (datagram.size() < kUdpHeaderSize) {
        return std::nullopt;
    }
    // Unknown 对齐无法推出 Tail 长度，整体拒绝而非按 8-bit 猜测
    if (alignment == PacketAlignment::Unknown) {
        return std::nullopt;
    }

    std::vector<UdpFrameView> frames;
    std::size_t offset = 0;

    while (offset < datagram.size()) {
        const std::size_t remaining = datagram.size() - offset;
        // 剩余字节不足以构成一个 Header -> 截断，整体失败
        if (remaining < kUdpHeaderSize) {
            return std::nullopt;
        }

        const auto* p = datagram.data() + offset;
        UdpHeader header;
        header.len =
            static_cast<DatagramLen>(static_cast<std::uint16_t>(p[0]) |
                                     (static_cast<std::uint16_t>(p[1]) << 8));
        header.ctr =
            static_cast<DatagramCtr>(static_cast<std::uint16_t>(p[2]) |
                                     (static_cast<std::uint16_t>(p[3]) << 8));

        // LEN == 0 非法：每个 Frame 至少携带 1 字节 XCP Packet（PID）
        if (header.len == 0U) {
            return std::nullopt;
        }
        // LEN 越界：Frame 不得跨 Datagram 边界
        if (static_cast<std::size_t>(header.len) > remaining - kUdpHeaderSize) {
            return std::nullopt;
        }

        // Tail 长度同样由 LEN 与 alignment 推出，无需读取 FILL 内容
        const std::uint8_t tail =
            TailBytesForAlignment(alignment, kUdpHeaderSize + header.len);
        const std::size_t frame_total =
            kUdpHeaderSize + static_cast<std::size_t>(header.len) + tail;
        // 含 Tail 的完整 Frame 超出剩余长度 -> 截断，整体失败
        if (frame_total > remaining) {
            return std::nullopt;
        }

        UdpFrameView view;
        view.header = header;
        view.xcp_packet = datagram.subspan(offset + kUdpHeaderSize, header.len);
        view.tail_bytes = tail;
        frames.push_back(view);

        offset += frame_total;
    }

    // 循环结束时 offset 必然等于 datagram.size()（含 Tail 精确推进）；
    // 若出现残留字节，上面的 frame_total 越界检查已经拦截，这里再显式确认。
    if (offset != datagram.size() || frames.empty()) {
        return std::nullopt;
    }
    return frames;
}

}  // namespace calmcar::xcp
