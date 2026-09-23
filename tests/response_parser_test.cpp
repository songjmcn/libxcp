/**
 * @file response_parser_test.cpp
 * @brief ResponseParser 分类、字段解析与畸形包防护的单元测试。
 */

#include "libxcp/response_parser.hpp"

#include <cstdint>
#include <variant>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

/// @brief 构造测试用字节序列
Bytes BytesOf(std::initializer_list<std::uint8_t> init) { return Bytes(init); }

// --------------------------------------------------------------------------
// Packet 分类
// --------------------------------------------------------------------------

TEST(ResponseParserClassify, PositiveResponseKeepsBodyWithoutPid) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFF, 0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10}),
                     CommandCode::Connect);
    ASSERT_TRUE(parsed.has_value());
    const auto* res = std::get_if<PositiveResponse>(&*parsed);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->command, CommandCode::Connect);
    // data 不含 0xFF 前缀
    EXPECT_EQ(res->data, BytesOf({0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10}));
}

TEST(ResponseParserClassify, NegativeResponseWithAdditionalInfo) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFE, 0x25, 0xAA, 0xBB}), CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* err = std::get_if<NegativeResponse>(&*parsed);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->raw_error_code, 0x25U);
    EXPECT_EQ(err->error_code,
              std::optional<ErrorCode>(ErrorCode::AccessLocked));
    // 附加信息必须保留，不能丢弃
    EXPECT_EQ(err->additional_info, BytesOf({0xAA, 0xBB}));
}

TEST(ResponseParserClassify, UnknownErrorCodePreservesRawValue) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFE, 0x99}), CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* err = std::get_if<NegativeResponse>(&*parsed);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->raw_error_code, 0x99U);
    EXPECT_FALSE(err->error_code.has_value());
    EXPECT_TRUE(err->additional_info.empty());
}

TEST(ResponseParserClassify, EventPacket) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFD, 0x05, 0x01}), CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* ev = std::get_if<EventPacket>(&*parsed);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->event_code, std::optional<EventCode>(EventCode::CmdPending));
    EXPECT_EQ(ev->info, BytesOf({0x01}));
}

TEST(ResponseParserClassify, ServicePacket) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFC, 0x03, 0x11}), CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* serv = std::get_if<ServicePacket>(&*parsed);
    ASSERT_NE(serv, nullptr);
    EXPECT_EQ(serv->service_code, 0x03U);
    EXPECT_EQ(serv->data, BytesOf({0x11}));
}

TEST(ResponseParserClassify, DtoOnlyIdentifiedNotDecoded) {
    const ResponseParser parser(ByteOrder::Intel);
    // 初始化列表元素需与循环变量类型一致，避免窄化警告
    for (const std::uint8_t pid :
         {std::uint8_t{0x00}, std::uint8_t{0x20}, std::uint8_t{0xFB}}) {
        const auto parsed =
            parser.Parse(BytesOf({pid, 0xDE, 0xAD}), CommandCode::Upload);
        ASSERT_TRUE(parsed.has_value());
        const auto* dto = std::get_if<DtoPacket>(&*parsed);
        ASSERT_NE(dto, nullptr);
        EXPECT_EQ(dto->pid, pid);
        EXPECT_EQ(dto->data, BytesOf({0xDE, 0xAD}));
    }
}

// --------------------------------------------------------------------------
// 畸形包防护：不得越界读
// --------------------------------------------------------------------------

TEST(ResponseParserMalformed, EmptyPacketRejected) {
    const ResponseParser parser(ByteOrder::Intel);
    EXPECT_FALSE(parser.Parse(BytesView{}, CommandCode::Connect).has_value());
}

TEST(ResponseParserMalformed, TruncatedErrEvServRejected) {
    const ResponseParser parser(ByteOrder::Intel);
    // 只有 PID，缺少必需的 Byte 1
    EXPECT_FALSE(
        parser.Parse(BytesOf({0xFE}), CommandCode::Upload).has_value());
    EXPECT_FALSE(
        parser.Parse(BytesOf({0xFD}), CommandCode::Upload).has_value());
    EXPECT_FALSE(
        parser.Parse(BytesOf({0xFC}), CommandCode::Upload).has_value());
}

