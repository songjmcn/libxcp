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
    EXPECT_EQ(frame.data, BytesOf({0x02, 0x00, 0x01, 0x02, 0xFF, 0x00}));
}

TEST(UdpHeaderEncode, CounterWrapsUseFullSixteenBits) {
    const Bytes packet = BytesOf({0xFD, 0x00});
    const auto frame = EncodeUdpFrame(packet, 0xFFFF);
    EXPECT_EQ(frame.data, BytesOf({0x02, 0x00, 0xFF, 0xFF, 0xFD, 0x00}));
}

TEST(UdpHeaderEncode, EmptyPacketProducesLenZeroButIsRejectedOnDecode) {
    // 编码器不禁止空 Packet（LEN=0），但解码器必须拒绝 LEN==0 的 Frame
    const auto frame = EncodeUdpFrame(BytesView{}, 0);
    EXPECT_EQ(frame.data.size(), kUdpHeaderSize);
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{frame.data}).has_value());
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
    EXPECT_EQ(frame.data.size(), kUdpMaxXcpPacket + kUdpHeaderSize);
    const auto decoded = DecodeUdpDatagram(BytesView{frame.data});
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 1U);
    EXPECT_EQ((*decoded)[0].header.len,
              static_cast<DatagramLen>(kUdpMaxXcpPacket));
    EXPECT_EQ((*decoded)[0].xcp_packet.size(), kUdpMaxXcpPacket);
}

// --------------------------------------------------------------------------
// 解码：单 Frame 边界条件
// --------------------------------------------------------------------------

TEST(UdpHeaderDecode, SingleFrameRoundTrip) {
    const Bytes packet =
        BytesOf({0xFF, 0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10});
    const auto frame = EncodeUdpFrame(BytesView{packet}, 42);
    const auto decoded = DecodeUdpDatagram(BytesView{frame.data});
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 1U);
    EXPECT_EQ((*decoded)[0].header.len, 8);
    EXPECT_EQ((*decoded)[0].header.ctr, 42);
    EXPECT_TRUE(std::equal(packet.begin(), packet.end(),
                           (*decoded)[0].xcp_packet.begin()));
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
    EXPECT_EQ((*decoded)[0].header.ctr, 100);
    EXPECT_EQ((*decoded)[0].header.len, 2);
    EXPECT_TRUE(
        std::equal(ev.begin(), ev.end(), (*decoded)[0].xcp_packet.begin()));
    EXPECT_EQ((*decoded)[1].header.ctr, 101);
    EXPECT_EQ((*decoded)[1].header.len, 3);
    EXPECT_TRUE(
        std::equal(res.begin(), res.end(), (*decoded)[1].xcp_packet.begin()));
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
        EXPECT_EQ((*decoded)[i].xcp_packet.size(), packets[i].size());
        EXPECT_TRUE(std::equal(packets[i].begin(), packets[i].end(),
                               (*decoded)[i].xcp_packet.begin()))
            << "第 " << i << " 个 Frame 内容不符";
    }
    // CTR 回绕在连续 Frame 中正确工作
    EXPECT_EQ((*decoded)[2].header.ctr, static_cast<DatagramCtr>(65535U));
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

// ===========================================================================
// T-02：PACKET_ALIGNMENT 强类型与 Tail 公式
// 规范基线 TL V1.5 §4.3.2（原始 PDF 第 9-10 页）
// ===========================================================================

TEST(PacketAlignment, BytesMappingCoversAllCases) {
    // 位宽 -> 字节数：8→1、16→2、32→4
    EXPECT_EQ(AlignmentBytes(PacketAlignment::Bit8), 1U);
    EXPECT_EQ(AlignmentBytes(PacketAlignment::Bit16), 2U);
    EXPECT_EQ(AlignmentBytes(PacketAlignment::Bit32), 4U);
    // Unknown 返回 0 而非 1：0 无法作模数，迫使调用方显式处理未知分支
    EXPECT_EQ(AlignmentBytes(PacketAlignment::Unknown), 0U);
}

