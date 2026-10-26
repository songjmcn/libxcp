/**
 * @file udp_boundary_framing_test.cpp
 * @brief 校验「以承载单元边界为准」的 UDP 帧解码（P1：不再依赖
 *        PACKET_ALIGNMENT 证据）。
 *
 * 覆盖点：
 *  - 单帧无 Tail、末帧 0..3 字节 FILL 均可解码；
 *  - 4 字节以上残尾、LEN=0、LEN 越界一律整体拒绝；
 *  - 同数据报串联多帧（中间帧无 Tail）可解码；
 *  - 中间帧带 FILL 时整体拒绝（不做试探）；
 *  - 发送侧编码结果恒为 `4 + LEN`，不补 Tail。
 */

#include "libxcp/ethernet_header_codec.hpp"
#include "libxcp/udp_header_codec.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace calmcar::xcp {
namespace {

using Bytes = std::vector<std::uint8_t>;

/// @brief 构造一个 Frame 的字节序列（可选在末尾追加 n 字节 FILL）
[[nodiscard]] Bytes MakeFrame(DatagramCtr ctr, const Bytes& packet,
                              std::uint8_t tail = 0) {
    Bytes out;
    const auto len = static_cast<DatagramLen>(packet.size());
    out.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(ctr & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((ctr >> 8) & 0xFFU));
    out.insert(out.end(), packet.begin(), packet.end());
    out.insert(out.end(), tail, static_cast<std::uint8_t>(0x00));
    return out;
}

/// @brief 拼接两个字节序列
[[nodiscard]] Bytes Concat(const Bytes& lhs, const Bytes& rhs) {
    Bytes out = lhs;
    out.insert(out.end(), rhs.begin(), rhs.end());
    return out;
}

TEST(UdpBoundaryFraming, DecodesSingleFrameWithoutTail) {
    const Bytes datagram = MakeFrame(0x0102, {0xFF, 0x00});
    const auto frames = DecodeEthernetDatagramByBoundary(datagram);

    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames->size(), 1U);
    EXPECT_EQ((*frames)[0].header.len, 2U);
    EXPECT_EQ((*frames)[0].header.ctr, 0x0102U);
    EXPECT_EQ((*frames)[0].tail_bytes, 0U);
    EXPECT_EQ((*frames)[0].xcp_packet.size(), 2U);
}

TEST(UdpBoundaryFraming, AcceptsTailUpToThreeBytesOnLastFrame) {
    // 末帧允许 0..3 字节 FILL：对端按 16/32-bit 对齐补白时不需协商对齐
    const Bytes one = MakeFrame(1U, {0xFF}, 1);
    const Bytes two = MakeFrame(2U, {0xFF}, 2);
    const Bytes three = MakeFrame(3U, {0xFF}, 3);

    const auto f1 = DecodeEthernetDatagramByBoundary(one);
    const auto f2 = DecodeEthernetDatagramByBoundary(two);
    const auto f3 = DecodeEthernetDatagramByBoundary(three);

    ASSERT_TRUE(f1.has_value());
    ASSERT_TRUE(f2.has_value());
    ASSERT_TRUE(f3.has_value());
    EXPECT_EQ((*f1)[0].tail_bytes, 1U);
    EXPECT_EQ((*f2)[0].tail_bytes, 2U);
    EXPECT_EQ((*f3)[0].tail_bytes, 3U);
    // 每个用例都只交付一个 Frame，且 XCP Packet 内容不被 FILL 污染
    EXPECT_EQ((*f3)[0].xcp_packet.size(), 1U);
}

TEST(UdpBoundaryFraming, RejectsTrailingBytesBeyondThree) {
    // 4 字节残尾不可能被解释为 FILL（max_tail=3），只能当作下一帧 Header；
    // 它的 LEN 为 0，因此整体拒绝而不是静默丢字节。
    const Bytes datagram = MakeFrame(1U, {0xFF}, 4);
    EXPECT_FALSE(DecodeEthernetDatagramByBoundary(datagram).has_value());
}

TEST(UdpBoundaryFraming, RejectsZeroLenFrame) {
    const Bytes datagram = {0x00, 0x00, 0x01, 0x00, 0xFF};
    EXPECT_FALSE(DecodeEthernetDatagramByBoundary(datagram).has_value());
}

TEST(UdpBoundaryFraming, RejectsTooShortDatagram) {
    const Bytes datagram = {0x01, 0x00, 0x02};
    EXPECT_FALSE(DecodeEthernetDatagramByBoundary(datagram).has_value());
}

TEST(UdpBoundaryFraming, RejectsTruncatedPayload) {
    // LEN=4 但只跟了 2 字节载荷：Frame 不得跨数据报边界
    const Bytes datagram = {0x04, 0x00, 0x01, 0x00, 0xFF, 0x00};
    EXPECT_FALSE(DecodeEthernetDatagramByBoundary(datagram).has_value());
}

TEST(UdpBoundaryFraming, DecodesConcatenatedFramesWithLastFrameTail) {
    // 中间帧无 Tail、末帧带 2 字节 FILL：真实从站串联多帧时的常见形态
    const Bytes first = MakeFrame(11U, {0xFF, 0x00});
    const Bytes second = MakeFrame(12U, {0xFB}, 2);
    const auto frames = DecodeEthernetDatagramByBoundary(Concat(first, second));

    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames->size(), 2U);
    EXPECT_EQ((*frames)[0].header.ctr, 11U);
    EXPECT_EQ((*frames)[0].tail_bytes, 0U);
    EXPECT_EQ((*frames)[1].header.ctr, 12U);
    EXPECT_EQ((*frames)[1].tail_bytes, 2U);
}

TEST(UdpBoundaryFraming, RejectsMiddleFrameWithTail) {
    // 中间帧若补了 FILL，后面就不在帧边界上；本函数不做试探 → 整体拒绝
    const Bytes first = MakeFrame(21U, {0xFF}, 2);
    const Bytes second = MakeFrame(22U, {0xFE});
    EXPECT_FALSE(
        DecodeEthernetDatagramByBoundary(Concat(first, second)).has_value());
}

TEST(UdpBoundaryFraming, RejectsMaxTailAboveThree) {
    const Bytes datagram = MakeFrame(1U, {0xFF});
    EXPECT_FALSE(DecodeEthernetDatagramByBoundary(datagram, 4).has_value());
}

TEST(UdpBoundaryFraming, SendsWithoutTailRegardlessOfAlignment) {
    // 发送侧契约：CRO 恒为 `4 + LEN`，不因对齐补 Tail
    const Bytes packet = {0xFF, 0x00};
    for (const auto alignment : {PacketAlignment::Bit8, PacketAlignment::Bit16,
                                 PacketAlignment::Bit32}) {
        const auto frame = EncodeUdpFrame(packet, 0x0007);
        EXPECT_EQ(frame.data.size(), kEthernetHeaderSize + packet.size())
            << "alignment=" << static_cast<int>(alignment);
        EXPECT_EQ(frame.tail_bytes, 0U);
    }
}

}  // namespace
}  // namespace calmcar::xcp