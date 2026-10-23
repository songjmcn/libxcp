/**
 * @file ethernet_header_codec_test.cpp
 * @brief 以太网通用 Header/Tail 编解码（`ethernet_header_codec.hpp`）与
 * 协议层能力集（`CapabilitySetFor`）的测试，覆盖 B-T1 §4.1 / §4.3。
 *
 * @par 本文件锁定的关键契约
 *   1. 通用层 LEN 上界为 0xFFFF（**不是** UDP 的 65503）；
 *   2. `EncodeEthernetFrameUnpadded()` 编出的 Frame 严格
 *      `Header + Packet`，无任何 Tail —— AsamV14 Profile 的 wire 契约；
 *   3. `EncodeEthernetFrameAligned()` 与 UDP 路径在同参数下逐字节一致
 *      （迁移未改变行为）；
 *   4. `DecodeEthernetDatagramAligned()` 对畸形输入**整体拒绝**；
 *   5. 能力集随版本单调增长，且无效版本返回全 false 的保守集。
 */

#include "libxcp/ethernet_header_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/protocol_version.hpp"
#include "libxcp/udp_header_codec.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 构造测试字节序列
Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 比较只读字节视图与期望序列
/// @param view 待比较的视图（span 无常用的 operator==，故逐元素比较）
/// @param expected 期望字节序列
/// @return 长度与内容均相同返回 true
bool ViewEquals(BytesView view, const Bytes& expected) {
    return view.size() == expected.size() &&
           std::equal(view.begin(), view.end(), expected.begin());
}

// --------------------------------------------------------------------------
// 常量：通用层上界与 UDP 专用上界必须彼此独立
// --------------------------------------------------------------------------

TEST(EthernetHeaderConstants, GenericPacketLimitIsSixteenBitField) {
    // LEN 是 16 位字段，通用层允许到 65535
    EXPECT_EQ(kEthernetMaxXcpPacket, 0xFFFFU);
    // 不得把 UDP 的 IPv4 物理限制误当作通用层上界
    EXPECT_GT(kEthernetMaxXcpPacket, kUdpMaxXcpPacket);
    EXPECT_EQ(kUdpMaxXcpPacket, 65503U);
}

TEST(EthernetHeaderConstants, HeaderSizeAndAlignmentBounds) {
    EXPECT_EQ(kEthernetHeaderSize, 4U);
    EXPECT_EQ(kEthernetMaxAlignmentBytes, 4U);
    // 容灾上界 = Header + 最大 Packet + 最大 Tail
    EXPECT_EQ(kEthernetMaxFrameSize, 4U + 0xFFFFU + 4U);
}

TEST(EthernetHeaderConstants, UdpAliasesPointToGenericTypes) {
    // 兼容别名必须与通用类型同一（可互相赋值），确保旧调用点零改动
    const UdpFrame udp_frame =
        EncodeEthernetFrameUnpadded(BytesOf({0xFF, 0x00}), 0x0001);
    const EthernetFrame generic = udp_frame;
    EXPECT_EQ(generic.data, udp_frame.data);
    EXPECT_EQ(kUdpHeaderSize, kEthernetHeaderSize);
}

// --------------------------------------------------------------------------
// 编码：AsamV15 带 Tail / AsamV14 无 Tail
// --------------------------------------------------------------------------

TEST(EthernetFrameEncodeAligned, Bit8ProducesNoTail) {
    const Bytes packet = BytesOf({0xFF, 0x00});
    const auto frame =
        EncodeEthernetFrameAligned(packet, 0x0201, PacketAlignment::Bit8);
    // 8-bit 对齐下 tail 恒为 0：Header(4) + Packet(2) = 6 字节
    EXPECT_EQ(frame.tail_bytes, 0U);
    EXPECT_EQ(frame.data, BytesOf({0x02, 0x00, 0x01, 0x02, 0xFF, 0x00}));
}

TEST(EthernetFrameEncodeAligned, Bit32PadsTailToAlignment) {
    // Header(4) + Packet(2) = 6 -> 对齐到 4 的倍数需补 2 字节
    const Bytes packet = BytesOf({0xAA, 0xBB});
    const auto frame =
        EncodeEthernetFrameAligned(packet, 7, PacketAlignment::Bit32);
    EXPECT_EQ(frame.tail_bytes, 2U);
    EXPECT_EQ(frame.data.size(), 8U);
    // FILL 内容规范定为 "do not care"，本库固定 0x00
    EXPECT_EQ(frame.data,
              BytesOf({0x02, 0x00, 0x07, 0x00, 0xAA, 0xBB, 0x00, 0x00}));
}