TEST(PacketAlignment, ParseAcceptsOnly8_16_32) {
    EXPECT_EQ(ParsePacketAlignment(8), PacketAlignment::Bit8);
    EXPECT_EQ(ParsePacketAlignment(16), PacketAlignment::Bit16);
    EXPECT_EQ(ParsePacketAlignment(32), PacketAlignment::Bit32);
    // 任何非 {8,16,32} 归为 Unknown，绝不猜测成 8
    EXPECT_EQ(ParsePacketAlignment(0), PacketAlignment::Unknown);
    EXPECT_EQ(ParsePacketAlignment(1), PacketAlignment::Unknown);
    EXPECT_EQ(ParsePacketAlignment(64), PacketAlignment::Unknown);
    EXPECT_EQ(ParsePacketAlignment(255), PacketAlignment::Unknown);
}

TEST(PacketAlignment, TailBytesFollowSpecFormula) {
    // 规范 §4.3.2.2：tail 为 0..3 字节
    // 8-bit：恒为 0（任何长度都已对齐）
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Bit8, 5U), 0U);
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Bit8, 44U), 0U);
    // 16-bit：无 tail 帧长 5 -> 补 1 到 6
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Bit16, 5U), 1U);
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Bit16, 6U), 0U);
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Bit16, 44U), 0U);
    // 32-bit：无 tail 帧长 5 -> 补 3 到 8；7 -> 补 1 到 8
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Bit32, 5U), 3U);
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Bit32, 7U), 1U);
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Bit32, 8U), 0U);
    // Unknown：无对齐依据，返回 0（调用方须自行拒绝该能力）
    EXPECT_EQ(TailBytesForAlignment(PacketAlignment::Unknown, 5U), 0U);
}

// ===========================================================================
// T-03：按 Alignment 编码，Tail 必须真实写入
// ===========================================================================

TEST(UdpAlignedEncode, Bit8HasNoTail) {
    const auto packet = BytesOf({0xFF, 0x01, 0x02});
    const auto frame = EncodeUdpFrameAligned(packet, 0x0007,
                                             PacketAlignment::Bit8);
    EXPECT_EQ(frame.tail_bytes, 0U);
    // LEN(2) + CTR(2) + Packet(3) = 7
    EXPECT_EQ(frame.data.size(), 7U);
    EXPECT_EQ(frame.data[0], 0x03U);  // LEN 低字节 = 原 XCP Packet 长度
    EXPECT_EQ(frame.data[1], 0x00U);
    EXPECT_EQ(frame.data[2], 0x07U);
    EXPECT_EQ(frame.data[3], 0x00U);
}

TEST(UdpAlignedEncode, Bit16AppendsTailFilledWithZero) {
    const auto packet = BytesOf({0xFF, 0x01, 0x02});
    const auto frame = EncodeUdpFrameAligned(packet, 0x0007,
                                             PacketAlignment::Bit16);
    // 无 tail 帧长 = 4 + 3 = 7；16-bit 对齐补 1 字节
    EXPECT_EQ(frame.tail_bytes, 1U);
    EXPECT_EQ(frame.data.size(), 8U);
    // LEN 仍只计原 Packet，**不含** Tail（TL V1.5 §4.3.1.1）
    EXPECT_EQ(frame.data[0], 0x03U);
    EXPECT_EQ(frame.data[1], 0x00U);
    // FILL 内容规范定为 "do not care"，本库固定 0x00
    EXPECT_EQ(frame.data[7], 0x00U);
}

TEST(UdpAlignedEncode, Bit32AppendsCorrectTail) {
    const auto packet = BytesOf({0xFF, 0x01, 0x02});
    const auto frame = EncodeUdpFrameAligned(packet, 0x0007,
                                             PacketAlignment::Bit32);
    // 无 tail 帧长 7；32-bit 对齐补 1 到 8
    EXPECT_EQ(frame.tail_bytes, 1U);
    EXPECT_EQ(frame.data.size(), 8U);
    EXPECT_EQ(frame.data[0], 0x03U);  // LEN 不含 Tail

    // 无 tail 帧长 5（packet 1 字节）-> 补 3 到 8
    const auto packet1 = BytesOf({0xFF});
    const auto frame1 = EncodeUdpFrameAligned(packet1, 0x0001,
                                              PacketAlignment::Bit32);
    EXPECT_EQ(frame1.tail_bytes, 3U);
    EXPECT_EQ(frame1.data.size(), 8U);
}

