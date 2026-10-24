/**
 * @file tcp_stream_decoder_test.cpp
 * @brief `TcpStreamDecoder` 的 B-T1 §4.4 Gate 测试：双 Wire Profile 状态机、
 * 逐字节分片、粘包、半帧 FIN、Tail 边界与交叉 Profile 负向。
 *
 * @par 测试组织
 *   每个用例只验证一条契约，失败时能直接定位到规则而非"解码结果不对"。
 */

#include "libxcp/tcp_stream_decoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/ethernet_header_codec.hpp"
#include "libxcp/tcp_wire_profile.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 构造测试字节序列
Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 手工拼一个 Frame：LEN(u16le) + CTR(u16le) + Packet [+ tail]
/// @param ctr 该帧计数器
/// @param packet XCP Packet 字节
/// @param tail_len 追加的 Tail 字节数（AsamV15 用；AsamV14 应传 0）
Bytes RawFrame(DatagramCtr ctr, BytesView packet, std::size_t tail_len = 0) {
    Bytes out;
    const auto len = static_cast<std::uint16_t>(packet.size());
    out.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(ctr & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((ctr >> 8) & 0xFFU));
    out.insert(out.end(), packet.begin(), packet.end());
    out.insert(out.end(), tail_len, static_cast<std::uint8_t>(0x00));
    return out;
}

/// @brief 构造一个 AsamV15 decoder（默认 32-bit 对齐，便于验证 Tail 路径）
TcpStreamDecoder MakeV15(PacketAlignment alignment = PacketAlignment::Bit32,
                         std::size_t max_packet = kEthernetMaxXcpPacket) {
    return TcpStreamDecoder(TcpWireProfile::AsamV15, alignment, max_packet);
}

/// @brief 构造一个 AsamV14 decoder
TcpStreamDecoder MakeV14(std::size_t max_packet = kEthernetMaxXcpPacket) {
    return TcpStreamDecoder(TcpWireProfile::AsamV14, PacketAlignment::Bit8,
                            max_packet);
}

/// @brief 比较只读视图与期望序列（span 无 operator==）
bool ViewEquals(BytesView view, const Bytes& expected) {
    return view.size() == expected.size() &&
           std::equal(view.begin(), view.end(), expected.begin());
}

// --------------------------------------------------------------------------
// 构造契约：Profile/alignment 必须显式且合法
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderCtor, RejectsUnknownProfile) {
    // 禁止自动推断：Unknown 必须被拒绝（fail closed）
    EXPECT_THROW((TcpStreamDecoder(TcpWireProfile::Unknown,
                                   PacketAlignment::Bit8, 1024)),
                 XcpException);
}

TEST(TcpStreamDecoderCtor, RejectsUnknownAlignment) {
    EXPECT_THROW((TcpStreamDecoder(TcpWireProfile::AsamV15,
                                   PacketAlignment::Unknown, 1024)),
                 XcpException);
}

TEST(TcpStreamDecoderCtor, RejectsZeroMaxPacketSize) {
    EXPECT_THROW(
        (TcpStreamDecoder(TcpWireProfile::AsamV15, PacketAlignment::Bit8, 0)),
        XcpException);
}

TEST(TcpStreamDecoderCtor, RejectsMaxPacketAboveLenField) {
    EXPECT_THROW((TcpStreamDecoder(TcpWireProfile::AsamV15,
                                   PacketAlignment::Bit8, 0x10000U)),
                 XcpException);
}

TEST(TcpStreamDecoderCtor, FreezesProfileAndAlignment) {
    // Profile/alignment 是连接级不可变属性
    auto decoder = MakeV15();
    EXPECT_EQ(decoder.Profile(), TcpWireProfile::AsamV15);
    EXPECT_EQ(decoder.Alignment(), PacketAlignment::Bit32);
    EXPECT_EQ(decoder.MaxPacketSize(), kEthernetMaxXcpPacket);
}