TEST(EthernetFrameEncodeAligned, RejectsUnknownAlignment) {
    // 无证据时不得假定对齐，必须拒绝而非静默按 8-bit 处理
    EXPECT_THROW((void)EncodeEthernetFrameAligned(BytesOf({0x01}), 0,
                                                  PacketAlignment::Unknown),
                 XcpException);
}

TEST(EthernetFrameEncodeAligned, RejectsPacketLargerThanSixteenBitField) {
    // 通用层允许到 0xFFFF，超出即无法编入 LEN
    const Bytes too_big(0x10000U, 0x00);
    EXPECT_THROW(
        (void)EncodeEthernetFrameAligned(too_big, 0, PacketAlignment::Bit8),
        XcpException);
}

TEST(EthernetFrameEncodeUnpadded, HasStrictlyNoTailEvenAtBit32) {
    // AsamV14 契约：无论 alignment 为何，wire 形式恒为 Header + Packet
    const Bytes packet = BytesOf({0xAA, 0xBB});
    const auto frame = EncodeEthernetFrameUnpadded(packet, 7);
    EXPECT_EQ(frame.tail_bytes, 0U);
    EXPECT_EQ(frame.data.size(), kEthernetHeaderSize + packet.size());
    EXPECT_EQ(frame.data, BytesOf({0x02, 0x00, 0x07, 0x00, 0xAA, 0xBB}));
}

TEST(EthernetFrameEncodeUnpadded, MatchesAlignedBit8ByteForByte) {
    // 数值上二者在 8-bit 对齐时等价，这是 AsamV14 与 AsamV15 可共存的前提
    const Bytes packet = BytesOf({0xFD, 0x00, 0x11});
    const auto unpadded = EncodeEthernetFrameUnpadded(packet, 0x1234);
    const auto aligned =
        EncodeEthernetFrameAligned(packet, 0x1234, PacketAlignment::Bit8);
    EXPECT_EQ(unpadded.data, aligned.data);
}

TEST(EthernetFrameEncodeUnpadded, RejectsPacketLargerThanSixteenBitField) {
    const Bytes too_big(0x10000U, 0x00);
    EXPECT_THROW((void)EncodeEthernetFrameUnpadded(too_big, 0), XcpException);
}

// --------------------------------------------------------------------------
// 解码：多 Frame 串联与整体拒绝
// --------------------------------------------------------------------------

TEST(EthernetDatagramDecode, ParsesMultipleFramesWithPerFrameHeader) {
    // 两个 Frame 串联，各自携带独立 LEN/CTR
    Bytes datagram = BytesOf({0x02, 0x00, 0x01, 0x00, 0xFF, 0x00});
    const Bytes second = BytesOf({0x01, 0x00, 0x02, 0x00, 0xFD});
    datagram.insert(datagram.end(), second.begin(), second.end());

    const auto frames =
        DecodeEthernetDatagramAligned(datagram, PacketAlignment::Bit8);
    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames->size(), 2U);
    EXPECT_EQ((*frames)[0].header.ctr, 0x0001U);
    EXPECT_TRUE(ViewEquals((*frames)[0].xcp_packet, BytesOf({0xFF, 0x00})));
    EXPECT_EQ((*frames)[1].header.ctr, 0x0002U);
    EXPECT_TRUE(ViewEquals((*frames)[1].xcp_packet, BytesOf({0xFD})));
}

TEST(EthernetDatagramDecode, SkipsTailAccordingToAlignment) {
    // 第一帧：Header(4) + Packet(2) = 6，Bit32 下 tail=(4-6%4)%4=2
    //         -> 帧长 8，第二 Frame 从偏移 8 开始
    // 第二帧：Header(4) + Packet(1) = 5，tail=(4-5%4)%4=3
    //         -> 帧长 8；本库对所有 Frame 统一补 Tail（含末帧），
    //            由既有 UDP 黄金向量与服务端行为共同固定
    Bytes datagram =
        BytesOf({0x02, 0x00, 0x01, 0x00, 0xAA, 0xBB, 0x00, 0x00});
    const Bytes second =
        BytesOf({0x01, 0x00, 0x02, 0x00, 0xCC, 0x00, 0x00, 0x00});
    datagram.insert(datagram.end(), second.begin(), second.end());

    const auto frames =
        DecodeEthernetDatagramAligned(datagram, PacketAlignment::Bit32);
    ASSERT_TRUE(frames.has_value());
    ASSERT_EQ(frames->size(), 2U);
    EXPECT_EQ((*frames)[0].tail_bytes, 2U);
    EXPECT_EQ((*frames)[1].tail_bytes, 3U);
    EXPECT_TRUE(ViewEquals((*frames)[1].xcp_packet, BytesOf({0xCC})));
}