TEST(UdpAlignedEncode, UnknownAlignmentIsRejectedNotAssumed) {
    const auto packet = BytesOf({0xFF});
    // Unknown 必须抛异常：不得静默按 8-bit 编码（方案 §1.9A.4）
    EXPECT_THROW((void)EncodeUdpFrameAligned(packet, 0x0001,
                                             PacketAlignment::Unknown),
                 XcpException);
}

TEST(UdpAlignedEncode, LegacyEncodeIsExactlyBit8) {
    const auto packet = BytesOf({0xFF, 0x01, 0x02});
    const auto legacy = EncodeUdpFrame(packet, 0x0007);
    const auto aligned = EncodeUdpFrameAligned(packet, 0x0007,
                                               PacketAlignment::Bit8);
    // 旧入口语义必须与新入口的 Bit8 完全一致
    EXPECT_EQ(legacy.data, aligned.data);
    EXPECT_EQ(legacy.tail_bytes, 0U);
}

// ===========================================================================
// T-04：按 Alignment 解码，跳过 Tail 并整体拒绝畸形输入
// ===========================================================================

TEST(UdpAlignedDecode, SkipsTailAndParsesAllFrames) {
    // 用编码器生成一个 16-bit 对齐的双 Frame Datagram
    const auto f1 = EncodeUdpFrameAligned(BytesOf({0xFF, 0x01}), 0x0001,
                                          PacketAlignment::Bit16);
    const auto f2 = EncodeUdpFrameAligned(BytesOf({0xFE, 0x02, 0x03}), 0x0002,
                                          PacketAlignment::Bit16);
    Bytes datagram = f1.data;
    datagram.insert(datagram.end(), f2.data.begin(), f2.data.end());

    const auto frames =
        DecodeUdpDatagramAligned(BytesView{datagram}, PacketAlignment::Bit16);
    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames->size(), 2U);
    // 两个 Packet 必须被原样取回，Tail 不得混入
    EXPECT_EQ((*frames)[0].xcp_packet.size(), 2U);
    EXPECT_EQ((*frames)[0].xcp_packet[0], 0xFFU);
    EXPECT_EQ((*frames)[0].header.ctr, 0x0001U);
    EXPECT_EQ((*frames)[1].xcp_packet.size(), 3U);
    EXPECT_EQ((*frames)[1].xcp_packet[0], 0xFEU);
    EXPECT_EQ((*frames)[1].header.ctr, 0x0002U);
    // Tail 计数应被如实报告
    EXPECT_EQ((*frames)[0].tail_bytes, f1.tail_bytes);
    EXPECT_EQ((*frames)[1].tail_bytes, f2.tail_bytes);
}

TEST(UdpAlignedDecode, Bit32RoundTripThreeFrames) {
    Bytes datagram;
    const std::vector<Bytes> packets = {
        BytesOf({0xFF}), BytesOf({0xFE, 0x01, 0x02}), BytesOf({0xFD, 0x03})};
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto f = EncodeUdpFrameAligned(
            BytesView{packets[i]}, static_cast<DatagramCtr>(i + 1),
            PacketAlignment::Bit32);
        datagram.insert(datagram.end(), f.data.begin(), f.data.end());
    }
    const auto frames =
        DecodeUdpDatagramAligned(BytesView{datagram}, PacketAlignment::Bit32);
    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames->size(), 3U);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        EXPECT_EQ((*frames)[i].xcp_packet.size(), packets[i].size());
        EXPECT_EQ((*frames)[i].header.ctr, static_cast<DatagramCtr>(i + 1));
    }
}