// --------------------------------------------------------------------------
// 基础单帧：三种 Profile/alignment 组合
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderSingle, V15Bit8NoTail) {
    auto decoder = MakeV15(PacketAlignment::Bit8);
    const auto frame = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 0);
    const auto result = decoder.Push(BytesView{frame});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 1U);
    EXPECT_EQ(result.frames[0].header.ctr, 0x0001U);
    EXPECT_EQ(result.frames[0].tail_bytes, 0U);
    EXPECT_TRUE(ViewEquals(result.frames[0].xcp_packet, BytesOf({0xFF, 0x00})));
}

TEST(TcpStreamDecoderSingle, V15Bit32ConsumesSyntheticTail) {
    auto decoder = MakeV15(PacketAlignment::Bit32);
    // Header(4) + Packet(2) = 6 -> tail=2
    const auto frame = RawFrame(0x0007, BytesOf({0xAA, 0xBB}), 2);
    const auto result = decoder.Push(BytesView{frame});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 1U);
    EXPECT_EQ(result.frames[0].tail_bytes, 2U);
    EXPECT_TRUE(ViewEquals(result.frames[0].xcp_packet, BytesOf({0xAA, 0xBB})));
}

TEST(TcpStreamDecoderSingle, V14NeverReadsTailEvenAtBit32) {
    // AsamV14 契约：即使 alignment=32 也没有 LEN 外 Tail。
    // 这里刻意让 Packet 长度使 AsamV15 会要求 Tail，以证明 V14 不进入该路径。
    auto decoder = TcpStreamDecoder(TcpWireProfile::AsamV14,
                                    PacketAlignment::Bit32,
                                    kEthernetMaxXcpPacket);
    const auto frame = RawFrame(0x0009, BytesOf({0xAA, 0xBB}), 0);
    const auto result = decoder.Push(BytesView{frame});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 1U);
    EXPECT_EQ(result.frames[0].tail_bytes, 0U);
    EXPECT_TRUE(ViewEquals(result.frames[0].xcp_packet, BytesOf({0xAA, 0xBB})));
}

// --------------------------------------------------------------------------
// 分片：Header/Payload/Tail 的每个 byte boundary
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderFragmentation, EveryHeaderBoundarySplitForV15) {
    auto decoder = MakeV15(PacketAlignment::Bit8);
    const auto frame = RawFrame(0x0102, BytesOf({0x11, 0x22, 0x33}), 0);

    // 在 Header 内部（1..3 字节）切分：每次都不得产出完整帧
    for (std::size_t split = 1; split < kEthernetHeaderSize; ++split) {
        decoder.Reset();
        const auto first =
            decoder.Push(BytesView{frame}.subspan(0, split));
        EXPECT_TRUE(first.frames.empty())
            << "Header 分片 " << split << " 字节时不应急于交付";
        EXPECT_FALSE(first.malformed);

        const auto second =
            decoder.Push(BytesView{frame}.subspan(split));
        ASSERT_EQ(second.frames.size(), 1U) << "补全后应交付 1 帧";
        EXPECT_TRUE(ViewEquals(second.frames[0].xcp_packet,
                               BytesOf({0x11, 0x22, 0x33})));
    }
}

TEST(TcpStreamDecoderFragmentation, EveryPayloadBoundarySplitForV15) {
    auto decoder = MakeV15(PacketAlignment::Bit8);
    const auto frame = RawFrame(0x0103, BytesOf({0x11, 0x22, 0x33}), 0);

    // 在 Payload 内部切分
    for (std::size_t split = kEthernetHeaderSize;
         split < frame.size(); ++split) {
        decoder.Reset();
        const auto first = decoder.Push(BytesView{frame}.subspan(0, split));
        EXPECT_TRUE(first.frames.empty())
            << "Payload 分片至 " << split << " 字节时不应急于交付";
        const auto second = decoder.Push(BytesView{frame}.subspan(split));
        ASSERT_EQ(second.frames.size(), 1U);
        EXPECT_TRUE(ViewEquals(second.frames[0].xcp_packet,
                               BytesOf({0x11, 0x22, 0x33})));
    }
}

