/**
 * @file dto_envelope_decoder_test.cpp
 * @brief DtoEnvelopeDecoder 四种识别模式、计数器/时间戳切分与畸形帧防御的单元测试。
 *
 * 依据 code-plan/libxcp_测量子系统代码增长计划.md §5.1 / §6（v0.2）。
 * 覆盖：
 *  - Absolute（绝对 ODT 号）
 *  - RelativeByte（XCPlite：[relODT][0xAA][DAQ16 LE]，header_bytes=4）
 *  - RelativeWord / RelativeWordAligned（WORD 识别：低字节 DAQ、高字节相对 ODT）
 *  - counter / timestamp 尾段切分与位宽合法性
 *  - pid_off / 短帧 / 识别字段非法 → MalformedPacket
 *  - 时间戳位宽非法 → UnsupportedFeature
 */

#include "libxcp/daq/dto_envelope_decoder.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 构造测试用字节序列
Bytes BytesOf(std::initializer_list<std::uint8_t> init) { return Bytes(init); }

// --------------------------------------------------------------------------
// Absolute 模式
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, AbsoluteParsesAbsoluteOdtAndPayload) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.header_bytes = 1;

    const auto dto = BytesOf({0x07, 0xDE, 0xAD, 0xBE});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.daq_list, 0U);  // Absolute 由调用方路由补充
    EXPECT_EQ(env.identity.odt, 0x07U);
    EXPECT_FALSE(env.counter.has_value());
    EXPECT_FALSE(env.raw_timestamp.has_value());
    ASSERT_EQ(env.payload.size(), 3U);
    EXPECT_EQ(env.payload[0], 0xDE);
    EXPECT_EQ(env.payload[1], 0xAD);
    EXPECT_EQ(env.payload[2], 0xBE);
}

// --------------------------------------------------------------------------
// RelativeByte（XCPlite 实然）
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, RelativeByteXcpliteHeader) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::RelativeByte;
    layout.first_odt = 0x10;
    layout.header_bytes = 4;  // [relODT][0xAA][DAQ16 LE]

    // relODT=0x02，DAQ16=0x000B（LE），净荷自 byte4。
    const auto dto = BytesOf({0x02, 0xAA, 0x0B, 0x00, 0x10, 0x20});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.daq_list, 0x000BU);                // DAQ16
    EXPECT_EQ(env.identity.odt, 0x10U + 0x02U);               // first_odt + rel
    ASSERT_EQ(env.payload.size(), 2U);
    EXPECT_EQ(env.payload[0], 0x10);
    EXPECT_EQ(env.payload[1], 0x20);
}

TEST(DtoEnvelopeDecoder, RelativeByteMissingMarkerThrows) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::RelativeByte;
    layout.header_bytes = 4;

    const auto dto = BytesOf({0x02, 0xBB, 0x0B, 0x00, 0x10});
    EXPECT_THROW(
        {
            try {
                (void)decoder.Decode(dto, layout);
            } catch (const XcpException& e) {
                EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
                throw;
            }
        },
        XcpException);
}

// --------------------------------------------------------------------------
// RelativeWord / RelativeWordAligned
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, RelativeWordLowDaqHighRelOdt) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::RelativeWord;
    layout.first_odt = 0x30;
    layout.header_bytes = 2;

    // 低字节 DAQ=0x05，高字节 相对 ODT=0x02。
    const auto dto = BytesOf({0x05, 0x02, 0xAA, 0xBB});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.daq_list, 0x05U);
    EXPECT_EQ(env.identity.odt, 0x30U + 0x02U);
    ASSERT_EQ(env.payload.size(), 2U);
    EXPECT_EQ(env.payload[0], 0xAA);
    EXPECT_EQ(env.payload[1], 0xBB);
}

TEST(DtoEnvelopeDecoder, RelativeWordAlignedFourByteBase) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type =
        IdentificationFieldType::RelativeWordAligned;
    layout.first_odt = 0x30;
    layout.header_bytes = 4;  // 扩展表按 4 字节 DWORD 对齐

    // WORD 在低 2 字节，payload 自 byte4。
    const auto dto = BytesOf({0x05, 0x02, 0x00, 0x00, 0x12});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.daq_list, 0x05U);
    EXPECT_EQ(env.identity.odt, 0x30U + 0x02U);
    ASSERT_EQ(env.payload.size(), 1U);
    EXPECT_EQ(env.payload[0], 0x12);
}