TEST(UdpAlignedDecode, TruncatedTailRejectsWholeDatagram) {
    // 16-bit 对齐下造一个**确实带 Tail** 的 Frame，再砍掉 Tail -> 整体拒绝
    // 注意尺寸选择：packet 必须使 4 + size 为奇数，否则 16-bit 的 tail 为 0，
    // 这个用例就退化成"合法帧"了（3 字节 packet -> 无 tail 帧长 7 -> tail 1）
    const auto f = EncodeUdpFrameAligned(BytesOf({0xFF, 0x01, 0x02}), 0x0001,
                                         PacketAlignment::Bit16);
    ASSERT_EQ(f.tail_bytes, 1U) << "本用例需要非零 Tail，见尺寸选择说明";
    Bytes truncated(f.data.begin(), f.data.end() - f.tail_bytes);
    EXPECT_FALSE(
        DecodeUdpDatagramAligned(BytesView{truncated}, PacketAlignment::Bit16)
            .has_value());
}

TEST(UdpAlignedDecode, UnknownAlignmentRejectsWholeDatagram) {
    const auto f = EncodeUdpFrame(BytesOf({0xFF, 0x01}), 0x0001);
    // Unknown 无法推出 Tail 长度 -> 整体拒绝，不按 8-bit 猜测
    EXPECT_FALSE(DecodeUdpDatagramAligned(BytesView{f.data},
                                         PacketAlignment::Unknown)
                     .has_value());
}

TEST(UdpAlignedDecode, LegacyDecodeIsExactlyBit8) {
    const auto f = EncodeUdpFrame(BytesOf({0xFF, 0x01, 0x02}), 0x0009);
    const auto legacy = DecodeUdpDatagram(BytesView{f.data});
    const auto aligned = DecodeUdpDatagramAligned(BytesView{f.data},
                                                  PacketAlignment::Bit8);
    ASSERT_TRUE(legacy.has_value());
    ASSERT_TRUE(aligned.has_value());
    EXPECT_EQ(legacy->size(), aligned->size());
    EXPECT_EQ((*legacy)[0].xcp_packet.size(), (*aligned)[0].xcp_packet.size());
}

TEST(UdpAlignedDecode, Bit8TreatsTailBytesAsResidue) {
    // 8-bit 语义下，多出来的 FILL 字节就是"残留字节"，必须整体拒绝。
    // 尺寸选择：packet 3 字节 -> 16-bit 下 tail=1（非零），
    // 这样该 Datagram 才是真正的 16-bit 布局，用 8-bit 解才会看到残留。
    const auto f = EncodeUdpFrameAligned(BytesOf({0xFF, 0x01, 0x02}), 0x0001,
                                         PacketAlignment::Bit16);
    ASSERT_EQ(f.tail_bytes, 1U) << "本用例需要非零 Tail，见尺寸选择说明";
    EXPECT_FALSE(DecodeUdpDatagram(BytesView{f.data}).has_value());
}

// ===========================================================================
// T-05：A2L PACKET_ALIGNMENT 贯通入口
// ===========================================================================

TEST(PacketAlignment, A2lRawBitsFlowThrough) {
    // 上游 a2lbridge 的 packet_alignment 字段为位宽（8/16/32，0=未知）；
    // 这里固化"原始值 -> 强类型 -> 字节数"的贯通语义
    struct Case {
        std::uint8_t raw;
        PacketAlignment expected;
        std::uint8_t bytes;
    };
    const Case cases[] = {
        {8, PacketAlignment::Bit8, 1},
        {16, PacketAlignment::Bit16, 2},
        {32, PacketAlignment::Bit32, 4},
        {0, PacketAlignment::Unknown, 0},
    };
    for (const auto& c : cases) {
        const auto parsed = ParsePacketAlignment(c.raw);
        EXPECT_EQ(parsed, c.expected);
        EXPECT_EQ(AlignmentBytes(parsed), c.bytes);
    }
}

}  // namespace
}  // namespace calmcar::xcp