TEST(EthernetDatagramDecode, RejectsTruncatedFrameWholesale) {
    // 声明 LEN=4 但只给了 3 字节 Packet：必须整体拒绝，不交付部分 Frame
    const Bytes datagram =
        BytesOf({0x04, 0x00, 0x01, 0x00, 0xAA, 0xBB, 0xCC});
    EXPECT_FALSE(
        DecodeEthernetDatagramAligned(datagram, PacketAlignment::Bit8)
            .has_value());
}

TEST(EthernetDatagramDecode, RejectsZeroLenFrame) {
    // LEN==0 无意义：每个 Frame 至少携带 1 字节 PID
    const Bytes datagram = BytesOf({0x00, 0x00, 0x01, 0x00});
    EXPECT_FALSE(
        DecodeEthernetDatagramAligned(datagram, PacketAlignment::Bit8)
            .has_value());
}

TEST(EthernetDatagramDecode, RejectsUnknownAlignment) {
    const Bytes datagram = BytesOf({0x01, 0x00, 0x01, 0x00, 0xFF});
    EXPECT_FALSE(
        DecodeEthernetDatagramAligned(datagram, PacketAlignment::Unknown)
            .has_value());
}

TEST(EthernetDatagramDecode, RejectsTooShortInput) {
    // 不足一个 Header
    EXPECT_FALSE(DecodeEthernetDatagramAligned(BytesOf({0x01, 0x00}),
                                               PacketAlignment::Bit8)
                     .has_value());
}

// --------------------------------------------------------------------------
// 迁移一致性：通用层与 UDP 路径必须逐字节一致
// --------------------------------------------------------------------------

TEST(EthernetCodecMigrationParity, AlignedEncodingMatchesUdpPath) {
    const Bytes packet = BytesOf({0x01, 0x02, 0x03, 0x04, 0x05});
    for (const auto alignment : {PacketAlignment::Bit8, PacketAlignment::Bit16,
                                 PacketAlignment::Bit32}) {
        const auto generic =
            EncodeEthernetFrameAligned(packet, 0x0A0B, alignment);
        const auto udp = EncodeUdpFrameAligned(packet, 0x0A0B, alignment);
        EXPECT_EQ(generic.data, udp.data) << "alignment 一致性被破坏";
        EXPECT_EQ(generic.tail_bytes, udp.tail_bytes);
    }
}

TEST(EthernetCodecMigrationParity, DecodingMatchesUdpPath) {
    // 帧1：Packet(2) -> tail=2；帧2：Packet(1) -> tail=3（Bit32 统一补尾）
    const Bytes datagram = BytesOf({0x02, 0x00, 0x01, 0x00, 0xFF, 0x00, 0x00, 0x00,
                                    0x01, 0x00, 0x02, 0x00, 0xFD, 0x00, 0x00,
                                    0x00});
    const auto generic =
        DecodeEthernetDatagramAligned(datagram, PacketAlignment::Bit32);
    const auto udp = DecodeUdpDatagramAligned(datagram, PacketAlignment::Bit32);
    ASSERT_TRUE(generic.has_value());
    ASSERT_TRUE(udp.has_value());
    ASSERT_EQ(generic->size(), udp->size());
    for (std::size_t i = 0; i < generic->size(); ++i) {
        EXPECT_TRUE(ViewEquals((*generic)[i].xcp_packet,
                               Bytes((*udp)[i].xcp_packet.begin(),
                                     (*udp)[i].xcp_packet.end())));
        EXPECT_EQ((*generic)[i].header.ctr, (*udp)[i].header.ctr);
    }
}

TEST(EthernetCodecMigrationParity, UdpStillRejectsPacketAboveItsOwnLimit) {
    // UDP 专属上界（65503）必须仍然生效，不得被通用层 65535 取代
    const Bytes above_udp_limit(kUdpMaxXcpPacket + 1, 0x00);
    EXPECT_THROW((void)EncodeUdpFrame(above_udp_limit, 0), XcpException);
}

