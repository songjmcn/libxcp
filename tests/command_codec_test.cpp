/**
 * @file command_codec_test.cpp
 * @brief CommandCodec 黄金报文测试（Intel / Motorola）与参数边界测试。
 *
 * 报文布局依据 docs/XCP_1.3.0_document.md 第 7.5.1 节与设计文档第 9.1 / 17.1
 * 节。
 */

#include "libxcp/command_codec.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

/// @brief 断言 CTO 与期望字节序列完全一致（含长度，防止多余尾字节）
void ExpectBytes(const Bytes& actual,
                 const std::vector<std::uint8_t>& expected) {
    EXPECT_EQ(actual, Bytes(expected.begin(), expected.end()));
}

/// @brief 由初始化列表构造 Bytes（Seed&Key 报文构造用）
Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

// --------------------------------------------------------------------------
// 2 字节命令：reserved 必须为 0
// --------------------------------------------------------------------------

TEST(CommandCodecGolden, ConnectDefaultMode) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeConnect(), {0xFF, 0x00});
}

TEST(CommandCodecGolden, ConnectUserDefinedMode) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeConnect(0x01), {0xFF, 0x01});
}

TEST(CommandCodecGolden, TwoByteCommands) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeDisconnect(), {0xFE, 0x00});
    ExpectBytes(codec.EncodeGetStatus(), {0xFD, 0x00});
    ExpectBytes(codec.EncodeSynch(), {0xFC, 0x00});
    ExpectBytes(codec.EncodeGetCommModeInfo(), {0xFB, 0x00});
}

// --------------------------------------------------------------------------
// SET_MTA：地址按 Session Byte Order，扩展在 Byte 2
// --------------------------------------------------------------------------

TEST(CommandCodecGolden, SetMtaIntel) {
    const CommandCodec codec(ByteOrder::Intel);
    // 地址 0x70012340, EXT=0x02 => [F6][00][02][40][23][01][70]
    ExpectBytes(codec.EncodeSetMta(0x02, 0x70012340U),
                {0xF6, 0x00, 0x02, 0x40, 0x23, 0x01, 0x70});
}

TEST(CommandCodecGolden, SetMtaMotorola) {
    const CommandCodec codec(ByteOrder::Motorola);
    // 同一地址在大端会话下 MSB 先出
    ExpectBytes(codec.EncodeSetMta(0x02, 0x70012340U),
                {0xF6, 0x00, 0x02, 0x70, 0x01, 0x23, 0x40});
}

TEST(CommandCodecGolden, SetMtaZeroExtension) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeSetMta(0x00, 0x00000060U),
                {0xF6, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00});
}

// --------------------------------------------------------------------------
// UPLOAD
// --------------------------------------------------------------------------

TEST(CommandCodecGolden, Upload) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeUpload(1), {0xF5, 0x01});
    ExpectBytes(codec.EncodeUpload(7), {0xF5, 0x07});
    ExpectBytes(codec.EncodeUpload(0xFF), {0xF5, 0xFF});
}

TEST(CommandCodecBoundary, UploadRejectsZeroAndOverOneByte) {
    const CommandCodec codec(ByteOrder::Intel);
    // [[nodiscard]] 返回值需显式丢弃，避免 C4834
    EXPECT_THROW((void)codec.EncodeUpload(0), XcpException);
    EXPECT_THROW((void)codec.EncodeUpload(256), XcpException);
    try {
        (void)codec.EncodeUpload(0);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

// --------------------------------------------------------------------------
// SHORT_UPLOAD
// --------------------------------------------------------------------------

TEST(CommandCodecGolden, ShortUploadIntel) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeShortUpload(4, 0x00, 0x00000060U),
                {0xF4, 0x04, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00});
}

TEST(CommandCodecGolden, ShortUploadMotorola) {
    const CommandCodec codec(ByteOrder::Motorola);
    ExpectBytes(codec.EncodeShortUpload(4, 0x01, 0x70012340U),
                {0xF4, 0x04, 0x00, 0x01, 0x70, 0x01, 0x23, 0x40});
}

TEST(CommandCodecBoundary, ShortUploadRejectsZeroAndOverOneByte) {
    const CommandCodec codec(ByteOrder::Intel);
    EXPECT_THROW((void)codec.EncodeShortUpload(0, 0x00, 0x0U), XcpException);
    EXPECT_THROW((void)codec.EncodeShortUpload(256, 0x00, 0x0U), XcpException);
}

// --------------------------------------------------------------------------
// GET_SEED / UNLOCK（Seed&Key，批次 7）
// --------------------------------------------------------------------------

TEST(CommandCodecGolden, GetSeedFirstModeGolden) {
    const CommandCodec codec(ByteOrder::Intel);
    // 报文为 [F8][mode][resource]：Mode 在前、Resource 在后（双源交叉验证）
    ExpectBytes(codec.EncodeGetSeed(Resource::CalPag, SeedMode::First),
                {0xF8, 0x00, 0x01});
}

