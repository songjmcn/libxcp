/**
 * @file dto_envelope_decoder_test.cpp
 * @brief DtoEnvelopeDecoder 四种识别模式、计数器/时间戳切分与畸形帧防御的单元测试。
 *
 * 依据 code-plan/libxcp_测量子系统代码增长计划.md §5.1 / §6（v0.2）。
 * 覆盖：
 *  - Absolute（绝对 ODT 号）
 *  - RelativeByte（PID + DAQ(BYTE)，2 字节）
 *  - RelativeWord（PID + DAQ(WORD)，3 字节）
 *  - RelativeWordAligned（PID + FILL + DAQ(WORD)，4 字节；FILL 取值自由）
 *  - counter / timestamp 尾段切分与位宽合法性（含 CTR 取代 FILL）
 *  - 会话字节序（Intel/Motorola）对 DAQ 字段与时间戳的影响
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

TEST(DtoEnvelopeDecoder, AbsolutePidUsesFirstPidAndRejectsEarlierPid) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.first_odt = 0x20;
    layout.header_bytes = 1;

    const auto dto = BytesOf({0x22, 0xDE, 0xAD});
    const DtoEnvelope env = decoder.Decode(dto, layout);
    EXPECT_EQ(env.identity.odt, 2U);
    ASSERT_EQ(env.payload.size(), 2U);
    EXPECT_THROW((void)decoder.Decode(BytesOf({0x1F, 0xDE}), layout), XcpException);
}

// --------------------------------------------------------------------------
// RelativeByte / RelativeWord / RelativeWordAligned
// 原始 PDF 第 95-96 页 Figure 51/52/53：识别字段恒为 PID（相对 ODT）在前，
// DAQ 字段在后；FILL 仅用于对齐且取值自由。
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, RelativeByteLayoutIsPidThenDaqByte) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::RelativeByte;
    layout.first_odt = 0x10;
    layout.header_bytes = 2;  // [PID(relODT)][DAQ(BYTE)]

    const auto dto = BytesOf({0x02, 0x0B, 0x10, 0x20});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.daq_list, 0x0BU);     // DAQ 在 byte1
    EXPECT_EQ(env.identity.odt, 0x10U + 0x02U);  // PID 在 byte0
    ASSERT_EQ(env.payload.size(), 2U);
    EXPECT_EQ(env.payload[0], 0x10);
    EXPECT_EQ(env.payload[1], 0x20);
}

TEST(DtoEnvelopeDecoder, RelativeWordAlignedAcceptsAnyFillValue) {
    // 规范只说 "an extra FILL byte"，不约束其取值。旧实现把 0xAA 当强制标志
    // 校验，会拒绝任何使用其它填充值的合规 Slave。
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type =
        IdentificationFieldType::RelativeWordAligned;
    layout.first_odt = 0x10;
    layout.header_bytes = 4;

    const auto with_aa = BytesOf({0x02, 0xAA, 0x0B, 0x00, 0x77});
    const auto with_zero = BytesOf({0x02, 0x00, 0x0B, 0x00, 0x77});
    const auto with_ff = BytesOf({0x02, 0xFF, 0x0B, 0x00, 0x77});

    for (const auto* frame : {&with_aa, &with_zero, &with_ff}) {
        const DtoEnvelope env = decoder.Decode(*frame, layout);
        EXPECT_EQ(env.identity.daq_list, 0x000BU);
        EXPECT_EQ(env.identity.odt, 0x12U);
        ASSERT_EQ(env.payload.size(), 1U);
        EXPECT_EQ(env.payload[0], 0x77);
    }
}

// --------------------------------------------------------------------------
// RelativeWord / RelativeWordAligned
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, RelativeWordLayoutIsPidThenDaqWord) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::RelativeWord;
    layout.first_odt = 0x30;
    layout.header_bytes = 3;  // [PID(relODT)][DAQ(WORD)]

    // PID(relODT)=0x02，DAQ(WORD LE)=0x0005。
    const auto dto = BytesOf({0x02, 0x05, 0x00, 0xAA, 0xBB});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.daq_list, 0x0005U);
    EXPECT_EQ(env.identity.odt, 0x30U + 0x02U);
    ASSERT_EQ(env.payload.size(), 2U);
    EXPECT_EQ(env.payload[0], 0xAA);
    EXPECT_EQ(env.payload[1], 0xBB);
}

TEST(DtoEnvelopeDecoder, RelativeWordAlignedHasFillBeforeDaqWord) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type =
        IdentificationFieldType::RelativeWordAligned;
    layout.first_odt = 0x30;
    layout.header_bytes = 4;  // [PID(relODT)][FILL][DAQ(WORD)]

    // PID=0x02，FILL=0xAA，DAQ(WORD LE)=0x0005。
    const auto dto = BytesOf({0x02, 0xAA, 0x05, 0x00, 0x12});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.daq_list, 0x0005U);
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
    layout.header_bytes = 2;

    const auto dto = BytesOf({0x02});  // 短于 2 字节识别字段
    EXPECT_THROW((void)decoder.Decode(dto, layout), XcpException);
}

TEST(DtoEnvelopeDecoder, UnknownIdentificationModeThrows) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type =
        static_cast<IdentificationFieldType>(0xFFU);
    layout.header_bytes = 4;

    const auto dto = BytesOf({0x02, 0xAA, 0x0B, 0x00});
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

// --------------------------------------------------------------------------
// F-06：CTR 取代 FILL 字节（原始 PDF 第 97-98 页 Figure 56）
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, AlignedCounterReplacesFillByte) {
    // (WORD, aligned) 下 CTR 占用识别字段内的 FILL 槽位（偏移 1），
    // 不额外增加帧长——这正是该模式存在的目的（保持对齐且不引入额外填充）。
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type =
        IdentificationFieldType::RelativeWordAligned;
    layout.first_odt = 0x00;
    layout.counter_enabled = true;
    layout.header_bytes = 4;

    // [PID=0x00][CTR=0x42][DAQ WORD LE=0x0007][payload]
    const auto dto = BytesOf({0x00, 0x42, 0x07, 0x00, 0x99});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    ASSERT_TRUE(env.counter.has_value());
    EXPECT_EQ(*env.counter, 0x42U);       // CTR 来自 FILL 槽位
    EXPECT_EQ(env.identity.daq_list, 0x0007U);
    ASSERT_EQ(env.payload.size(), 1U);    // 净荷仍自 byte4，未因 CTR 后移
    EXPECT_EQ(env.payload[0], 0x99);
}

TEST(DtoEnvelopeDecoder, NonAlignedCounterSitsAfterIdentificationField) {
    // 其余模式下 CTR 位于识别字段**之后**，额外占 1 字节。
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::RelativeByte;
    layout.counter_enabled = true;
    layout.header_bytes = 2;

    const auto dto = BytesOf({0x00, 0x07, 0x42, 0x99});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    ASSERT_TRUE(env.counter.has_value());
    EXPECT_EQ(*env.counter, 0x42U);
    EXPECT_EQ(env.identity.daq_list, 0x0007U);
    ASSERT_EQ(env.payload.size(), 1U);
    EXPECT_EQ(env.payload[0], 0x99);
}

// --------------------------------------------------------------------------
// F-07：会话字节序影响 DAQ 字段与时间戳（不再固定小端）
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, MotorolaByteOrderDecodesDaqWordBigEndian) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Motorola);
    DtoFrameLayout layout;
    layout.identification_field_type =
        IdentificationFieldType::RelativeWordAligned;
    layout.header_bytes = 4;

    // DAQ WORD 大端：0x0102 → 字节序 0x01 0x02。
    const auto dto = BytesOf({0x00, 0xAA, 0x01, 0x02, 0x77});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    EXPECT_EQ(env.identity.daq_list, 0x0102U);
    ASSERT_EQ(env.payload.size(), 1U);
    EXPECT_EQ(env.payload[0], 0x77);
}

TEST(DtoEnvelopeDecoder, MotorolaByteOrderDecodesTimestampBigEndian) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Motorola);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.header_bytes = 1;
    layout.timestamp_enabled = true;
    layout.timestamp_size_bits = 32;

    // 大端时间戳 0x01020304。
    const auto dto = BytesOf({0x00, 0x01, 0x02, 0x03, 0x04});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    ASSERT_TRUE(env.raw_timestamp.has_value());
    EXPECT_EQ(*env.raw_timestamp, 0x01020304ULL);
}

TEST(DtoEnvelopeDecoder, IntelByteOrderDecodesTimestampLittleEndian) {
    const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    DtoFrameLayout layout;
    layout.identification_field_type = IdentificationFieldType::Absolute;
    layout.header_bytes = 1;
    layout.timestamp_enabled = true;
    layout.timestamp_size_bits = 32;

    const auto dto = BytesOf({0x00, 0x04, 0x03, 0x02, 0x01});
    const DtoEnvelope env = decoder.Decode(dto, layout);

    ASSERT_TRUE(env.raw_timestamp.has_value());
    EXPECT_EQ(*env.raw_timestamp, 0x01020304ULL);
}

// --------------------------------------------------------------------------
// F-08：DAQ_KEY_BYTE → 识别字段模式映射（规范 Table 143 / Figure 51-53）
// --------------------------------------------------------------------------

TEST(DtoEnvelopeDecoder, KeyByteMapsToSpecIdentificationFieldType) {
    EXPECT_EQ(IdentificationFieldTypeFromKeyByte(0U),
              IdentificationFieldType::Absolute);
    EXPECT_EQ(IdentificationFieldTypeFromKeyByte(1U),
              IdentificationFieldType::RelativeByte);
    EXPECT_EQ(IdentificationFieldTypeFromKeyByte(2U),
              IdentificationFieldType::RelativeWord);
    // wire code 3（XCPlite 申报的 DAQ_HDR_ODT_FIL_DAQW）是标准 (WORD, aligned)。
    EXPECT_EQ(IdentificationFieldTypeFromKeyByte(3U),
              IdentificationFieldType::RelativeWordAligned);
    // 4..255 非法。
    EXPECT_FALSE(IdentificationFieldTypeFromKeyByte(4U).has_value());
    EXPECT_FALSE(IdentificationFieldTypeFromKeyByte(0xFFU).has_value());
}

TEST(DtoEnvelopeDecoder, MinimumBytesMatchSpecFigures) {
    EXPECT_EQ(IdentificationFieldMinimumBytes(
                  IdentificationFieldType::Absolute),
              1U);
    EXPECT_EQ(IdentificationFieldMinimumBytes(
                  IdentificationFieldType::RelativeByte),
              2U);
    EXPECT_EQ(IdentificationFieldMinimumBytes(
                  IdentificationFieldType::RelativeWord),
              3U);
    EXPECT_EQ(IdentificationFieldMinimumBytes(
                  IdentificationFieldType::RelativeWordAligned),
              4U);
}

TEST(DtoEnvelopeDecoder, OnlyAlignedModeMergesCounterIntoIdentification) {
    EXPECT_TRUE(IdentificationFieldMergesCounter(
        IdentificationFieldType::RelativeWordAligned));
    EXPECT_FALSE(IdentificationFieldMergesCounter(
        IdentificationFieldType::Absolute));
    EXPECT_FALSE(IdentificationFieldMergesCounter(
        IdentificationFieldType::RelativeByte));
    EXPECT_FALSE(IdentificationFieldMergesCounter(
        IdentificationFieldType::RelativeWord));
}

}  // namespace
}  // namespace calmcar::xcp