// --------------------------------------------------------------------------
// §4.4 Gate：LEN 边界值（1 / 65503 / 65504 / 65535 / 65536）
// --------------------------------------------------------------------------

TEST(EthernetGateLenBoundaries, LenOneIsAccepted) {
    const Bytes packet = BytesOf({0xFF});
    const auto frame =
        EncodeEthernetFrameAligned(packet, 0x0001, PacketAlignment::Bit8);
    EXPECT_EQ(frame.data.size(), kEthernetHeaderSize + 1U);
    EXPECT_EQ(frame.data[0], 0x01U);
    EXPECT_EQ(frame.data[1], 0x00U);
}

TEST(EthernetGateLenBoundaries, UdpLimitBoundary65503AcceptedByBothLayers) {
    // 65503 是 UDP 的上界，也是通用层的合法值
    const Bytes packet(kUdpMaxXcpPacket, 0xAB);
    const auto udp = EncodeUdpFrame(packet, 0x0001);
    const auto generic =
        EncodeEthernetFrameAligned(packet, 0x0001, PacketAlignment::Bit8);
    EXPECT_EQ(udp.data, generic.data);
}

TEST(EthernetGateLenBoundaries, Len65504AcceptedByGenericButRejectedByUdp) {
    // 65504 已超出 UDP 的 IPv4 容量，但仍是合法的通用层长度
    const Bytes packet(kUdpMaxXcpPacket + 1U, 0xAB);
    EXPECT_THROW((void)EncodeUdpFrame(packet, 0), XcpException);
    const auto generic =
        EncodeEthernetFrameAligned(packet, 0, PacketAlignment::Bit8);
    EXPECT_EQ(generic.data.size(), kEthernetHeaderSize + 65504U);
}

TEST(EthernetGateLenBoundaries, Len65535IsMaxAccepted) {
    const Bytes packet(kEthernetMaxXcpPacket, 0xCD);
    const auto frame =
        EncodeEthernetFrameAligned(packet, 0, PacketAlignment::Bit8);
    EXPECT_EQ(frame.data.size(), kEthernetHeaderSize + 0xFFFFU);
    // LEN 字段应为 0xFFFF 的小端形式
    EXPECT_EQ(frame.data[0], 0xFFU);
    EXPECT_EQ(frame.data[1], 0xFFU);
}

TEST(EthernetGateLenBoundaries, Len65536IsRejected) {
    const Bytes packet(0x10000U, 0xCD);
    EXPECT_THROW(
        (void)EncodeEthernetFrameAligned(packet, 0, PacketAlignment::Bit8),
        XcpException);
}

TEST(EthernetGateLenBoundaries, MaxPacketWithBit32AlignmentStaysInBounds) {
    // 计划 §4.4：65535 + 32-bit alignment 总长不越界
    const Bytes packet(kEthernetMaxXcpPacket, 0xEF);
    const auto frame =
        EncodeEthernetFrameAligned(packet, 0, PacketAlignment::Bit32);
    // 4 + 65535 = 65539；Bit32 下 rem = 65539 % 4 = 3 -> tail = 1
    EXPECT_EQ(frame.tail_bytes, 1U);
    EXPECT_EQ(frame.data.size(), kEthernetHeaderSize + 0xFFFFU + 1U);
    // 总长必须落在容灾上界内
    EXPECT_LE(frame.data.size(), kEthernetMaxFrameSize);

    // 同一 Frame 必须能原样解回，且不越界
    const auto decoded =
        DecodeEthernetDatagramAligned(BytesView{frame.data},
                                      PacketAlignment::Bit32);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 1U);
    EXPECT_EQ((*decoded)[0].xcp_packet.size(), kEthernetMaxXcpPacket);
}

// --------------------------------------------------------------------------
// §4.3 能力集：随版本单调增长，无效版本全 false
// --------------------------------------------------------------------------

TEST(CapabilitySetFor, InvalidVersionYieldsNothing) {
    // 无版本证据（Major 为 0）时不得假定任何能力
    const CapabilitySet caps = CapabilitySetFor(XcpProtocolVersion{0, 0});
    EXPECT_FALSE(caps.get_version);
    EXPECT_FALSE(caps.level1_commands);
    EXPECT_FALSE(caps.extended_daq_queries);
    EXPECT_FALSE(caps.daq_packed_mode);
    EXPECT_FALSE(caps.optional_tl_subcommands);
}