TEST(TcpStreamDecoderFragmentation, TailBoundarySplitForV15) {
    auto decoder = MakeV15(PacketAlignment::Bit32);
    // Packet 2 字节 -> tail 2 字节
    const auto frame = RawFrame(0x0104, BytesOf({0xAA, 0xBB}), 2);

    // 在 Tail 内部切分：Payload 完整时可以先交付该帧
    const std::size_t payload_end = kEthernetHeaderSize + 2U;
    for (std::size_t split = payload_end; split < frame.size(); ++split) {
        decoder.Reset();
        const auto first = decoder.Push(BytesView{frame}.subspan(0, split));
        EXPECT_FALSE(first.malformed)
            << "Tail 未收全但流未结束时不得判为畸形";
        // Payload 完整即可交付，Tail 留待后续
        ASSERT_EQ(first.frames.size(), 1U);

        const auto second = decoder.Push(BytesView{frame}.subspan(split));
        EXPECT_FALSE(second.malformed);
    }
}

TEST(TcpStreamDecoderFragmentation, SingleByteAtATimeDecodesAll) {
    // 最严苛分片：每次只投喂 1 字节
    auto decoder = MakeV15(PacketAlignment::Bit32);
    Bytes stream = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 2);
    const Bytes second = RawFrame(0x0002, BytesOf({0xFD}), 3);
    stream.insert(stream.end(), second.begin(), second.end());

    std::vector<std::uint16_t> ctrs;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        const auto result = decoder.Push(BytesView{stream}.subspan(i, 1));
        ASSERT_FALSE(result.malformed) << "第 " << i << " 字节处不应畸形";
        for (const auto& f : result.frames) {
            ctrs.push_back(f.header.ctr);
        }
    }
    ASSERT_EQ(ctrs.size(), 2U);
    EXPECT_EQ(ctrs[0], 0x0001U);
    EXPECT_EQ(ctrs[1], 0x0002U);
}

// --------------------------------------------------------------------------
// 粘包：一次 Push 解出多帧
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderCoalescing, TwoFramesInOnePushV15) {
    auto decoder = MakeV15(PacketAlignment::Bit32);
    Bytes stream = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 2);
    const Bytes second = RawFrame(0x0002, BytesOf({0xFD}), 3);
    stream.insert(stream.end(), second.begin(), second.end());

    const auto result = decoder.Push(BytesView{stream});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 2U);
    EXPECT_EQ(result.frames[0].header.ctr, 0x0001U);
    EXPECT_EQ(result.frames[1].header.ctr, 0x0002U);
}

TEST(TcpStreamDecoderCoalescing, ThreeFramesPlusPartialHeaderV15) {
    // "一次 Push 中多个完整 Frame 和半个后续 Frame"
    auto decoder = MakeV15(PacketAlignment::Bit32);
    Bytes stream = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 2);
    const Bytes second = RawFrame(0x0002, BytesOf({0xFD}), 3);
    const Bytes third = RawFrame(0x0003, BytesOf({0xEE, 0x01}), 2);
    stream.insert(stream.end(), second.begin(), second.end());
    stream.insert(stream.end(), third.begin(), third.end());
    // 追加半个后继 Header（2 字节）
    const Bytes partial = RawFrame(0x0004, BytesOf({0xAB}), 3);
    stream.insert(stream.end(), partial.begin(), partial.begin() + 2);

    const auto result = decoder.Push(BytesView{stream});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 3U) << "只应交付 3 个完整帧";
    EXPECT_EQ(result.frames[2].header.ctr, 0x0003U);
    EXPECT_EQ(decoder.DeliveredFrameCount(), 3U);
}

TEST(TcpStreamDecoderCoalescing, V14FramePlusNextHeaderNoDesync) {
    // V14 连续非对齐 golden：frame + next header 不失步
    auto decoder = MakeV14();
    Bytes stream = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 0);
    const Bytes second = RawFrame(0x0002, BytesOf({0xFD, 0x00, 0x11}), 0);
    stream.insert(stream.end(), second.begin(), second.end());

    const auto result = decoder.Push(BytesView{stream});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 2U);
    EXPECT_TRUE(ViewEquals(result.frames[0].xcp_packet, BytesOf({0xFF, 0x00})));
    EXPECT_TRUE(
        ViewEquals(result.frames[1].xcp_packet, BytesOf({0xFD, 0x00, 0x11})));
}