TEST(CommandCodecGolden, GetSeedRemainderModeAllResources) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeGetSeed(Resource::CalPag, SeedMode::Remainder),
                {0xF8, 0x01, 0x01});
    ExpectBytes(codec.EncodeGetSeed(Resource::Daq, SeedMode::Remainder),
                {0xF8, 0x01, 0x04});
    ExpectBytes(codec.EncodeGetSeed(Resource::Stim, SeedMode::Remainder),
                {0xF8, 0x01, 0x08});
    ExpectBytes(codec.EncodeGetSeed(Resource::Pgm, SeedMode::First),
                {0xF8, 0x00, 0x10});
}

TEST(CommandCodecGolden, SeedKeyCommandsAreByteOrderIndependent) {
    // GET_SEED/UNLOCK 全部为单字节字段：Motorola 会话下报文必须与 Intel
    // 完全一致（与 Session Byte Order 解耦）
    const CommandCodec intel(ByteOrder::Intel);
    const CommandCodec motorola(ByteOrder::Motorola);
    ExpectBytes(motorola.EncodeGetSeed(Resource::Daq, SeedMode::First),
                intel.EncodeGetSeed(Resource::Daq, SeedMode::First));
    ExpectBytes(motorola.EncodeUnlock(0x02, BytesOf({0xAA, 0xBB})),
                intel.EncodeUnlock(0x02, BytesOf({0xAA, 0xBB})));
}

TEST(CommandCodecGolden, UnlockFirstFrameCarriesTotalLength) {
    const CommandCodec codec(ByteOrder::Intel);
    // 首帧：Length=Key 总长（10），本帧携带 6 字节（MAX_CTO-2）
    ExpectBytes(
        codec.EncodeUnlock(10, BytesOf({0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5})),
        {0xF7, 0x0A, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5});
}

TEST(CommandCodecGolden, UnlockFollowFrameCarriesRemainingLength) {
    const CommandCodec codec(ByteOrder::Intel);
    // 后续帧：Length=剩余长度（4），本帧携带全部剩余 4 字节
    ExpectBytes(codec.EncodeUnlock(4, BytesOf({0xB0, 0xB1, 0xB2, 0xB3})),
                {0xF7, 0x04, 0xB0, 0xB1, 0xB2, 0xB3});
}

TEST(CommandCodecBoundary, UnlockRejectsLengthSmallerThanSegment) {
    const CommandCodec codec(ByteOrder::Intel);
    // Length 字段声明的剩余量不可能小于本帧携带量
    try {
        (void)codec.EncodeUnlock(2, BytesOf({0x01, 0x02, 0x03}));
        FAIL() << "Length < 本帧字节数应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

// --------------------------------------------------------------------------
// 其他行为
// --------------------------------------------------------------------------

TEST(CommandCodecBehavior, ReportsConfiguredByteOrder) {
    EXPECT_EQ(CommandCodec(ByteOrder::Intel).GetByteOrder(), ByteOrder::Intel);
    EXPECT_EQ(CommandCodec(ByteOrder::Motorola).GetByteOrder(),
              ByteOrder::Motorola);
}

// --------------------------------------------------------------------------
// 批次14（T14-13）：DAQ 命令组与写回的黄金报文
//
// 字节偏移依据：docs/XCP_1.3.0_document.md 的字段顺序 + 只读交叉参考
// thirdparty/XCPlite/src/xcp.h（见 command_codec.hpp 顶部的对照表）。
// --------------------------------------------------------------------------

TEST(CommandCodecGolden, ClearDaqListLayout) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeClearDaqList(0x0201), {0xE3, 0x00, 0x01, 0x02});
    const CommandCodec moto(ByteOrder::Motorola);
    ExpectBytes(moto.EncodeClearDaqList(0x0201), {0xE3, 0x00, 0x02, 0x01});
}

TEST(CommandCodecGolden, SetDaqPtrLayout) {
    const CommandCodec codec(ByteOrder::Intel);
    // [E2][reserved][DAQ(WORD)][ODT][ENTRY]（xcp.h:689-692）
    ExpectBytes(codec.EncodeSetDaqPtr(3, 1, 2),
                {0xE2, 0x00, 0x03, 0x00, 0x01, 0x02});
}

TEST(CommandCodecGolden, WriteDaqLayoutAndBitOffsetNone) {
    const CommandCodec codec(ByteOrder::Intel);
    // [E1][BIT_OFFSET][SIZE][EXT][ADDR(DWORD)]；0xFF = 无位偏（docs L1861）
    ExpectBytes(codec.EncodeWriteDaq(kDaqBitOffsetNone, 4, 0x12, 0x000C5508),
                {0xE1, 0xFF, 0x04, 0x12, 0x08, 0x55, 0x0C, 0x00});
}