TEST(CapabilitySetFor, V12HasGetVersionAndLevel1) {
    // XCPlite 上报 PL 0x0102：必须可用 GET_VERSION（版本发现的唯一权威手段）
    const CapabilitySet caps = CapabilitySetFor(kProtocolVersion12);
    EXPECT_TRUE(caps.get_version);
    EXPECT_TRUE(caps.level1_commands);
    // 但 V1.3 起才有的扩展查询不得假定具备
    EXPECT_FALSE(caps.extended_daq_queries);
    EXPECT_FALSE(caps.daq_packed_mode);
}

TEST(CapabilitySetFor, V13AddsExtendedDaqQueries) {
    const CapabilitySet caps = CapabilitySetFor(kProtocolVersion13);
    EXPECT_TRUE(caps.extended_daq_queries);
    EXPECT_FALSE(caps.daq_packed_mode);
}

TEST(CapabilitySetFor, V15AddsPackedMode) {
    const CapabilitySet caps = CapabilitySetFor(kProtocolVersion15);
    EXPECT_TRUE(caps.daq_packed_mode);
}

TEST(CapabilitySetFor, OptionalTlSubcommandNeverAdvertised) {
    // 本库基础 Profile 不实现 Optional TL Subcommand，任何版本下都必须为 false
    for (const auto& version :
         {kProtocolVersion12, kProtocolVersion13, kProtocolVersion15}) {
        EXPECT_FALSE(CapabilitySetFor(version).optional_tl_subcommands);
    }
}

TEST(CapabilitySetFor, CapabilitiesGrowMonotonicallyWithVersion) {
    // 高阶版本必须包含低阶版本的全部能力（累加式推导的回归保护）
    const CapabilitySet v12 = CapabilitySetFor(kProtocolVersion12);
    const CapabilitySet v13 = CapabilitySetFor(kProtocolVersion13);
    const CapabilitySet v15 = CapabilitySetFor(kProtocolVersion15);

    EXPECT_TRUE(!v12.get_version || v13.get_version);
    EXPECT_TRUE(!v12.extended_daq_queries || v13.extended_daq_queries);
    EXPECT_TRUE(!v13.get_version || v15.get_version);
    EXPECT_TRUE(!v13.extended_daq_queries || v15.extended_daq_queries);
    EXPECT_TRUE(!v13.daq_packed_mode || v15.daq_packed_mode);
}

TEST(CapabilitySetFor, V14BaselineIsBetweenV13AndV15) {
    // 中间版本的行为：具备 V1.3 能力但无 V1.5 的 packed 模式
    const CapabilitySet caps = CapabilitySetFor(XcpProtocolVersion{1, 4});
    EXPECT_TRUE(caps.get_version);
    EXPECT_TRUE(caps.extended_daq_queries);
    EXPECT_FALSE(caps.daq_packed_mode);
}

// --------------------------------------------------------------------------
// §4.4 Gate：PL 1.2/1.3/1.4/1.5 版本比较与 capability gate
// --------------------------------------------------------------------------

TEST(ProtocolVersionBaseline, V12IsNowWithinSupportedRange) {
    // B-T1 §4.3：下限自 V1.3 下调至 V1.2，以容纳 XCPlite 的 PL 0x0102
    EXPECT_LT(kProtocolVersion12, kProtocolVersion13);
    EXPECT_TRUE(kProtocolVersion12.IsValid());
    // 低于 1.2 仍非法
    VersionPolicy policy;
    const XcpProtocolVersion below_floor{1, 1};
    policy.protocol_version = below_floor;
    EXPECT_FALSE(policy.Validate().empty()) << "1.1 应低于支持下限被拒绝";
}

TEST(ProtocolVersionBaseline, V12IsAcceptedByPolicy) {
    VersionPolicy policy;
    policy.protocol_version = kProtocolVersion12;
    EXPECT_TRUE(policy.Validate().empty()) << "PL 1.2 必须被接受";
}

TEST(ProtocolVersionBaseline, VersionsOrderStrictlyAcrossSupportedRange) {
    // 1.2 < 1.3 < 1.4 < 1.5 的序关系是能力裁决的基础
    // 注意：花括号初始化式不能直接写进宏实参，先落成具名变量
    const XcpProtocolVersion v14{1, 4};
    EXPECT_LT(kProtocolVersion12, kProtocolVersion13);
    EXPECT_LT(kProtocolVersion13, v14);
    EXPECT_LT(v14, kProtocolVersion15);
}

}  // namespace
}  // namespace calmcar::xcp