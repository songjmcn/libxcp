/**
 * @file udp_header_codec_test.cpp
 * @brief UdpHeaderCodec 黄金向量与多 Frame Datagram 解析测试（设计 §15.3 第 1
 * 条）。
 */

#include "libxcp/udp_header_codec.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

/// @brief 构造测试字节序列
Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 手工拼一个 Frame：LEN(u16le) + CTR(u16le) + Packet
Bytes RawFrame(std::uint16_t len, std::uint16_t ctr, BytesView packet) {
    Bytes out;
    out.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(ctr & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((ctr >> 8) & 0xFFU));
    out.insert(out.end(), packet.begin(), packet.end());
    return out;
}

// --------------------------------------------------------------------------
// 编码：LEN/CTR 恒为小端，与 Session Byte Order 无关
// --------------------------------------------------------------------------

TEST(UdpHeaderEncode, GoldenSingleFrame) {
    const Bytes packet = BytesOf({0xFF, 0x00});
    const auto frame = EncodeUdpFrame(packet, 0x0201);
    // LEN=2(小端 02 00), CTR=0x0201(小端 01 02), 然后原样 Packet
    EXPECT_EQ(frame.m_data_, BytesOf({0x02, 0x00, 0x01, 0x02, 0xFF, 0x00}));
}

TEST(UdpHeaderEncode, CounterWrapsUseFullSixteenBits) {
    const Bytes packet = BytesOf({0xFD, 0x00});
    const auto frame = EncodeUdpFrame(packet, 0xFFFF);
    EXPECT_EQ(frame.m_data_, BytesOf({0x02, 0x00, 0xFF, 0xFF, 0xFD, 0x00}));
}

TEST(UdpHeaderEncode, EmptyPacketProducesLenZeroButIsRejectedOnDecode) {
    // 编码器不禁止空 Packet（LEN=0），但解码器必须拒绝 LEN==0 的 Frame
    const auto frame = EncodeUdpFrame(BytesView{}, 0);
    EXPECT_EQ(frame.m_data_.size(), kUdpHeaderSize);
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{frame.m_data_}).has_value());
}

TEST(UdpHeaderEncode, RejectsOversizedPacket) {
    Bytes huge(kUdpMaxXcpPacket + 1, 0x00);
    EXPECT_THROW((void)EncodeUdpFrame(BytesView{huge}, 0), XcpException);
    try {
        (void)EncodeUdpFrame(BytesView{huge}, 0);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

TEST(UdpHeaderEncode, MaxAllowedPacketAccepted) {
    Bytes big(kUdpMaxXcpPacket, 0xAB);
    const auto frame = EncodeUdpFrame(BytesView{big}, 7);
    EXPECT_EQ(frame.m_data_.size(), kUdpMaxXcpPacket + kUdpHeaderSize);
    const auto decoded = DecodeUdpDatagram(BytesView{frame.m_data_});
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 1U);
    EXPECT_EQ((*decoded)[0].m_header_.m_len_,
              static_cast<DatagramLen>(kUdpMaxXcpPacket));
    EXPECT_EQ((*decoded)[0].m_xcp_packet_.size(), kUdpMaxXcpPacket);
}

// --------------------------------------------------------------------------
// 解码：单 Frame 边界条件
// --------------------------------------------------------------------------

TEST(UdpHeaderDecode, SingleFrameRoundTrip) {
    const Bytes packet =
        BytesOf({0xFF, 0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10});
    const auto frame = EncodeUdpFrame(BytesView{packet}, 42);
    const auto decoded = DecodeUdpDatagram(BytesView{frame.m_data_});
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 1U);
    EXPECT_EQ((*decoded)[0].m_header_.m_len_, 8);
    EXPECT_EQ((*decoded)[0].m_header_.m_ctr_, 42);
    EXPECT_TRUE(std::equal(packet.begin(), packet.end(),
                           (*decoded)[0].m_xcp_packet_.begin()));
}

TEST(UdpHeaderDecode, EmptyDatagramRejected) {
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{}).has_value());
}

TEST(UdpHeaderDecode, TruncatedHeadersRejected) {
    for (std::size_t n = 0; n < kUdpHeaderSize; ++n) {
        Bytes buf(4, 0x00);
        EXPECT_FALSE(DecodeUdpDatagram(BytesView{buf}.first(n)).has_value())
            << "长度 " << n;
    }
}

TEST(UdpHeaderDecode, LenBeyondDatagramRejected) {
    // 声明 LEN=10 但实际只有 2 字节数据
    const Bytes packet = BytesOf({0xFF, 0x00});
    const Bytes bad = RawFrame(10, 0, BytesView{packet});
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{bad}).has_value());
}

