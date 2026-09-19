/**
 * @file udp_header_codec.cpp
 * @brief UdpHeaderCodec 实现：LEN/CTR 固定小端，与 Session Byte Order
 * 完全解耦。
 */

#include "libxcp/udp_header_codec.hpp"

#include <string>

namespace calmcar::xcp {

UdpFrame encodeUdpFrame(BytesView xcp_packet, DatagramCtr ctr) {
    if (xcp_packet.size() > kUdpMaxXcpPacket) {
        throw detail::MakeInvalidArgument(
            "XCP Packet 超过单个 Frame 上限 " +
            std::to_string(kUdpMaxXcpPacket) +
            " 字节: " + std::to_string(xcp_packet.size()));
    }
    if (xcp_packet.size() > 0xFFFFU) {
        // LEN 为 16 位字段，理论上被上一条限制覆盖；此处保留防御性检查
        throw detail::MakeInvalidArgument(
            "XCP Packet 长度超出 LEN 字段可表示范围");
    }

    UdpFrame frame;
    frame.m_data_.reserve(kUdpHeaderSize + xcp_packet.size());

    const auto len = static_cast<DatagramLen>(xcp_packet.size());
    // LEN：固定 Intel（低字节在前），不受 Session Byte Order 影响
    frame.m_data_.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    frame.m_data_.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFU));
    // CTR：同样固定 Intel
    frame.m_data_.push_back(static_cast<std::uint8_t>(ctr & 0xFFU));
    frame.m_data_.push_back(static_cast<std::uint8_t>((ctr >> 8) & 0xFFU));
    // 原始 XCP Packet
    frame.m_data_.insert(frame.m_data_.end(), xcp_packet.begin(),
                         xcp_packet.end());
    return frame;
}

std::optional<std::vector<UdpFrameView>> decodeUdpDatagram(
    BytesView datagram) noexcept {
    // 空 Datagram 或不足一个 Header：无法解析
    if (datagram.size() < kUdpHeaderSize) {
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
        header.m_len_ =
            static_cast<DatagramLen>(static_cast<std::uint16_t>(p[0]) |
                                     (static_cast<std::uint16_t>(p[1]) << 8));
        header.m_ctr_ =
            static_cast<DatagramCtr>(static_cast<std::uint16_t>(p[2]) |
                                     (static_cast<std::uint16_t>(p[3]) << 8));

        // LEN == 0 非法：每个 Frame 至少携带 1 字节 XCP Packet（PID）
        if (header.m_len_ == 0U) {
            return std::nullopt;
        }
        // LEN 越界：Frame 不得跨 Datagram 边界
        if (static_cast<std::size_t>(header.m_len_) >
            remaining - kUdpHeaderSize) {
            return std::nullopt;
        }

        UdpFrameView view;
        view.m_header_ = header;
        view.m_xcp_packet_ =
            datagram.subspan(offset + kUdpHeaderSize, header.m_len_);
        frames.push_back(view);

        offset += kUdpHeaderSize + header.m_len_;
    }

    // 循环结束时 offset 必然等于 datagram.size()（LEN 精确推进）；
    // 若出现残留字节，上面的 LEN 越界检查已经拦截，这里再做一次显式确认。
    if (offset != datagram.size() || frames.empty()) {
        return std::nullopt;
    }
    return frames;
}

}  // namespace calmcar::xcp
