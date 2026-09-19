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