TEST(UdpHeaderDecode, ZeroLenRejected) {
    const Bytes bad = RawFrame(0, 0, BytesView{});
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{bad}).has_value());
}

TEST(UdpHeaderDecode, TrailingResidueRejected) {
    // 一个完整 Frame 之后多出 1 个残留字节 -> 整体失败（原子性策略）
    const Bytes packet = BytesOf({0xFC, 0x00});
    Bytes bad = RawFrame(2, 0, BytesView{packet});
    bad.push_back(0xEE);
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{bad}).has_value());
}

TEST(UdpHeaderDecode, CorruptSecondFrameKillsWholeDatagram) {
    // 设计 §15.3 第 4 条：第二个 Frame LEN 损坏时整个 Datagram 不得交付
    const Bytes first = BytesOf({0xFD, 0x05});
    const Bytes second = BytesOf({0xFF, 0xE0});
    Bytes datagram = RawFrame(2, 0, BytesView{first});
    Bytes broken_second = RawFrame(99, 1, BytesView{second});  // LEN 越界
    datagram.insert(datagram.end(), broken_second.begin(), broken_second.end());
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{datagram}).has_value());
}

// --------------------------------------------------------------------------
// 多 Frame 打包（XCP 1.1 Part 3 §1.3.1）
// --------------------------------------------------------------------------

TEST(UdpHeaderDecode, TwoFramesParsedInOrder) {
    const Bytes ev = BytesOf({0xFD, 0x05});
    const Bytes res = BytesOf({0xFF, 0xE0, 0x11});
    Bytes datagram = RawFrame(2, 100, BytesView{ev});
    auto f2 = RawFrame(3, 101, BytesView{res});
    datagram.insert(datagram.end(), f2.begin(), f2.end());

    const auto decoded = DecodeUdpDatagram(BytesView{datagram});
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 2U);
    EXPECT_EQ((*decoded)[0].m_header_.m_ctr_, 100);
    EXPECT_EQ((*decoded)[0].m_header_.m_len_, 2);
    EXPECT_TRUE(
        std::equal(ev.begin(), ev.end(), (*decoded)[0].m_xcp_packet_.begin()));
    EXPECT_EQ((*decoded)[1].m_header_.m_ctr_, 101);
    EXPECT_EQ((*decoded)[1].m_header_.m_len_, 3);
    EXPECT_TRUE(std::equal(res.begin(), res.end(),
                           (*decoded)[1].m_xcp_packet_.begin()));
}

TEST(UdpHeaderDecode, ThreeFramesWithVariedLengths) {
    std::vector<Bytes> packets{BytesOf({0xFE, 0x20}), BytesOf({0xFF}),
                               BytesOf({0xFC, 0x01, 0x02, 0x03})};
    Bytes datagram;
    std::uint16_t ctr = 65533;
    for (const auto& p : packets) {
        auto f =
            RawFrame(static_cast<std::uint16_t>(p.size()), ctr++, BytesView{p});
        datagram.insert(datagram.end(), f.begin(), f.end());
    }
    const auto decoded = DecodeUdpDatagram(BytesView{datagram});
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 3U);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ((*decoded)[i].m_xcp_packet_.size(), packets[i].size());
        EXPECT_TRUE(std::equal(packets[i].begin(), packets[i].end(),
                               (*decoded)[i].m_xcp_packet_.begin()))
            << "第 " << i << " 个 Frame 内容不符";
    }
    // CTR 回绕在连续 Frame 中正确工作
    EXPECT_EQ((*decoded)[2].m_header_.m_ctr_, static_cast<DatagramCtr>(65535U));
}

TEST(UdpHeaderDecode, FrameMustNotCrossDatagramBoundary) {
    // 把一个 Frame 拆成两段发送：每段单独解码都必须失败
    const Bytes packet = BytesOf({0xFF, 0x01, 0x02, 0x03, 0x04});
    const Bytes whole = RawFrame(5, 0, BytesView{packet});
    const Bytes first_half = Bytes(whole.begin(), whole.begin() + 6);
    const Bytes second_half = Bytes(whole.begin() + 6, whole.end());
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{first_half}).has_value());
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{second_half}).has_value());
}

TEST(UdpHeaderConstants, LimitsMatchIpv4Udp) {
    EXPECT_EQ(kUdpHeaderSize, 4U);
    EXPECT_EQ(kUdpMaxDatagramSize, 65507U);
    EXPECT_EQ(kUdpMaxXcpPacket, 65503U);
}

}  // namespace
}  // namespace calmcar::xcp
