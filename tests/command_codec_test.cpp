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

TEST(CommandCodecBehavior, GoldenLengthsMatchMaxCtoEightLayout) {
    // MAX_CTO=8 的典型 Slave：SET_MTA 与 SHORT_UPLOAD 恰好占满 8/7 字节
    const CommandCodec codec(ByteOrder::Intel);
    EXPECT_EQ(codec.EncodeSetMta(0, 0).size(), 7U);
    EXPECT_EQ(codec.EncodeShortUpload(1, 0, 0).size(), 8U);
    EXPECT_EQ(codec.EncodeConnect().size(), 2U);
    EXPECT_EQ(codec.EncodeUpload(1).size(), 2U);
}

}  // namespace
}  // namespace calmcar::xcp
