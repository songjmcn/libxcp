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

TEST(ResponseParserClassify, DtoDeliversWholeFrameWithPid) {
    // 批次14（R12 定稿）：DTO 的 data 含 PID（data[0]==pid），
    // 因为下游 IDaqLayout::Decode 的入参契约是"含 envelope 头的完整帧"
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
        EXPECT_EQ(dto->data, BytesOf({pid, 0xDE, 0xAD}));
        ASSERT_FALSE(dto->data.empty());
        EXPECT_EQ(dto->data[0], dto->pid)
            << "整帧契约：data[0] 必须与 pid 一致";
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

// --------------------------------------------------------------------------
// GET_SEED 响应（Seed&Key，批次 7）
// --------------------------------------------------------------------------

TEST(ParseGetSeedResponse, DecodesLengthAndSegment) {
    const ResponseParser parser(ByteOrder::Intel);
    // [length=16（总长）][seed 6 字节首段]
    const auto resp = parser.ParseGetSeedResponse(
        BytesView{BytesOf({0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->length, 0x10U);
    EXPECT_EQ(resp->seed, BytesOf({0x01, 0x02, 0x03, 0x04, 0x05, 0x06}));
}

TEST(ParseGetSeedResponse, ZeroLengthMeansUnprotected) {
    const ResponseParser parser(ByteOrder::Intel);
    // Length=0：资源未保护、无需 UNLOCK，Seed 为空
    const auto resp = parser.ParseGetSeedResponse(BytesView{BytesOf({0x00})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->length, 0U);
    EXPECT_TRUE(resp->seed.empty());
}

TEST(ParseGetSeedResponse, TruncatedRejected) {
    const ResponseParser parser(ByteOrder::Intel);
    const Bytes body{};  // 去掉 PID 后无任何字节
    EXPECT_FALSE(parser.ParseGetSeedResponse(BytesView{body}).has_value());
}

TEST(ParseGetSeedResponse, ByteOrderIndependent) {
    // 全部单字节字段：Motorola 解析结果必须与 Intel 完全一致
    const Bytes body{0x10, 0xAA, 0xBB};
    const auto intel =
        ResponseParser(ByteOrder::Intel).ParseGetSeedResponse(BytesView{body});
    const auto motorola = ResponseParser(ByteOrder::Motorola)
                              .ParseGetSeedResponse(BytesView{body});
    ASSERT_TRUE(intel.has_value());
    ASSERT_TRUE(motorola.has_value());
    EXPECT_EQ(intel->length, motorola->length);
    EXPECT_EQ(intel->seed, motorola->seed);
}

// --------------------------------------------------------------------------
// UNLOCK 响应（Seed&Key，批次 7）
// --------------------------------------------------------------------------

TEST(ParseUnlockResponse, DecodesProtectionMask) {
    const ResponseParser parser(ByteOrder::Intel);
    // [Current Resource Protection Status=0x1C]（CAL/DAQ/STIM/PGM 仍锁定）
    const auto resp = parser.ParseUnlockResponse(BytesView{BytesOf({0x1C})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->resource_protection, 0x1CU);
}

TEST(ParseUnlockResponse, ZeroMaskMeansAllUnlocked) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto resp = parser.ParseUnlockResponse(BytesView{BytesOf({0x00})});
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->resource_protection, 0U);
}

TEST(ParseUnlockResponse, TruncatedRejected) {
    const ResponseParser parser(ByteOrder::Intel);
    const Bytes body{};
    EXPECT_FALSE(parser.ParseUnlockResponse(BytesView{body}).has_value());
}

TEST(ResponseParserBehavior, ReportsConfiguredByteOrder) {
    EXPECT_EQ(ResponseParser(ByteOrder::Intel).GetByteOrder(),
              ByteOrder::Intel);
    EXPECT_EQ(ResponseParser(ByteOrder::Motorola).GetByteOrder(),
              ByteOrder::Motorola);
}

// --------------------------------------------------------------------------
// 批次14（T14-13）：DAQ 命令响应解析（PR 布局 + 截断负例）
// --------------------------------------------------------------------------

TEST(ParseDaqResponses, StartStopDaqListCarriesFirstPid) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto parsed = parser.ParseStartStopDaqListResponse(BytesOf({0x21}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first_pid, 0x21U);
    // 截断（无 FIRST_PID）必须判畸形
    EXPECT_FALSE(parser.ParseStartStopDaqListResponse(BytesView{}).has_value());
}

TEST(ParseDaqResponses, GetDaqListInfoFields) {
    const ResponseParser parser(ByteOrder::Intel);
    // [PROPERTIES][MAX_ODT][MAX_ODT_ENTRY][FIXED_EVENT(WORD 小端)]
    const auto parsed =
        parser.ParseGetDaqListInfo(BytesOf({0x05, 0x04, 0x08, 0xE8, 0x03}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(static_cast<unsigned>(parsed->properties), 0x05U);
    EXPECT_TRUE(
        HasDaqProperty(parsed->properties, DaqListPropertyBit::kPredefined));
    EXPECT_TRUE(
        HasDaqProperty(parsed->properties, DaqListPropertyBit::kDirectionDaq));
    EXPECT_FALSE(
        HasDaqProperty(parsed->properties, DaqListPropertyBit::kDirectionStim));
    EXPECT_EQ(parsed->max_odt, 4U);
    EXPECT_EQ(parsed->max_odt_entries, 8U);
    EXPECT_EQ(parsed->fixed_event, 0x03E8U);
    // 少一字节即畸形（FIXED_EVENT 为 WORD）
    EXPECT_FALSE(parser.ParseGetDaqListInfo(BytesOf({0x05, 0x04, 0x08, 0xE8}))
                     .has_value());
}

TEST(ParseDaqResponses, GetDaqResolutionInfoSplitsTimestampMode) {
    const ResponseParser parser(ByteOrder::Intel);
    // [GRAN_DAQ][MAX_DAQ][GRAN_STIM][MAX_STIM][TS_MODE][TICKS(WORD 小端)]
    const auto parsed = parser.ParseGetDaqResolutionInfo(
        BytesOf({0x01, 0x08, 0x01, 0x08, 0x39, 0xE8, 0x03}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->granularity_daq, 1U);
    EXPECT_EQ(parsed->max_odt_entry_size_daq, 8U);
    EXPECT_EQ(parsed->granularity_stim, 1U);
    EXPECT_EQ(parsed->max_odt_entry_size_stim, 8U);
    // 0x39 = UNIT 3<<4 | FIXED 0x08 | TYPE 1 → 只拆位，不换算单位（R13 无表）
    EXPECT_TRUE(parsed->timestamp_mode.fixed);
    EXPECT_EQ(parsed->timestamp_mode.size_code, 1U);
    EXPECT_EQ(parsed->timestamp_mode.unit_code, 3U);
    EXPECT_EQ(parsed->timestamp_mode.raw, 0x39U);
    EXPECT_EQ(parsed->timestamp_ticks, 0x03E8U);
    EXPECT_FALSE(parser
                     .ParseGetDaqResolutionInfo(
                         BytesOf({0x01, 0x08, 0x01, 0x08, 0x39, 0xE8}))
                     .has_value());
}

TEST(ParseDaqResponses, GetDaqProcessorInfoSplitsKeyByte) {
    const ResponseParser parser(ByteOrder::Intel);
    // [PROPERTIES][MAX_DAQ(WORD)][MAX_EVENT(WORD)][MIN_DAQ][KEY_BYTE]
    const auto parsed = parser.ParseGetDaqProcessorInfo(
        BytesOf({0x02, 0x04, 0x00, 0x02, 0x00, 0x00, 0x30}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->properties, 0x02U);
    EXPECT_EQ(parsed->max_daq, 4U);
    EXPECT_EQ(parsed->max_event_channel, 2U);
    EXPECT_EQ(parsed->min_daq, 0U);
    // 0x30 = ADDRESS_EXTENSION=DAQ(3<<4)、IDENTIFICATION=ABSOLUTE(0<<6)
    EXPECT_EQ(parsed->key_byte.optimisation_type, 0U);
    EXPECT_EQ(parsed->key_byte.address_extension_mode, 3U);
    EXPECT_EQ(parsed->key_byte.identification_field_type, 0U);
    EXPECT_FALSE(parser
                     .ParseGetDaqProcessorInfo(
                         BytesOf({0x02, 0x04, 0x00, 0x02, 0x00, 0x00}))
                     .has_value());
}

TEST(ParseDaqResponses, ReadDaqReturnsEntryDescriptor) {
    const ResponseParser parser(ByteOrder::Intel);
    // [BITOFFSET][SIZE][EXT][ADDR(DWORD 小端)]
    const auto parsed = parser.ParseReadDaq(
        BytesOf({0xFF, 0x04, 0x12, 0x08, 0x55, 0x0C, 0x00}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->bit_offset, kDaqBitOffsetNone);
    EXPECT_EQ(parsed->size, 4U);
    EXPECT_EQ(parsed->address_extension, 0x12U);
    EXPECT_EQ(parsed->address, 0x000C5508U);
    EXPECT_FALSE(parser.ParseReadDaq(BytesOf({0xFF, 0x04, 0x12, 0x08, 0x55}))
                     .has_value());
}

TEST(ParseDaqResponses, MotorolaByteOrderReversesMultiByteFields) {
    const ResponseParser parser(ByteOrder::Motorola);
    const auto parsed = parser.ParseReadDaq(
        BytesOf({0xFF, 0x04, 0x12, 0x00, 0x0C, 0x55, 0x08}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->address, 0x000C5508U);
    const auto info =
        parser.ParseGetDaqListInfo(BytesOf({0x00, 0x00, 0x00, 0x03, 0xE8}));
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->fixed_event, 0x03E8U);
}

TEST(ParseDaqResponses, DaqKeyByteParserIsPureBitMath) {
    // ParseDaqKeyByte 与 ParseDaqTimestampMode 是纯位运算，单独锁定位段
    const DaqKeyByte key = ParseDaqKeyByte(0xD5U);
    EXPECT_EQ(key.optimisation_type, 0x05U);          // bit0-3
    EXPECT_EQ(key.address_extension_mode, 0x01U);     // bit4-5
    EXPECT_EQ(key.identification_field_type, 0x03U);  // bit6-7
    const DaqTimestampMode ts = ParseDaqTimestampMode(0x39U);
    EXPECT_EQ(ts.size_code, 1U);
    EXPECT_TRUE(ts.fixed);
    EXPECT_EQ(ts.unit_code, 3U);
    EXPECT_TRUE(HasDaqMode(DaqListModeBit::kStim | DaqListModeBit::kTimestamp,
                           DaqListModeBit::kTimestamp));
    EXPECT_FALSE(HasDaqMode(DaqListModeBit::kStim, DaqListModeBit::kPidOff));
}

// --------------------------------------------------------------------------
// 变量标定批次：Calibration / Page Switching 响应（docs §7.5.2.5 / §7.5.3；
// CRM 布局对照 thirdparty/XCPlite/src/xcp.h:604-660）
// --------------------------------------------------------------------------

TEST(ParseCalibrationResponses, GetCalPageCarriesLogicalPage) {
    const ResponseParser parser(ByteOrder::Intel);
    // [reserved][reserved][PAGE_NUMBER]（xcp.h CRM_GET_CAL_PAGE_LEN=4）
    const auto resp = parser.ParseGetCalPage(BytesOf({0x00, 0x00, 0x07}));
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->page, 0x07U);
    // 截断（少于 3 字节）拒绝
    EXPECT_FALSE(parser.ParseGetCalPage(BytesOf({0x00, 0x01})).has_value());
}

TEST(ParseCalibrationResponses, GetPagProcessorInfoDecodesSegmentAndProps) {
    const ResponseParser parser(ByteOrder::Intel);
    // [MAX_SEGMENT][PAG_PROPERTIES]（FREEZE_SUPPORTED=bit0）
    const auto resp = parser.ParseGetPagProcessorInfo(BytesOf({0x04, 0x01}));
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->max_segment, 0x04U);
    EXPECT_TRUE(HasPagProperty(resp->properties, PagPropertyBit::kFreezeSupported));
    const auto no_freeze =
        parser.ParseGetPagProcessorInfo(BytesOf({0x01, 0x00}));
    ASSERT_TRUE(no_freeze.has_value());
    EXPECT_FALSE(
        HasPagProperty(no_freeze->properties, PagPropertyBit::kFreezeSupported));
    EXPECT_FALSE(parser.ParseGetPagProcessorInfo(BytesOf({0x04})).has_value());
}

TEST(ParseCalibrationResponses, GetSegmentInfoMode0ReadsDwordIntel) {
    const ResponseParser parser(ByteOrder::Intel);
    // Mode 0/2 剥 FF 后为 DWORD@0..3（CRM 总长 8，多余填充忽略）
    const auto addr = parser.ParseGetSegmentInfo(
        BytesOf({0x00, 0x31, 0x00, 0x00, 0x00, 0x00}),
        SegmentInfoMode::BasicInfo);
    ASSERT_TRUE(addr.has_value());
    const auto* basic = std::get_if<SegmentBasicInfo>(&addr->data);
    ASSERT_NE(basic, nullptr);
    EXPECT_EQ(basic->value, 0x00003100U);
    // 截断（<4 字节）拒绝
    EXPECT_FALSE(parser
                     .ParseGetSegmentInfo(BytesOf({0x00, 0x31, 0x00}),
                                          SegmentInfoMode::BasicInfo)
                     .has_value());
}

TEST(ParseCalibrationResponses, GetSegmentInfoMode0MotorolaByteOrder) {
    const ResponseParser parser(ByteOrder::Motorola);
    const auto len = parser.ParseGetSegmentInfo(
        BytesOf({0x00, 0x01, 0x00, 0x00}), SegmentInfoMode::BasicInfo);
    ASSERT_TRUE(len.has_value());
    const auto* basic = std::get_if<SegmentBasicInfo>(&len->data);
    ASSERT_NE(basic, nullptr);
    EXPECT_EQ(basic->value, 0x00010000U);
}

TEST(ParseCalibrationResponses, GetSegmentInfoMode1StandardProperties) {
    const ResponseParser parser(ByteOrder::Intel);
    // [MAX_PAGES][ADDRESS_EXTENSION][MAX_MAPPING][COMPRESSION][ENCRYPTION]
    const auto resp = parser.ParseGetSegmentInfo(
        BytesOf({0x08, 0x02, 0x03, 0x01, 0x00}),
        SegmentInfoMode::StandardProperties);
    ASSERT_TRUE(resp.has_value());
    const auto* props = std::get_if<SegmentStandardProperties>(&resp->data);
    ASSERT_NE(props, nullptr);
    EXPECT_EQ(props->max_pages, 0x08U);
    EXPECT_EQ(props->address_extension, 0x02U);
    EXPECT_EQ(props->max_mapping, 0x03U);
    EXPECT_EQ(props->compression_method, 0x01U);
    EXPECT_EQ(props->encryption_method, 0x00U);
    // Mode 1 最小长度 5，截断拒绝
    EXPECT_FALSE(parser
                     .ParseGetSegmentInfo(BytesOf({0x08, 0x02, 0x03, 0x01}),
                                          SegmentInfoMode::StandardProperties)
                     .has_value());
}

TEST(ParseCalibrationResponses, GetSegmentInfoMode2YieldsMappingVariant) {
    const ResponseParser parser(ByteOrder::Intel);
    const auto resp =
        parser.ParseGetSegmentInfo(BytesOf({0x10, 0x20, 0x00, 0x00}),
                                   SegmentInfoMode::MappingInfo);
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(std::holds_alternative<SegmentMappingInfo>(resp->data));
    EXPECT_EQ(std::get<SegmentMappingInfo>(resp->data).value, 0x00002010U);
}

TEST(ParseCalibrationResponses, GetPageInfoSplitsPageProperties) {
    const ResponseParser parser(ByteOrder::Intel);
    // PAGE_PROPERTIES: ECU_ACCESS=3(bits0-1)、XCP_READ=1(bits2-3)、
    //                  XCP_WRITE=2(bits4-5) → 0b10_01_11 = 0x27
    const auto resp = parser.ParseGetPageInfo(BytesOf({0x27, 0x05}));
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->properties.ecu_access, PageAccessType::DontCare);
    EXPECT_EQ(resp->properties.xcp_read_access,
              PageAccessType::WithoutOtherAccess);
    EXPECT_EQ(resp->properties.xcp_write_access,
              PageAccessType::WithConcurrentAccess);
    EXPECT_EQ(resp->init_segment, 0x05U);
    EXPECT_FALSE(parser.ParseGetPageInfo(BytesOf({0x27})).has_value());
}

TEST(ParseCalibrationResponses, GetSegmentModeReadsModeAtOffsetOne) {
    const ResponseParser parser(ByteOrder::Intel);
    // [reserved][MODE]（xcp.h CRM_GET_SEGMENT_MODE_LEN=3）
    const auto frozen = parser.ParseGetSegmentMode(BytesOf({0x00, 0x01}));
    ASSERT_TRUE(frozen.has_value());
    EXPECT_TRUE(HasSegmentMode(frozen->mode, SegmentModeBit::kFreeze));
    const auto running = parser.ParseGetSegmentMode(BytesOf({0x00, 0x00}));
    ASSERT_TRUE(running.has_value());
    EXPECT_FALSE(HasSegmentMode(running->mode, SegmentModeBit::kFreeze));
    EXPECT_FALSE(parser.ParseGetSegmentMode(BytesOf({0x00})).has_value());
}

}  // namespace
}  // namespace calmcar::xcp