TEST(CommandCodecBoundary, WriteDaqRejectsZeroSize) {
    const CommandCodec codec(ByteOrder::Intel);
    try {
        (void)codec.EncodeWriteDaq(kDaqBitOffsetNone, 0, 0, 0);
        FAIL() << "Size=0 应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

TEST(CommandCodecGolden, SetDaqListModeLayout) {
    const CommandCodec codec(ByteOrder::Intel);
    // [E0][MODE][DAQ(WORD)][EVENT(WORD)][PRESCALER][PRIORITY]（xcp.h:713-718）
    const DaqListModeBit mode =
        DaqListModeBit::kDtoCounter | DaqListModeBit::kTimestamp;
    ExpectBytes(codec.EncodeSetDaqListMode(mode, 2, 1, 1, 0xFF),
                {0xE0, 0x18, 0x02, 0x00, 0x01, 0x00, 0x01, 0xFF});
}

TEST(CommandCodecGolden, StartStopDaqListAndSynchLayouts) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeStartStopDaqList(DaqListAction::Select, 5),
                {0xDE, 0x02, 0x05, 0x00});
    ExpectBytes(codec.EncodeStartStopSynch(DaqSynchAction::StartSelected),
                {0xDD, 0x01});
}

TEST(CommandCodecGolden, DaqQueryCommandsHaveNoReservedByte) {
    const CommandCodec codec(ByteOrder::Intel);
    // GET_DAQ_PROCESSOR_INFO / GET_DAQ_RESOLUTION_INFO / READ_DAQ 均 1 字节
    // （xcp.h:789/798/781）；GET_DAQ_LIST_INFO 带 reserved + DAQ(WORD)
    ExpectBytes(codec.EncodeGetDaqProcessorInfo(), {0xDA});
    ExpectBytes(codec.EncodeGetDaqResolutionInfo(), {0xD9});
    ExpectBytes(codec.EncodeReadDaq(), {0xDB});
    ExpectBytes(codec.EncodeGetDaqListInfo(7), {0xD8, 0x00, 0x07, 0x00});
}

TEST(CommandCodecGolden, DownloadAndShortDownloadLayouts) {
    const CommandCodec codec(ByteOrder::Intel);
    // DOWNLOAD: [F0][SIZE][data...]（xcp.h:578-581）
    ExpectBytes(codec.EncodeDownload(4, BytesOf({0x00, 0x00, 0x80, 0x3F})),
                {0xF0, 0x04, 0x00, 0x00, 0x80, 0x3F});
    // SHORT_DOWNLOAD: [ED][SIZE][reserved][EXT][ADDR(DWORD)][data...]
    ExpectBytes(
        codec.EncodeShortDownload(2, 0x12, 0x00003100, BytesOf({0x34, 0x12})),
        {0xED, 0x02, 0x00, 0x12, 0x00, 0x31, 0x00, 0x00, 0x34, 0x12});
}

TEST(CommandCodecBoundary, DownloadRejectsEmptyAndOutOfRangeElements) {
    const CommandCodec codec(ByteOrder::Intel);
    try {
        (void)codec.EncodeDownload(1, BytesView{});
        FAIL() << "空数据应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    try {
        (void)codec.EncodeDownload(0, BytesOf({0x01}));
        FAIL() << "元素数 0 应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    try {
        (void)codec.EncodeShortDownload(256, 0, 0, BytesOf({0x01}));
        FAIL() << "元素数超单字节字段应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

TEST(CommandCodecBehavior, GoldenLengthsMatchMaxCtoEightLayout) {
    // MAX_CTO=8 的典型 Slave：SET_MTA 与 SHORT_UPLOAD 恰好占满 8/7 字节
    const CommandCodec codec(ByteOrder::Intel);
    EXPECT_EQ(codec.EncodeSetMta(0, 0).size(), 7U);
    EXPECT_EQ(codec.EncodeShortUpload(1, 0, 0).size(), 8U);
    EXPECT_EQ(codec.EncodeConnect().size(), 2U);
    EXPECT_EQ(codec.EncodeUpload(1).size(), 2U);
    // 批次14 新增命令的定长部分同样受 MAX_CTO=8 约束（写回只能走 DOWNLOAD）
    EXPECT_EQ(codec.EncodeWriteDaq(kDaqBitOffsetNone, 1, 0, 0).size(), 8U);
    EXPECT_EQ(
        codec.EncodeSetDaqListMode(DaqListModeBit::kNone, 0, 0, 1, 0).size(),
        8U);
    EXPECT_EQ(codec.EncodeStartStopDaqList(DaqListAction::Start, 0).size(), 4U);
    EXPECT_EQ(codec.EncodeStartStopSynch(DaqSynchAction::StopAll).size(), 2U);
    EXPECT_EQ(codec.EncodeGetDaqListInfo(0).size(), 4U);
    EXPECT_EQ(codec.EncodeGetDaqProcessorInfo().size(), 1U);
    EXPECT_EQ(codec.EncodeGetDaqResolutionInfo().size(), 1U);
    EXPECT_EQ(codec.EncodeReadDaq().size(), 1U);
    EXPECT_EQ(codec.EncodeClearDaqList(0).size(), 4U);
    EXPECT_EQ(codec.EncodeSetDaqPtr(0, 0, 0).size(), 6U);
    EXPECT_EQ(codec.EncodeDownload(1, BytesOf({0x11})).size(), 3U);
    EXPECT_EQ(codec.EncodeShortDownload(1, 0, 0, BytesOf({0x11})).size(), 9U);
}

}  // namespace
}  // namespace calmcar::xcp