TEST(ResponseParserMalformed, BareResIsLegalButBodyEmpty) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed = parser.Parse(BytesOf({0xFF}), CommandCode::GetStatus);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(std::get<PositiveResponse>(*parsed).data.empty());
}

// --------------------------------------------------------------------------
// CONNECT 响应：规范示例 FF 15 C0 08 08 00 10 10
// --------------------------------------------------------------------------

TEST(ParseConnectResponse, SpecExampleIntel) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto resp = parser.ParseConnectResponse(
        BytesView{BytesOf({0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->resource_mask, 0x15U);
    EXPECT_TRUE(HasResource(resp->resource_mask, Resource::CalPag));
    EXPECT_TRUE(HasResource(resp->resource_mask, Resource::Daq));
    EXPECT_TRUE(HasResource(resp->resource_mask, Resource::Pgm));
    EXPECT_EQ(resp->byte_order, ByteOrder::Intel);
    EXPECT_EQ(resp->address_granularity, AddressGranularity::Byte);
    EXPECT_TRUE(resp->slave_block_mode_supported);
    EXPECT_TRUE(resp->optional_comm_mode_available);
    EXPECT_EQ(resp->max_cto, 8U);
    EXPECT_EQ(resp->max_dto, 8U);
    EXPECT_EQ(resp->protocol_layer_version, 0x10U);
    EXPECT_EQ(resp->transport_layer_version, 0x10U);
}

TEST(ParseConnectResponse, MotorolaByteOrderAndDwordAg) {
    const ResponseParser parser(ByteOrder::Motorola);
    // COMM_MODE_BASIC = 0xA5: bit0=1(Motorola), bit1-2=10(DWORD),
    // bit7=1(Optional) MAX_DTO = 0x0010 在大端下按 [hi][lo] 排布
    const auto resp = parser.ParseConnectResponse(
        BytesView{BytesOf({0x00, 0xA5, 0x10, 0x00, 0x10, 0x11, 0x01})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->byte_order, ByteOrder::Motorola);
    EXPECT_EQ(resp->address_granularity, AddressGranularity::DWord);
    EXPECT_FALSE(resp->slave_block_mode_supported);
    EXPECT_TRUE(resp->optional_comm_mode_available);
    EXPECT_EQ(resp->max_cto, 0x10U);
    EXPECT_EQ(resp->max_dto, 0x0010U);
    EXPECT_EQ(resp->protocol_layer_version, 0x11U);
    EXPECT_EQ(resp->transport_layer_version, 0x01U);
}

TEST(ParseConnectResponse, WordAgFromSpecBits) {
    const ResponseParser parser(ByteOrder::Intel);
    // bit1-2 = 01 => WORD
    const auto resp = parser.ParseConnectResponse(
        BytesView{BytesOf({0x00, 0x02, 0x08, 0x08, 0x00, 0x10, 0x10})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->address_granularity, AddressGranularity::Word);
}

TEST(ParseConnectResponse, ReservedAgBitsRejected) {
    const ResponseParser parser(ByteOrder::Intel);
    // bit1-2 = 11 为保留值，必须判为畸形而不是静默接受
    const auto resp = parser.ParseConnectResponse(
        BytesView{BytesOf({0x00, 0x06, 0x08, 0x08, 0x00, 0x10, 0x10})});
    EXPECT_FALSE(resp.has_value());
}

TEST(ParseConnectResponse, ShorterThanMinimumRejected) {
    const ResponseParser parser(ByteOrder::Intel);
    for (std::size_t n = 0; n < 7; ++n) {
        Bytes body(8, 0x00);
        body[1] = 0xC0;  // 保证 AG 合法，只测长度
        const auto resp = parser.ParseConnectResponse(BytesView{body}.first(n));
        EXPECT_FALSE(resp.has_value()) << "长度 " << n << " 应被拒绝";
    }
}

TEST(ParseConnectResponse, LongerThanMinimumAccepted) {
    const ResponseParser parser(ByteOrder::Intel);
    // 允许 Slave 追加后续版本字段，多余字节忽略
    const auto resp = parser.ParseConnectResponse(BytesView{
        BytesOf({0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10, 0xFF, 0xFF})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->max_cto, 8U);
}

// --------------------------------------------------------------------------
// GET_STATUS 响应
// --------------------------------------------------------------------------

TEST(ParseGetStatusResponse, DecodesAllStatusBits) {
    const ResponseParser parser(ByteOrder::Intel);
    // Session Status = 0xCA: bit7 RESUME, bit6 DAQ_RUNNING, bit3 CLEAR_DAQ_REQ,
    // bit2 STORE_DAQ_REQ Protection = 0x15, STATE_NUMBER = 0x04, ConfigID =
    // 0x1234 (Intel)
    const auto resp = parser.ParseGetStatusResponse(
        BytesView{BytesOf({0xCA, 0x15, 0x04, 0x34, 0x12})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(resp->resume);
    EXPECT_TRUE(resp->daq_running);
    EXPECT_TRUE(resp->clear_daq_req);
    EXPECT_TRUE(resp->store_daq_req);
    EXPECT_FALSE(resp->store_cal_req);
    EXPECT_EQ(resp->resource_protection, 0x15U);
    EXPECT_EQ(resp->state_number, 0x04U);
    EXPECT_EQ(resp->session_config_id, 0x1234U);
}

TEST(ParseGetStatusResponse, StoreCalReqBit0) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto resp = parser.ParseGetStatusResponse(
        BytesView{BytesOf({0x01, 0x00, 0x00, 0x00, 0x00})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(resp->store_cal_req);
    EXPECT_FALSE(resp->resume);
    EXPECT_FALSE(resp->daq_running);
}

TEST(ParseGetStatusResponse, RespectsSessionByteOrder) {
    const ResponseParser parser(ByteOrder::Motorola);
    const auto resp = parser.ParseGetStatusResponse(
        BytesView{BytesOf({0x00, 0x00, 0x00, 0x12, 0x34})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->session_config_id, 0x1234U);
}

TEST(ParseGetStatusResponse, TruncatedRejected) {
    const ResponseParser parser(ByteOrder::Intel);
    const Bytes body{0x00, 0x00, 0x00,
                     0x00};  // 只有 4 字节，缺 ConfigID 高字节
    EXPECT_FALSE(parser.ParseGetStatusResponse(BytesView{body}).has_value());
}

// --------------------------------------------------------------------------
// GET_COMM_MODE_INFO 响应
// --------------------------------------------------------------------------

TEST(ParseGetCommModeInfoResponse, DecodesFieldsAndDriverVersion) {
    const ResponseParser parser(ByteOrder::Intel);
    // [reserved][COMM_MODE_OPTIONAL][reserved][MAX_BS][MIN_ST][QUEUE_SIZE][DRIVER_VER]
    const auto resp = parser.ParseGetCommModeInfoResponse(
        BytesView{BytesOf({0x00, 0x2A, 0x00, 0x04, 0x02, 0x08, 0x13})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->comm_mode_optional, 0x2AU);
    EXPECT_EQ(resp->max_bs, 0x04U);
    EXPECT_EQ(resp->min_st, 0x02U);
    EXPECT_EQ(resp->queue_size, 0x08U);
    EXPECT_EQ(resp->driver_version_major, 1U);
    EXPECT_EQ(resp->driver_version_minor, 3U);
}

TEST(ParseGetCommModeInfoResponse, DriverVersionNibbles) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto resp = parser.ParseGetCommModeInfoResponse(
        BytesView{BytesOf({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->driver_version_major, 0x0FU);
    EXPECT_EQ(resp->driver_version_minor, 0x00U);
}

TEST(ParseGetCommModeInfoResponse, TruncatedRejected) {
    const ResponseParser parser(ByteOrder::Intel);
    const Bytes body{0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // 6 字节，缺驱动版本
    EXPECT_FALSE(
        parser.ParseGetCommModeInfoResponse(BytesView{body}).has_value());
}

TEST(ResponseParserBehavior, ReportsConfiguredByteOrder) {
    EXPECT_EQ(ResponseParser(ByteOrder::Intel).GetByteOrder(),
              ByteOrder::Intel);
    EXPECT_EQ(ResponseParser(ByteOrder::Motorola).GetByteOrder(),
              ByteOrder::Motorola);
}

}  // namespace
}  // namespace calmcar::xcp