TEST(TcpStreamDecoderCoalescing, V14PreservesLenInternalPadding) {
    // V14 的 DTO/异步 CRM 可能把 message 总长按 4 字节扩长并**计入 LEN**。
    // 这是 LEN 内的合法数据：必须完整上交，禁止按尾部零值裁剪。
    auto decoder = MakeV14();
    const Bytes packet = BytesOf({0xFF, 0x00, 0x00, 0x00});  // 尾部零是真实数据
    const auto frame = RawFrame(0x0001, packet, 0);
    const auto result = decoder.Push(BytesView{frame});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 1U);
    EXPECT_TRUE(ViewEquals(result.frames[0].xcp_packet, packet))
        << "LEN 内尾部零数据被错误裁剪";
    EXPECT_EQ(result.frames[0].xcp_packet.size(), 4U);
}

// --------------------------------------------------------------------------
// FIN / 截断
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderFinish, CleanEndAfterCompleteFrameIsValid) {
    auto decoder = MakeV15(PacketAlignment::Bit8);
    const auto frame = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 0);
    const auto pushed = decoder.Push(BytesView{frame});
    ASSERT_EQ(pushed.frames.size(), 1U);

    const auto finished = decoder.Finish();
    EXPECT_FALSE(finished.malformed) << "完整帧后 EOF 必须合法";
    EXPECT_TRUE(finished.end_of_stream);
}

TEST(TcpStreamDecoderFinish, HalfHeaderIsTruncated) {
    auto decoder = MakeV15(PacketAlignment::Bit8);
    const auto frame = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 0);
    (void)decoder.Push(BytesView{frame}.subspan(0, 2));  // 半个 Header

    const auto finished = decoder.Finish();
    EXPECT_TRUE(finished.malformed);
    EXPECT_FALSE(finished.malformed_reason.empty());
}

TEST(TcpStreamDecoderFinish, HalfPayloadIsTruncated) {
    auto decoder = MakeV15(PacketAlignment::Bit8);
    const auto frame = RawFrame(0x0001, BytesOf({0xFF, 0x00, 0x11, 0x22}), 0);
    // Header 全给，Payload 只给 2/4
    (void)decoder.Push(BytesView{frame}.subspan(0, kEthernetHeaderSize + 2U));

    const auto finished = decoder.Finish();
    EXPECT_TRUE(finished.malformed);
}

TEST(TcpStreamDecoderFinish, PartialTailIsTruncated) {
    auto decoder = MakeV15(PacketAlignment::Bit32);
    // Packet 2 字节 -> 需要 2 字节 Tail；只投喂 1 字节 Tail
    const auto frame = RawFrame(0x0001, BytesOf({0xAA, 0xBB}), 2);
    const auto pushed =
        decoder.Push(BytesView{frame}.subspan(0, frame.size() - 1U));
    ASSERT_EQ(pushed.frames.size(), 1U) << "Payload 完整应先交付";
    EXPECT_FALSE(pushed.malformed);

    const auto finished = decoder.Finish();
    EXPECT_TRUE(finished.malformed) << "1..N-1 字节部分 Tail 必须判为截断";
}

TEST(TcpStreamDecoderFinish, OmittedTrailingTailIsValid) {
    // AsamV15 单独一帧时 Tail 可选：Payload 完整且无 Tail 后 FIN 合法
    auto decoder = MakeV15(PacketAlignment::Bit32);
    const auto frame = RawFrame(0x0001, BytesOf({0xAA, 0xBB}), 0);  // 不带 Tail
    const auto pushed = decoder.Push(BytesView{frame});
    ASSERT_FALSE(pushed.malformed);
    ASSERT_EQ(pushed.frames.size(), 1U);

    const auto finished = decoder.Finish();
    EXPECT_FALSE(finished.malformed) << "省略末尾 Tail 必须合法";
}

// --------------------------------------------------------------------------
// 畸形：不可恢复，且必须停机
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderMalformed, ZeroLenFrameFailsClosed) {
    auto decoder = MakeV15();
    const Bytes stream = BytesOf({0x00, 0x00, 0x01, 0x00});
    const auto result = decoder.Push(BytesView{stream});
    EXPECT_TRUE(result.malformed);
    EXPECT_TRUE(decoder.IsMalformed());
}

