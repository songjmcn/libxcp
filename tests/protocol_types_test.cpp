/**
 * @file protocol_types_test.cpp
 * @brief protocol_types.hpp 的单元测试：PID 分类、AG
 * 换算、资源掩码、地址推进、名称映射。
 */

#include "libxcp/protocol_types.hpp"

#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

namespace calmcar::xcp {
namespace {

// --------------------------------------------------------------------------
// ClassifyPacket：Slave -> Master PID 空间分类
// --------------------------------------------------------------------------

TEST(ClassifyPacket, RecognizesResErrEvServ) {
    EXPECT_EQ(ClassifyPacket(0xFF), std::optional<PacketType>(PacketType::Res));
    EXPECT_EQ(ClassifyPacket(0xFE), std::optional<PacketType>(PacketType::Err));
    EXPECT_EQ(ClassifyPacket(0xFD), std::optional<PacketType>(PacketType::Ev));
    EXPECT_EQ(ClassifyPacket(0xFC),
              std::optional<PacketType>(PacketType::Serv));
}

TEST(ClassifyPacket, TreatsDaqDtoRangeAsNullopt) {
    // 0x00..0xFB 属于 DAQ DTO，本阶段仅识别不解析
    EXPECT_EQ(ClassifyPacket(0x00), std::nullopt);
    EXPECT_EQ(ClassifyPacket(0x7F), std::nullopt);
    EXPECT_EQ(ClassifyPacket(0xFB), std::nullopt);
}

// --------------------------------------------------------------------------
// AG 换算与 COMM_MODE_BASIC 位域转换
// --------------------------------------------------------------------------

TEST(AddressGranularity, ConvertsToBytes) {
    EXPECT_EQ(AgToBytes(AddressGranularity::Byte), 1U);
    EXPECT_EQ(AgToBytes(AddressGranularity::Word), 2U);
    EXPECT_EQ(AgToBytes(AddressGranularity::DWord), 4U);
}

TEST(AddressGranularity, DecodesCommModeBasicField) {
    EXPECT_EQ(CommModeBasicToAg(0x00),
              std::optional<AddressGranularity>(AddressGranularity::Byte));
    EXPECT_EQ(CommModeBasicToAg(0x01),
              std::optional<AddressGranularity>(AddressGranularity::Word));
    EXPECT_EQ(CommModeBasicToAg(0x02),
              std::optional<AddressGranularity>(AddressGranularity::DWord));
    // 11 为保留值，必须判为非法
    EXPECT_EQ(CommModeBasicToAg(0x03), std::nullopt);
    // 高位应被忽略（调用方只传 bit1-2，但实现需自保）
    EXPECT_EQ(CommModeBasicToAg(0xE1),
              std::optional<AddressGranularity>(AddressGranularity::Word));
}

TEST(AddressGranularity, RoundTripsThroughCommModeBasicField) {
    for (auto ag : {AddressGranularity::Byte, AddressGranularity::Word,
                    AddressGranularity::DWord}) {
        EXPECT_EQ(CommModeBasicToAg(AgToCommModeBasicField(ag)),
                  std::optional<AddressGranularity>(ag));
    }
}

// --------------------------------------------------------------------------
// 错误码 / 事件码映射
// --------------------------------------------------------------------------

TEST(ErrorCodeMapping, NamesMatchXcpSpec) {
    EXPECT_EQ(ErrorCodeName(ErrorCode::CmdSynch), "ERR_CMD_SYNCH");
    EXPECT_EQ(ErrorCodeName(ErrorCode::CmdBusy), "ERR_CMD_BUSY");
    EXPECT_EQ(ErrorCodeName(ErrorCode::CmdUnknown), "ERR_CMD_UNKNOWN");
    EXPECT_EQ(ErrorCodeName(ErrorCode::AccessLocked), "ERR_ACCESS_LOCKED");
    EXPECT_EQ(ErrorCodeName(ErrorCode::ResourceTemporaryNotAccessible),
              "ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE");
}

TEST(ErrorCodeMapping, RejectsUnknownRawValues) {
    EXPECT_EQ(ToErrorCode(0x20),
              std::optional<ErrorCode>(ErrorCode::CmdUnknown));
    // 0x99 未在标准表中定义
    EXPECT_EQ(ToErrorCode(0x99), std::nullopt);
}

TEST(EventCodeMapping, NamesAndRawConversion) {
    EXPECT_EQ(EventCodeName(EventCode::CmdPending), "EV_CMD_PENDING");
    EXPECT_EQ(EventCodeName(EventCode::SessionTerminated),
              "EV_SESSION_TERMINATED");
    EXPECT_EQ(ToEventCode(0x05),
              std::optional<EventCode>(EventCode::CmdPending));
    // 0x04 在标准事件表中未定义
    EXPECT_EQ(ToEventCode(0x04), std::nullopt);
}

// --------------------------------------------------------------------------
// 资源掩码
// --------------------------------------------------------------------------

TEST(ResourceMask, ChecksBits) {
    // 规范示例 RESOURCE = 0x15 => CAL/PAG(bit0) + DAQ(bit2) + PGM(bit4)
    const ResourceMask mask = 0x15U;
    EXPECT_TRUE(HasResource(mask, Resource::CalPag));
    EXPECT_TRUE(HasResource(mask, Resource::Daq));
    EXPECT_FALSE(HasResource(mask, Resource::Stim));
    EXPECT_TRUE(HasResource(mask, Resource::Pgm));
    EXPECT_FALSE(HasResource(0x00U, Resource::CalPag));
}

TEST(DaqProcessorProperty, ChecksAndCombinesBits) {
    EXPECT_EQ(static_cast<std::uint8_t>(DaqProcessorPropertyBit::kConfigType),
              0x01U);
    EXPECT_EQ(static_cast<std::uint8_t>(DaqProcessorPropertyBit::kPrescaler),
              0x02U);
    EXPECT_EQ(static_cast<std::uint8_t>(DaqProcessorPropertyBit::kResume),
              0x04U);
    EXPECT_EQ(static_cast<std::uint8_t>(DaqProcessorPropertyBit::kBitStim),
              0x08U);
    EXPECT_EQ(static_cast<std::uint8_t>(DaqProcessorPropertyBit::kTimestamp),
              0x10U);
    EXPECT_EQ(static_cast<std::uint8_t>(DaqProcessorPropertyBit::kNoPid),
              0x20U);
    EXPECT_EQ(
        static_cast<std::uint8_t>(DaqProcessorPropertyBit::kOverloadIndicator),
        0xC0U);

    const auto props = DaqProcessorPropertyBit::kConfigType |
                       DaqProcessorPropertyBit::kTimestamp;
    EXPECT_TRUE(
        HasDaqProcessorProperty(props, DaqProcessorPropertyBit::kConfigType));
    EXPECT_TRUE(
        HasDaqProcessorProperty(props, DaqProcessorPropertyBit::kTimestamp));
    EXPECT_FALSE(
        HasDaqProcessorProperty(props, DaqProcessorPropertyBit::kNoPid));
}

TEST(ResourceMask, CombinesWithBitOr) {
    // operator| 只接受两个
    // Resource（返回底层掩码类型），因此第三次合并用内置整数 |
    const ResourceMask two = Resource::CalPag | Resource::Daq;
    const ResourceMask combined =
        two | static_cast<ResourceMask>(Resource::Pgm);
    EXPECT_EQ(combined, 0x15U);
    EXPECT_TRUE(HasResource(combined, Resource::CalPag));
    EXPECT_TRUE(HasResource(combined, Resource::Daq));
    EXPECT_FALSE(HasResource(combined, Resource::Stim));
    EXPECT_TRUE(HasResource(combined, Resource::Pgm));
}

// --------------------------------------------------------------------------
// XcpAddress40::Advance：按 AG 换算并检查 32 位溢出
// --------------------------------------------------------------------------

TEST(XcpAddress40, AdvancesByElementsInByteAg) {
    XcpAddress40 addr{0x70012340U, 0x02U};
    const auto next = addr.Advance(4, AddressGranularity::Byte);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->address, 0x70012344U);
    EXPECT_EQ(next->extension, 0x02U);  // 扩展位保持不变
}

TEST(XcpAddress40, AdvancesByElementsInWordAndDwordAg) {
    XcpAddress40 addr{0x1000U, 0x00U};
    EXPECT_EQ(addr.Advance(3, AddressGranularity::Word)->address, 0x1006U);
    EXPECT_EQ(addr.Advance(2, AddressGranularity::DWord)->address, 0x1008U);
}

TEST(XcpAddress40, DetectsOverflow) {
    XcpAddress40 near_end{0xFFFFFFFFU, 0x00U};
    EXPECT_EQ(near_end.Advance(1, AddressGranularity::Byte), std::nullopt);
    EXPECT_EQ(near_end.Advance(0, AddressGranularity::Byte)->address,
              0xFFFFFFFFU);

    XcpAddress40 last_byte{0xFFFFFFF0U, 0x00U};
    EXPECT_EQ(last_byte.Advance(4, AddressGranularity::DWord), std::nullopt);
    EXPECT_EQ(last_byte.Advance(3, AddressGranularity::DWord)->address,
              0xFFFFFFFCU);
}

TEST(XcpAddress40, EqualityComparesBothParts) {
    const XcpAddress40 base{0x10U, 0x01U};
    const XcpAddress40 same{0x10U, 0x01U};
    const XcpAddress40 other_extension{0x10U, 0x02U};
    const XcpAddress40 other_address{0x11U, 0x01U};
    // 初始化列表中的逗号会被 EXPECT_* 宏当作参数分隔，必须先落成命名变量
    EXPECT_TRUE(base == same);
    EXPECT_FALSE(base == other_extension);
    EXPECT_FALSE(base == other_address);
}

// --------------------------------------------------------------------------
// SessionState 名称
// --------------------------------------------------------------------------

TEST(SessionStateName, CoversAllStates) {
    EXPECT_EQ(SessionStateName(SessionState::Disconnected), "Disconnected");
    EXPECT_EQ(SessionStateName(SessionState::Connecting), "Connecting");
    EXPECT_EQ(SessionStateName(SessionState::Connected), "Connected");
    EXPECT_EQ(SessionStateName(SessionState::Disconnecting), "Disconnecting");
    EXPECT_EQ(SessionStateName(SessionState::Recovering), "Recovering");
    EXPECT_EQ(SessionStateName(SessionState::Failed), "Failed");
}

}  // namespace
}  // namespace calmcar::xcp