// --------------------------------------------------------------------------
// counter / timestamp 尾段
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, CounterByteParsedAfterIdField) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.counter_enabled = true;
    layout.header_bytes = 1;

    // [ODT=0x01][counter=0x42][payload...]
    const auto dto = BytesOf({0x01, 0x42, 0xAB});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.odt, 0x01U);
    ASSERT_TRUE(env.counter.has_value());
    EXPECT_EQ(*env.counter, 0x42U);
    ASSERT_EQ(env.payload.size(), 1U);
    EXPECT_EQ(env.payload[0], 0xAB);
}

TEST(DtoEnvelopeDecoder, Timestamp32ReadLe) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.timestamp_enabled = true;
    layout.timestamp_size_bits = 32;
    layout.header_bytes = 1;

    // [ODT=0x01][ts=0x01020304 LE][payload...]
    const auto dto = BytesOf({0x01, 0x04, 0x03, 0x02, 0x01, 0xEE});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    ASSERT_TRUE(env.raw_timestamp.has_value());
    EXPECT_EQ(*env.raw_timestamp, 0x01020304ULL);
    ASSERT_EQ(env.payload.size(), 1U);
    EXPECT_EQ(env.payload[0], 0xEE);
}

TEST(DtoEnvelopeDecoder, Timestamp64ReadLe) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.timestamp_enabled = true;
    layout.timestamp_size_bits = 64;
    layout.header_bytes = 1;

    // [ODT=0x01][ts=8B LE]
    const auto dto = BytesOf({0x01, 0x01, 0x02, 0x03, 0x04,
                              0x05, 0x06, 0x07, 0x08});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    ASSERT_TRUE(env.raw_timestamp.has_value());
    EXPECT_EQ(*env.raw_timestamp, 0x0807060504030201ULL);
    EXPECT_TRUE(env.payload.empty());
}

TEST(DtoEnvelopeDecoder, CounterAndTimestampTogether) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.counter_enabled = true;
    layout.timestamp_enabled = true;
    layout.timestamp_size_bits = 16;
    layout.header_bytes = 1;

    // [ODT=0x02][counter=0x07][ts=0x0304 LE][payload=0x99,0x88]
    const auto dto =
        BytesOf({0x02, 0x07, 0x04, 0x03, 0x99, 0x88});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.odt, 0x02U);
    ASSERT_TRUE(env.counter.has_value());
    EXPECT_EQ(*env.counter, 0x07U);
    ASSERT_TRUE(env.raw_timestamp.has_value());
    EXPECT_EQ(*env.raw_timestamp, 0x0304U);
    ASSERT_EQ(env.payload.size(), 2U);
    EXPECT_EQ(env.payload[0], 0x99);
    EXPECT_EQ(env.payload[1], 0x88);
}

TEST(DtoEnvelopeDecoder, TimestampNonByteMultipleThrowsUnsupported) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.timestamp_enabled = true;
    layout.timestamp_size_bits = 20;  // 2.5 字节，非整字节，无法切分
    layout.header_bytes = 1;

    const auto dto = BytesOf({0x01, 0x00, 0x00, 0x00, 0x00});
    EXPECT_THROW(
        {
            try {
                (void)decoder.Decode(dto, layout);
            } catch (const XcpException& e) {
                EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
                throw;
            }
        },
        XcpException);
}

// --------------------------------------------------------------------------
// 畸形防御
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, PidOffRejected) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.pid_off = true;  // 无识别字段，B-7 必拒
    layout.header_bytes = 0;

    const auto dto = BytesOf({0x01, 0x02});
    EXPECT_THROW(
        {
            try {
                (void)decoder.Decode(dto, layout);
            } catch (const XcpException& e) {
                EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
                throw;
            }
        },
        XcpException);
}

TEST(DtoEnvelopeDecoder, ShortFrameThrows) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::RelativeByte;
    layout.header_bytes = 4;

    const auto dto = BytesOf({0x02, 0xAA, 0x0B});  // 短于 4 字节识别字段
    EXPECT_THROW((void)decoder.Decode(dto, layout), XcpException);
}

TEST(DtoEnvelopeDecoder, CounterOverflowThrows) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.counter_enabled = true;
    layout.header_bytes = 1;

    const auto dto = BytesOf({0x01});  // 缺 counter 字节
    EXPECT_THROW(
        {
            try {
                (void)decoder.Decode(dto, layout);
            } catch (const XcpException& e) {
                EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
                throw;
            }
        },
        XcpException);
}

TEST(DtoEnvelopeDecoder, EmptyPayloadIsValid) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.header_bytes = 1;

    const auto dto = BytesOf({0x03});  // 仅识别字段，无净荷
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.odt, 0x03U);
    EXPECT_TRUE(env.payload.empty());
}

}  // namespace
}  // namespace calmcar::xcp