TEST(TcpStreamDecoderMalformed, LenAboveConfiguredMaxFailsClosed) {
    // 配置上限 16 字节，对端声称 LEN=100 -> 拒绝且不扩容
    auto decoder = MakeV15(PacketAlignment::Bit8, 16U);
    const Bytes stream = BytesOf({0x64, 0x00, 0x01, 0x00});
    const auto result = decoder.Push(BytesView{stream});
    EXPECT_TRUE(result.malformed);
}

TEST(TcpStreamDecoderMalformed, StaysFailedAndIgnoresFurtherData) {
    // 一旦畸形即停机：后续投喂不得让它"自愈"
    auto decoder = MakeV15();
    (void)decoder.Push(BytesOf({0x00, 0x00, 0x01, 0x00}));
    ASSERT_TRUE(decoder.IsMalformed());

    const auto later = decoder.Push(RawFrame(0x0002, BytesOf({0xFF}), 0));
    EXPECT_TRUE(later.malformed);
    EXPECT_TRUE(later.frames.empty());
}

TEST(TcpStreamDecoderMalformed, IncorrectlyAddedTailFailsWithoutHeuristicRecovery) {
    // AsamV14 对端若错误附加了 LEN 外 Tail，decoder 必须稳定进入畸形路径，
    // **不得**启发式恢复（例如"读到下一个 0 再当 Header"）。
    auto decoder = MakeV14();
    const Bytes packet = BytesOf({0xFF, 0x00});
    const auto frame = RawFrame(0x0001, packet, 2);  // 多出的 2 字节 Tail
    const auto first = decoder.Push(BytesView{frame});
    ASSERT_FALSE(first.malformed);
    ASSERT_EQ(first.frames.size(), 1U) << "第一帧本身合法";

    // 多出的 2 字节被当作下一个 Header 的开头；补足后其 LEN 必然离谱，
    // 必须判为畸形而不是"恢复"出第二个假帧。
    Bytes more = BytesOf({0x00, 0x00});
    const auto second = decoder.Push(BytesView{more});
    EXPECT_TRUE(second.malformed);
    EXPECT_TRUE(second.frames.empty());
}

// --------------------------------------------------------------------------
// Reset 与视图稳定性
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderReset, ClearsStreamStateButKeepsCounter) {
    auto decoder = MakeV15(PacketAlignment::Bit8);
    const auto frame = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 0);
    (void)decoder.Push(BytesView{frame});
    ASSERT_EQ(decoder.DeliveredFrameCount(), 1U);

    decoder.Reset();
    EXPECT_FALSE(decoder.IsMalformed());
    EXPECT_EQ(decoder.Profile(), TcpWireProfile::AsamV15) << "Profile 不随 Reset 改变";
    EXPECT_EQ(decoder.DeliveredFrameCount(), 1U) << "累计计数跨 Reset 保留";

    // Reset 后可重新解出同一帧（证明状态已干净）
    const auto again = decoder.Push(BytesView{frame});
    ASSERT_EQ(again.frames.size(), 1U);
    EXPECT_EQ(decoder.DeliveredFrameCount(), 2U);
}

TEST(TcpStreamDecoderReset, ClearsMalformedState) {
    auto decoder = MakeV15();
    (void)decoder.Push(BytesOf({0x00, 0x00, 0x01, 0x00}));
    ASSERT_TRUE(decoder.IsMalformed());

    decoder.Reset();
    EXPECT_FALSE(decoder.IsMalformed()) << "Reset 应允许重连后重新开始";

    const auto frame = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 2);
    const auto result = decoder.Push(BytesView{frame});
    EXPECT_FALSE(result.malformed);
    EXPECT_EQ(result.frames.size(), 1U);
}

