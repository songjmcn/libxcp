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
    EXPECT_EQ(res->m_command_, CommandCode::Connect);
    // data 不含 0xFF 前缀
    EXPECT_EQ(res->m_data_,
              BytesOf({0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10}));
}

TEST(ResponseParserClassify, NegativeResponseWithAdditionalInfo) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFE, 0x25, 0xAA, 0xBB}), CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* err = std::get_if<NegativeResponse>(&*parsed);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->m_raw_error_code_, 0x25U);
    EXPECT_EQ(err->m_error_code_,
              std::optional<ErrorCode>(ErrorCode::AccessLocked));
    // 附加信息必须保留，不能丢弃
    EXPECT_EQ(err->m_additional_info_, BytesOf({0xAA, 0xBB}));
}

TEST(ResponseParserClassify, UnknownErrorCodePreservesRawValue) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFE, 0x99}), CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* err = std::get_if<NegativeResponse>(&*parsed);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->m_raw_error_code_, 0x99U);
    EXPECT_FALSE(err->m_error_code_.has_value());
    EXPECT_TRUE(err->m_additional_info_.empty());
}

TEST(ResponseParserClassify, EventPacket) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFD, 0x05, 0x01}), CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* ev = std::get_if<EventPacket>(&*parsed);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->m_event_code_,
              std::optional<EventCode>(EventCode::CmdPending));
    EXPECT_EQ(ev->m_info_, BytesOf({0x01}));
}

TEST(ResponseParserClassify, ServicePacket) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed =
        parser.Parse(BytesOf({0xFC, 0x03, 0x11}), CommandCode::Upload);
    ASSERT_TRUE(parsed.has_value());
    const auto* serv = std::get_if<ServicePacket>(&*parsed);
    ASSERT_NE(serv, nullptr);
    EXPECT_EQ(serv->m_service_code_, 0x03U);
    EXPECT_EQ(serv->m_data_, BytesOf({0x11}));
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
        EXPECT_EQ(dto->m_pid_, pid);
        EXPECT_EQ(dto->m_data_, BytesOf({0xDE, 0xAD}));
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
    EXPECT_TRUE(std::get<PositiveResponse>(*parsed).m_data_.empty());
}

// --------------------------------------------------------------------------
// CONNECT 响应：规范示例 FF 15 C0 08 08 00 10 10
// --------------------------------------------------------------------------

TEST(ParseConnectResponse, SpecExampleIntel) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto resp = parser.ParseConnectResponse(
        BytesView{BytesOf({0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->m_resource_mask_, 0x15U);
    EXPECT_TRUE(HasResource(resp->m_resource_mask_, Resource::CalPag));
    EXPECT_TRUE(HasResource(resp->m_resource_mask_, Resource::Daq));
    EXPECT_TRUE(HasResource(resp->m_resource_mask_, Resource::Pgm));
    EXPECT_EQ(resp->m_byte_order_, ByteOrder::Intel);
    EXPECT_EQ(resp->m_address_granularity_, AddressGranularity::Byte);
    EXPECT_TRUE(resp->m_slave_block_mode_supported_);
    EXPECT_TRUE(resp->m_optional_comm_mode_available_);
    EXPECT_EQ(resp->m_max_cto_, 8U);
    EXPECT_EQ(resp->m_max_dto_, 8U);
    EXPECT_EQ(resp->m_protocol_layer_version_, 0x10U);
    EXPECT_EQ(resp->m_transport_layer_version_, 0x10U);
}

TEST(ParseConnectResponse, MotorolaByteOrderAndDwordAg) {
    const ResponseParser parser(ByteOrder::Motorola);
    // COMM_MODE_BASIC = 0xA5: bit0=1(Motorola), bit1-2=10(DWORD),
    // bit7=1(Optional) MAX_DTO = 0x0010 在大端下按 [hi][lo] 排布
    const auto resp = parser.ParseConnectResponse(
        BytesView{BytesOf({0x00, 0xA5, 0x10, 0x00, 0x10, 0x11, 0x01})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->m_byte_order_, ByteOrder::Motorola);
    EXPECT_EQ(resp->m_address_granularity_, AddressGranularity::DWord);
    EXPECT_FALSE(resp->m_slave_block_mode_supported_);
    EXPECT_TRUE(resp->m_optional_comm_mode_available_);
    EXPECT_EQ(resp->m_max_cto_, 0x10U);
    EXPECT_EQ(resp->m_max_dto_, 0x0010U);
    EXPECT_EQ(resp->m_protocol_layer_version_, 0x11U);
    EXPECT_EQ(resp->m_transport_layer_version_, 0x01U);
}

TEST(ParseConnectResponse, WordAgFromSpecBits) {
    const ResponseParser parser(ByteOrder::Intel);
    // bit1-2 = 01 => WORD
    const auto resp = parser.ParseConnectResponse(
        BytesView{BytesOf({0x00, 0x02, 0x08, 0x08, 0x00, 0x10, 0x10})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->m_address_granularity_, AddressGranularity::Word);
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
    EXPECT_EQ(resp->m_max_cto_, 8U);
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
    EXPECT_TRUE(resp->m_resume_);
    EXPECT_TRUE(resp->m_daq_running_);
    EXPECT_TRUE(resp->m_clear_daq_req_);
    EXPECT_TRUE(resp->m_store_daq_req_);
    EXPECT_FALSE(resp->m_store_cal_req_);
    EXPECT_EQ(resp->m_resource_protection_, 0x15U);
    EXPECT_EQ(resp->m_state_number_, 0x04U);
    EXPECT_EQ(resp->m_session_config_id_, 0x1234U);
}

TEST(ParseGetStatusResponse, StoreCalReqBit0) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto resp = parser.ParseGetStatusResponse(
        BytesView{BytesOf({0x01, 0x00, 0x00, 0x00, 0x00})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(resp->m_store_cal_req_);
    EXPECT_FALSE(resp->m_resume_);
    EXPECT_FALSE(resp->m_daq_running_);
}

TEST(ParseGetStatusResponse, RespectsSessionByteOrder) {
    const ResponseParser parser(ByteOrder::Motorola);
    const auto resp = parser.ParseGetStatusResponse(
        BytesView{BytesOf({0x00, 0x00, 0x00, 0x12, 0x34})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->m_session_config_id_, 0x1234U);
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
    EXPECT_EQ(resp->m_comm_mode_optional_, 0x2AU);
    EXPECT_EQ(resp->m_max_bs_, 0x04U);
    EXPECT_EQ(resp->m_min_st_, 0x02U);
    EXPECT_EQ(resp->m_queue_size_, 0x08U);
    EXPECT_EQ(resp->m_driver_version_major_, 1U);
    EXPECT_EQ(resp->m_driver_version_minor_, 3U);
}

TEST(ParseGetCommModeInfoResponse, DriverVersionNibbles) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto resp = parser.ParseGetCommModeInfoResponse(
        BytesView{BytesOf({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->m_driver_version_major_, 0x0FU);
    EXPECT_EQ(resp->m_driver_version_minor_, 0x00U);
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
