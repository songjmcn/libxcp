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
// SET_MTA：XCP 1.3 CRO 布局 [F6][MODE][reserved][EXT@3][ADDR@4..7]（8 字节）
// 与 SHORT_UPLOAD 地址域同构；XCPlite 对手端协议调试核证（旧 7 字节布局被
// 真实 Slave 以 ERR_CMD_SYNTAX 拒绝）。
// --------------------------------------------------------------------------

TEST(CommandCodecGolden, SetMtaIntel) {
    const CommandCodec codec(ByteOrder::Intel);
    // 地址 0x70012340, EXT=0x02 => [F6][00][00][02][40][23][01][70]
    ExpectBytes(codec.EncodeSetMta(0x02, 0x70012340U),
                {0xF6, 0x00, 0x00, 0x02, 0x40, 0x23, 0x01, 0x70});
}

TEST(CommandCodecGolden, SetMtaMotorola) {
    const CommandCodec codec(ByteOrder::Motorola);
    // 同一地址在大端会话下 MSB 先出
    ExpectBytes(codec.EncodeSetMta(0x02, 0x70012340U),
                {0xF6, 0x00, 0x00, 0x02, 0x70, 0x01, 0x23, 0x40});
}

TEST(CommandCodecGolden, SetMtaZeroExtension) {
    const CommandCodec codec(ByteOrder::Intel);
    ExpectBytes(codec.EncodeSetMta(0x00, 0x00000060U),
                {0xF6, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00});
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

TEST(CommandCodecGolden, GetDaqEventInfoHasReservedByteBeforeWord) {
    const CommandCodec codec(ByteOrder::Intel);
    // xcp.h:817-818: CRO_LEN=4 and CRO_WORD(1) means bytes 2..3;
    // byte 1 is reserved, as with GET_DAQ_LIST_INFO.
    ExpectBytes(codec.EncodeGetDaqEventInfo(0x0102),
                {0xD7, 0x00, 0x02, 0x01});
    ExpectBytes(codec.EncodeGetDaqEventInfo(7), {0xD7, 0x00, 0x07, 0x00});
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
    // MAX_CTO=8 的典型 Slave：SET_MTA 与 SHORT_UPLOAD 均为 8 字节（地址域
    // 同构：ext@3、addr@4-7；XCPlite 对手端协议调试核证）
    const CommandCodec codec(ByteOrder::Intel);
    EXPECT_EQ(codec.EncodeSetMta(0, 0).size(), 8U);
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

// ---- 变量标定批次：Calibration / Page Switching 黄金报文（docs §7.5.2.5 /
//      §7.5.3；CRO 布局对照 thirdparty/XCPlite/src/xcp.h:604-660）----

TEST(CommandCodecGolden, ModifyBitsIntelAndMotorola) {
    const CommandCodec intel(ByteOrder::Intel);
    // MODIFY_BITS: [EC][shift][AND Mask(WORD)][XOR Mask(WORD)]
    ExpectBytes(intel.EncodeModifyBits(8, 0x00FFU, 0xFF00U),
                {0xEC, 0x08, 0xFF, 0x00, 0x00, 0xFF});
    const CommandCodec moto(ByteOrder::Motorola);
    ExpectBytes(moto.EncodeModifyBits(8, 0x00FFU, 0xFF00U),
                {0xEC, 0x08, 0x00, 0xFF, 0xFF, 0x00});
    // Shift 边界 0/16 合法
    EXPECT_EQ(intel.EncodeModifyBits(0, 0xFFFFU, 0x0000U).size(), 6U);
    EXPECT_EQ(intel.EncodeModifyBits(16, 0x0000U, 0xFFFFU).size(), 6U);
}

TEST(CommandCodecBoundary, ModifyBitsRejectsShiftOverSixteen) {
    const CommandCodec codec(ByteOrder::Intel);
    try {
        (void)codec.EncodeModifyBits(17, 0xFFFFU, 0x0000U);
        FAIL() << "Shift=17 超出 docs §7.5.2.5 范围 0..16，应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

TEST(CommandCodecGolden, SetCalPageLayouts) {
    const CommandCodec codec(ByteOrder::Intel);
    // SET_CAL_PAGE: [EB][mode][segment][page]
    ExpectBytes(codec.EncodeSetCalPage(CalPageModeBit::kEcu, 1, 2),
                {0xEB, 0x01, 0x01, 0x02});
    ExpectBytes(codec.EncodeSetCalPage(CalPageModeBit::kXcp, 0, 255),
                {0xEB, 0x02, 0x00, 0xFF});
    ExpectBytes(
        codec.EncodeSetCalPage(CalPageModeBit::kEcu | CalPageModeBit::kXcp,
                               3, 7),
        {0xEB, 0x03, 0x03, 0x07});
    // ALL 位与 ECU|XCP 组合透传（docs：ALL=0x80 时 Slave 忽略 Segment）
    ExpectBytes(codec.EncodeSetCalPage(
                    CalPageModeBit::kEcu | CalPageModeBit::kAll, 9, 4),
                {0xEB, 0x81, 0x09, 0x04});
}

TEST(CommandCodecBoundary, SetCalPageRejectsModeWithoutEcuOrXcp) {
    const CommandCodec codec(ByteOrder::Intel);
    try {
        (void)codec.EncodeSetCalPage(CalPageModeBit::kNone, 0, 0);
        FAIL() << "Mode 不含 ECU/XCP 位应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    try {
        (void)codec.EncodeSetCalPage(CalPageModeBit::kAll, 0, 0);
        FAIL() << "仅 ALL 位（无 ECU/XCP）应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

TEST(CommandCodecGolden, GetCalPageLayouts) {
    const CommandCodec codec(ByteOrder::Intel);
    // GET_CAL_PAGE: [EA][access_mode][segment]
    ExpectBytes(codec.EncodeGetCalPage(CalPageAccessMode::Ecu, 5),
                {0xEA, 0x01, 0x05});
    ExpectBytes(codec.EncodeGetCalPage(CalPageAccessMode::Xcp, 0),
                {0xEA, 0x02, 0x00});
}

TEST(CommandCodecBoundary, GetCalPageRejectsIllegalAccessMode) {
    const CommandCodec codec(ByteOrder::Intel);
    try {
        (void)codec.EncodeGetCalPage(static_cast<CalPageAccessMode>(0x00), 1);
        FAIL() << "Access Mode 0x00 非法应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    try {
        (void)codec.EncodeGetCalPage(static_cast<CalPageAccessMode>(0x03), 1);
        FAIL() << "Access Mode 0x03 非法应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

TEST(CommandCodecGolden, PagQueryAndPageInfoLayouts) {
    const CommandCodec codec(ByteOrder::Intel);
    // GET_PAG_PROCESSOR_INFO: [E9] 单字节无参
    ExpectBytes(codec.EncodeGetPagProcessorInfo(), {0xE9});
    // GET_SEGMENT_INFO: [E8][mode][segment][segment_info][mapping_index]
    ExpectBytes(codec.EncodeGetSegmentInfo(SegmentInfoMode::BasicInfo, 1,
                                           SegmentInfoSelector::SegmentAddress,
                                           0),
                {0xE8, 0x00, 0x01, 0x00, 0x00});
    ExpectBytes(codec.EncodeGetSegmentInfo(
                    SegmentInfoMode::StandardProperties, 2,
                    SegmentInfoSelector::SegmentAddress, 0),
                {0xE8, 0x01, 0x02, 0x00, 0x00});
    ExpectBytes(codec.EncodeGetSegmentInfo(SegmentInfoMode::MappingInfo, 3,
                                           SegmentInfoSelector::MappingLength,
                                           7),
                {0xE8, 0x02, 0x03, 0x02, 0x07});
    // GET_PAGE_INFO: [E7][reserved][segment][page]
    ExpectBytes(codec.EncodeGetPageInfo(1, 2), {0xE7, 0x00, 0x01, 0x02});
    // SET_SEGMENT_MODE: [E6][mode][segment]
    ExpectBytes(codec.EncodeSetSegmentMode(SegmentModeBit::kFreeze, 4),
                {0xE6, 0x01, 0x04});
    ExpectBytes(codec.EncodeSetSegmentMode(SegmentModeBit::kNone, 4),
                {0xE6, 0x00, 0x04});
    // GET_SEGMENT_MODE: [E5][reserved][segment]
    ExpectBytes(codec.EncodeGetSegmentMode(6), {0xE5, 0x00, 0x06});
    // COPY_CAL_PAGE: [E4][src_segment][src_page][dst_segment][dst_page]
    ExpectBytes(codec.EncodeCopyCalPage(CopyCalPageRequest{1, 2, 3, 4}),
                {0xE4, 0x01, 0x02, 0x03, 0x04});
}

TEST(CommandCodecBoundary, GetSegmentInfoRejectsOutOfRangeModeAndInfo) {
    const CommandCodec codec(ByteOrder::Intel);
    try {
        (void)codec.EncodeGetSegmentInfo(static_cast<SegmentInfoMode>(3), 0,
                                         SegmentInfoSelector::SegmentAddress,
                                         0);
        FAIL() << "Mode=3 超出 0..2 应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    try {
        (void)codec.EncodeGetSegmentInfo(SegmentInfoMode::BasicInfo, 0,
                                         static_cast<SegmentInfoSelector>(3),
                                         0);
        FAIL() << "SegmentInfo=3 超出 0..2 应抛 InvalidArgument";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

}  // namespace
}  // namespace calmcar::xcp