TEST(TcpStreamDecoderReset, ViewsFromPreviousRoundRemainUsableUntilNextPush) {
    // 视图稳定性：交付后、下一次 Push 前，Frame 内容必须仍然正确可读。
    auto decoder = MakeV15(PacketAlignment::Bit8);
    const auto frame = RawFrame(0x0001, BytesOf({0x11, 0x22, 0x33}), 0);
    auto result = decoder.Push(BytesView{frame});
    ASSERT_EQ(result.frames.size(), 1U);
    // 把视图拷出来读，验证存储未被搬动
    const Bytes copied(result.frames[0].xcp_packet.begin(),
                       result.frames[0].xcp_packet.end());
    EXPECT_EQ(copied, BytesOf({0x11, 0x22, 0x33}));
}

TEST(TcpStreamDecoderReset, EmptyPushProducesNothingAndIsNotMalformed) {
    auto decoder = MakeV15();
    const auto result = decoder.Push(BytesView{});
    EXPECT_FALSE(result.malformed);
    EXPECT_TRUE(result.frames.empty());
}

// --------------------------------------------------------------------------
// 交叉 Profile 负向：同一字节流在两种 Profile 下结论不同
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderCrossProfile, V15TailConfusesV14ForSecondFrame) {
    // 用 AsamV15 规则造一个带 Tail 的双帧流，交给 V14 解码：
    // V14 会把 Tail 当作下一帧 Header 的一部分，从而无法正确解出第二帧。
    const Bytes packet = BytesOf({0xAA, 0xBB});  // tail=2 (Bit32)
    Bytes stream = RawFrame(0x0001, packet, 2);
    const Bytes second = RawFrame(0x0002, BytesOf({0xCC, 0xDD}), 2);
    stream.insert(stream.end(), second.begin(), second.end());

    auto v15 = MakeV15(PacketAlignment::Bit32);
    const auto ok = v15.Push(BytesView{stream});
    ASSERT_FALSE(ok.malformed);
    ASSERT_EQ(ok.frames.size(), 2U) << "V15 应正确解出两帧";
    EXPECT_EQ(ok.frames[1].header.ctr, 0x0002U);

    auto v14 = TcpStreamDecoder(TcpWireProfile::AsamV14, PacketAlignment::Bit32,
                                kEthernetMaxXcpPacket);
    const auto wrong = v14.Push(BytesView{stream});
    // V14 视角下第一帧只有 4+2=6 字节，其后 2 字节 Tail 会被当成新 Header
    EXPECT_TRUE(wrong.frames.size() < 2U || wrong.malformed)
        << "交叉 Profile 必须暴露不一致，不得静默得到相同结果";
}

TEST(TcpStreamDecoderCrossProfile, V14StreamIsAlsoValidV15WhenAlignedBit8) {
    // alignment=8 下两种 wire 形式数值等价（计划 §1.2 明确说明）：
    // 这正是"必须有显式 Profile"的动机——不能靠首帧内容区分。
    const Bytes stream = RawFrame(0x0001, BytesOf({0xFF, 0x00}), 0);

    auto v15 = MakeV15(PacketAlignment::Bit8);
    auto v14 = MakeV14();
    const auto r15 = v15.Push(BytesView{stream});
    const auto r14 = v14.Push(BytesView{stream});
    ASSERT_EQ(r15.frames.size(), 1U);
    ASSERT_EQ(r14.frames.size(), 1U);
    EXPECT_TRUE(ViewEquals(r15.frames[0].xcp_packet, BytesOf({0xFF, 0x00})));
    EXPECT_TRUE(ViewEquals(r14.frames[0].xcp_packet, BytesOf({0xFF, 0x00})));
    EXPECT_EQ(r15.frames[0].tail_bytes, r14.frames[0].tail_bytes);
}

// --------------------------------------------------------------------------
// Profile 名称渲染（诊断用）
// --------------------------------------------------------------------------

TEST(TcpWireProfileName, RendersAllEnumerators) {
    EXPECT_EQ(TcpWireProfileName(TcpWireProfile::Unknown), "Unknown");
    EXPECT_EQ(TcpWireProfileName(TcpWireProfile::AsamV15), "AsamV15");
    EXPECT_EQ(TcpWireProfileName(TcpWireProfile::AsamV14), "AsamV14");
}

TEST(TcpWireProfileName, ResolvedHelperExcludesUnknownOnly) {
    EXPECT_FALSE(IsTcpWireProfileResolved(TcpWireProfile::Unknown));
    EXPECT_TRUE(IsTcpWireProfileResolved(TcpWireProfile::AsamV15));
    EXPECT_TRUE(IsTcpWireProfileResolved(TcpWireProfile::AsamV14));
}

