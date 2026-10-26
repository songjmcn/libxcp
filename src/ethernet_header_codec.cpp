/**
 * @file ethernet_header_codec.cpp
 * @brief XCP on Ethernet 通用 Header/Tail 编解码实现：LEN/CTR 固定小端，
 * 与 Session Byte Order 完全解耦；Tail（PACKET_ALIGNMENT FILL）见
 * TL V1.5 §4.3.2。
 *
 * 算法自原 `src/udp_header_codec.cpp` 原样迁入（行为一字未改），使 UDP 与
 * TCP 共享同一份 Header/LEN/CTR/Tail 实现。
 */

#include "libxcp/ethernet_header_codec.hpp"

#include <string>

namespace calmcar::xcp {

void CheckEthernetPacketLen(std::size_t len) {
    if (len == 0U) {
        // 计划 §4.1 明确要求：空 Packet 抛 InvalidArgument。
        // 每个 Frame 至少携带 1 字节 XCP Packet（PID），LEN=0 的 Frame
        // 不构成合法 XCP 消息，禁止编出。
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度不得为 0：每个 Frame 至少携带 1 字节 PID");
    }
    if (len > kEthernetMaxXcpPacket) {
        // LEN 为 16 位字段，超过 0xFFFF 无法编入
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度超出 LEN 字段可表示范围 " +
            std::to_string(kEthernetMaxXcpPacket) +
            " 字节: " + std::to_string(len));
    }
}

EthernetFrame EncodeEthernetFrameAligned(BytesView xcp_packet, DatagramCtr ctr,
                                         PacketAlignment alignment) {
    CheckEthernetPacketLen(xcp_packet.size());

    // 无对齐依据时拒绝编码：不得静默按 8-bit 处理（方案 §1.9A.4）
    if (alignment == PacketAlignment::Unknown) {
        throw detail::MakeInvalidArgument(
            "PACKET_ALIGNMENT 未知，无法编码 Ethernet Frame："
            "需要 A2L 或显式配置提供对齐位宽");
    }

    // Tail 长度由「Header + Packet」推出，使后继 Packet 起始偏移满足对齐；
    // LEN 只计原 XCP Packet，不含 Tail（TL V1.5 §4.3.1.1）
    const std::uint8_t tail = TailBytesForAlignment(
        alignment, kEthernetHeaderSize + xcp_packet.size());

    EthernetFrame frame;
    frame.tail_bytes = tail;
    frame.data.reserve(kEthernetHeaderSize + xcp_packet.size() + tail);

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

EthernetFrame EncodeEthernetFrameUnpadded(BytesView xcp_packet,
                                          DatagramCtr ctr) {
    // AsamV14 只做长度校验，**不查 alignment**：该 Profile 的 wire 形式与
    // alignment 无关，恒为 Header + Packet（方案 §1.4）
    CheckEthernetPacketLen(xcp_packet.size());

    EthernetFrame frame;
    frame.tail_bytes = 0;
    frame.data.reserve(kEthernetHeaderSize + xcp_packet.size());

    const auto len = static_cast<DatagramLen>(xcp_packet.size());
    // LEN：固定 Intel（低字节在前）
    frame.data.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    frame.data.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFU));
    // CTR：同样固定 Intel
    frame.data.push_back(static_cast<std::uint8_t>(ctr & 0xFFU));
    frame.data.push_back(static_cast<std::uint8_t>((ctr >> 8) & 0xFFU));
    // 原始 XCP Packet：到此为止，绝不附加任何 Tail
    frame.data.insert(frame.data.end(), xcp_packet.begin(), xcp_packet.end());
    return frame;
}

std::optional<std::vector<EthernetFrameView>> DecodeEthernetDatagramByBoundary(
    BytesView datagram, std::uint8_t max_tail) noexcept {
    // 空 Datagram 或不足一个 Header：无法解析
    if (datagram.size() < kEthernetHeaderSize) {
        return std::nullopt;
    }
    // 规范上限为 3（对齐最大 32-bit）；超出视为调用方参数错误
    if (max_tail > 3U) {
        return std::nullopt;
    }

    std::vector<EthernetFrameView> frames;
    std::size_t offset = 0;

    while (offset < datagram.size()) {
        const std::size_t remaining = datagram.size() - offset;
        // 剩余字节不足以构成一个 Header -> 截断，整体失败
        if (remaining < kEthernetHeaderSize) {
            return std::nullopt;
        }

        const auto* p = datagram.data() + offset;
        EthernetHeader header;
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
        if (static_cast<std::size_t>(header.len) >
            remaining - kEthernetHeaderSize) {
            return std::nullopt;
        }

        const std::size_t payload_end =
            offset + kEthernetHeaderSize + static_cast<std::size_t>(header.len);
        const std::size_t rest = datagram.size() - payload_end;

        // 剩余字节不超过 max_tail 时，本帧即末帧，剩余字节全部是 FILL；
        // 因为 max_tail <= 3，这些字节不可能构成下一帧的 4 字节 Header，
        // 故该判定无歧义（不读取 FILL 内容）。
        std::uint8_t tail = 0;
        if (rest > 0U) {
            if (rest > static_cast<std::size_t>(max_tail)) {
                // 剩余字节多于 max_tail：说明后面还有整帧，中间帧不得有 Tail
                tail = 0;
            } else {
                tail = static_cast<std::uint8_t>(rest);
            }
        }

        EthernetFrameView view;
        view.header = header;
        view.xcp_packet =
            datagram.subspan(offset + kEthernetHeaderSize, header.len);
        view.tail_bytes = tail;
        frames.push_back(view);

        offset = payload_end + static_cast<std::size_t>(tail);
    }

    // 正常结束必然精确推进到末尾；防御性再确认一次
    if (offset != datagram.size() || frames.empty()) {
        return std::nullopt;
    }
    return frames;
}

std::optional<std::vector<EthernetFrameView>> DecodeEthernetDatagramAligned(
    BytesView datagram, PacketAlignment alignment) noexcept {
    // 空 Datagram 或不足一个 Header：无法解析
    if (datagram.size() < kEthernetHeaderSize) {
        return std::nullopt;
    }
    // Unknown 对齐无法推出 Tail 长度，整体拒绝而非按 8-bit 猜测
    if (alignment == PacketAlignment::Unknown) {
        return std::nullopt;
    }

    std::vector<EthernetFrameView> frames;
    std::size_t offset = 0;

    while (offset < datagram.size()) {
        const std::size_t remaining = datagram.size() - offset;
        // 剩余字节不足以构成一个 Header -> 截断，整体失败
        if (remaining < kEthernetHeaderSize) {
            return std::nullopt;
        }

        const auto* p = datagram.data() + offset;
        EthernetHeader header;
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
        if (static_cast<std::size_t>(header.len) >
            remaining - kEthernetHeaderSize) {
            return std::nullopt;
        }

        // Tail 长度同样由 LEN 与 alignment 推出，无需读取 FILL 内容
        const std::uint8_t tail = TailBytesForAlignment(
            alignment, kEthernetHeaderSize + header.len);
        const std::size_t frame_total =
            kEthernetHeaderSize + static_cast<std::size_t>(header.len) + tail;
        // 含 Tail 的完整 Frame 超出剩余长度 -> 截断，整体失败
        if (frame_total > remaining) {
            return std::nullopt;
        }

        EthernetFrameView view;
        view.header = header;
        view.xcp_packet =
            datagram.subspan(offset + kEthernetHeaderSize, header.len);
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