// --------------------------------------------------------------------------
// §4.4 Gate：大帧与上限边界（decoder 侧）
// --------------------------------------------------------------------------

TEST(TcpStreamDecoderGate, MaxLenFrameWithBit32AlignmentDecodes) {
    // 65535 + 32-bit alignment：LEN 为合法上界，Tail 为 1 字节
    auto decoder = MakeV15(PacketAlignment::Bit32);
    const Bytes packet(kEthernetMaxXcpPacket, 0xEF);
    const auto frame =
        EncodeEthernetFrameAligned(packet, 0x0001, PacketAlignment::Bit32);
    EXPECT_EQ(frame.tail_bytes, 1U);

    const auto result = decoder.Push(BytesView{frame.data});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 1U);
    EXPECT_EQ(result.frames[0].xcp_packet.size(), kEthernetMaxXcpPacket);
}

TEST(TcpStreamDecoderGate, LenExactlyAtConfiguredMaxIsAccepted) {
    // 上限即允许值：LEN == max_packet_size 不得被判越界
    const std::size_t limit = 32U;
    auto decoder = MakeV15(PacketAlignment::Bit8, limit);
    const Bytes packet(limit, 0x11);
    const auto frame =
        EncodeEthernetFrameAligned(packet, 0x0001, PacketAlignment::Bit8);
    const auto result = decoder.Push(BytesView{frame.data});
    ASSERT_FALSE(result.malformed) << "LEN == 上限必须接受";
    ASSERT_EQ(result.frames.size(), 1U);
    EXPECT_EQ(result.frames[0].xcp_packet.size(), limit);
}

TEST(TcpStreamDecoderGate, LenOneAboveConfiguredMaxIsRejected) {
    const std::size_t limit = 32U;
    auto decoder = MakeV15(PacketAlignment::Bit8, limit);
    const Bytes packet(limit + 1U, 0x11);
    const auto frame =
        EncodeEthernetFrameAligned(packet, 0x0001, PacketAlignment::Bit8);
    const auto result = decoder.Push(BytesView{frame.data});
    EXPECT_TRUE(result.malformed) << "LEN 超过上限必须 fail closed";
}

TEST(TcpStreamDecoderGate, ManyFramesSinglePushStayOrdered) {
    // 粘包压力：8 帧一次投喂，顺序与 CTR 必须严格保持
    auto decoder = MakeV15(PacketAlignment::Bit8);
    Bytes stream;
    for (std::uint16_t i = 0; i < 8U; ++i) {
        const Bytes packet = BytesOf({0x10, static_cast<std::uint8_t>(i)});
        const auto frame = RawFrame(i, packet, 0);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }
    const auto result = decoder.Push(BytesView{stream});
    ASSERT_FALSE(result.malformed);
    ASSERT_EQ(result.frames.size(), 8U);
    for (std::uint16_t i = 0; i < 8U; ++i) {
        EXPECT_EQ(result.frames[i].header.ctr, i) << "第 " << i << " 帧顺序错乱";
    }
    EXPECT_EQ(decoder.DeliveredFrameCount(), 8U);
}

TEST(TcpStreamDecoderGate, V14LenInternalPaddingSurvivesByteAtATime) {
    // V14 的 LEN 内扩长在逐字节分片下也必须完整保留
    auto decoder = MakeV14();
    const Bytes packet = BytesOf({0xFF, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto frame = RawFrame(0x0001, packet, 0);
    bool decoded = false;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const auto r = decoder.Push(BytesView{frame}.subspan(i, 1));
        ASSERT_FALSE(r.malformed);
        if (!r.frames.empty()) {
            ASSERT_EQ(r.frames.size(), 1U);
            EXPECT_TRUE(ViewEquals(r.frames[0].xcp_packet, packet))
                << "LEN 内尾部零数据在分片下被裁剪";
            decoded = true;
        }
    }
    EXPECT_TRUE(decoded);
}

}  // namespace
}  // namespace calmcar::xcp