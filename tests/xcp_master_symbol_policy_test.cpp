#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "libxcp/a2l/a2l_types.hpp"
#include "tools/xcp_master/calibration_policy.hpp"

namespace {

using calmcar::xcp::PageAccessType;
using calmcar::xcp::a2l::CharacteristicType;
using calmcar::xcp::a2l::FixedCurveAxisInfo;
using calmcar::xcp::a2l::MemoryPageInfo;
using calmcar::xcp::a2l::MemorySegmentInfo;
using calmcar::xcp::a2l::SymbolInfo;
using calmcar::xcp::a2l::SymbolKind;
using calmcar::xcp::a2l::XcpPageWriteAccess;
using calmcar::xcp::tools::CalibrationPageWriteUnsupportedReason;
using calmcar::xcp::tools::CurvePointWriteUnsupportedReason;
using calmcar::xcp::tools::ElementRangeLayoutUnsupportedReason;
using calmcar::xcp::tools::MakeElementPageRange;
using calmcar::xcp::tools::NextElementPageStart;
using calmcar::xcp::tools::ParseElementIndex;
using calmcar::xcp::tools::SymbolDaqObservationUnsupportedReason;
using calmcar::xcp::tools::SymbolDetailReadUnsupportedReason;
using calmcar::xcp::tools::SymbolReadUnsupportedReason;
using calmcar::xcp::tools::SymbolStaticWriteUnsupportedReason;
using calmcar::xcp::tools::SymbolWriteUnsupportedReason;

TEST(XcpMasterSymbolPolicyTest, AllowsScalarMeasurementReadOnly) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Measurement;
    symbol.element_size_bytes = 4;

    EXPECT_TRUE(SymbolReadUnsupportedReason(symbol).empty());
    EXPECT_EQ(SymbolWriteUnsupportedReason(symbol),
              "仅支持 CHARACTERISTIC/VALUE 写入");
}

TEST(XcpMasterSymbolPolicyTest, AllowsWritableScalarValueCharacteristic) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.element_size_bytes = 2;
    symbol.read_write = true;

    EXPECT_TRUE(SymbolReadUnsupportedReason(symbol).empty());
    EXPECT_TRUE(SymbolWriteUnsupportedReason(symbol).empty());
}

/// @brief DAQ拒绝标定量及 Generic 数组；显式 XCPlite 接纳安全的一维数组。
TEST(XcpMasterSymbolPolicyTest, DaqRejectsCalibrationAndGenericArrays) {
    const std::vector<std::uint16_t> event_channels{3};
    SymbolInfo calibration;
    calibration.kind = SymbolKind::Characteristic;
    calibration.characteristic_type = CharacteristicType::Value;
    calibration.element_size_bytes = 4;
    calibration.daq_event_fixed = {3};
    EXPECT_TRUE(SymbolReadUnsupportedReason(calibration).empty());
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(calibration, event_channels)
                  .find("MEASUREMENT"),
              std::string::npos);

    SymbolInfo array;
    array.kind = SymbolKind::Measurement;
    array.element_size_bytes = 4;
    array.dimensions.push_back({0, 8, 4});
    array.daq_event_fixed = {3};
    EXPECT_TRUE(SymbolDetailReadUnsupportedReason(array).empty());
    // 数组准入不再按厂商 profile 门控；未开启 allow_contiguous_1d_array 时
    // 一律给出「暂不支持 DAQ 数组」。
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(array, event_channels)
                  .find("暂不支持 DAQ 数组"),
              std::string::npos);
    EXPECT_TRUE(SymbolDaqObservationUnsupportedReason(
                    array, event_channels, /*allow_contiguous_1d_array=*/true)
                    .empty());

    array.dimensions.front().extent = 256;
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(array, event_channels, true)
                  .find("单 Entry"),
              std::string::npos);
    array.dimensions.front().extent = 64;  // 元素数可表示，但 64 * 4 = 256
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(array, event_channels, true)
                  .find("单 Entry"),
              std::string::npos);
    array.dimensions.front().extent = 8;
    array.dimensions.front().byte_stride = 8;
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(array, event_channels, true)
                  .find("数组维度"),
              std::string::npos);
}

/// @brief DAQ必须具有唯一固定事件，且A2L中存在对应事件频道。
TEST(XcpMasterSymbolPolicyTest, DaqRequiresUniqueKnownFixedEvent) {
    const std::vector<std::uint16_t> event_channels{3};
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Measurement;
    symbol.element_size_bytes = 4;
    symbol.daq_event_default = {3};
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(symbol, event_channels)
                  .find("FIXED_EVENT_LIST"),
              std::string::npos);
    symbol.daq_event_fixed = {3, 4};
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(symbol, event_channels)
                  .find("FIXED_EVENT_LIST"),
              std::string::npos);
    symbol.daq_event_fixed = {4};
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(symbol, event_channels)
                  .find("EVENT_CHANNEL"),
              std::string::npos);
    symbol.daq_event_fixed = {3, 3};
    EXPECT_TRUE(
        SymbolDaqObservationUnsupportedReason(symbol, event_channels).empty());
    symbol.daq_event_fixed = {3};
    EXPECT_TRUE(
        SymbolDaqObservationUnsupportedReason(symbol, event_channels).empty());
}

TEST(XcpMasterSymbolPolicyTest, RejectsReadOnlyAndNonValueCharacteristics) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.element_size_bytes = 2;
    EXPECT_EQ(SymbolWriteUnsupportedReason(symbol), "A2L 未标记为可写");

    symbol.read_write = true;
    symbol.characteristic_type = CharacteristicType::Curve;
    EXPECT_FALSE(SymbolReadUnsupportedReason(symbol).empty());
    EXPECT_FALSE(SymbolWriteUnsupportedReason(symbol).empty());
}

TEST(XcpMasterSymbolPolicyTest, RejectsUnsafeMemoryLayouts) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Measurement;
    symbol.element_size_bytes = 4;

    symbol.dimensions.push_back({});
    EXPECT_NE(SymbolReadUnsupportedReason(symbol).find("数组"),
              std::string::npos);
    symbol.dimensions.clear();

    symbol.daq_bit_offset = 0;
    EXPECT_NE(SymbolReadUnsupportedReason(symbol).find("位字段"),
              std::string::npos);
    symbol.daq_bit_offset.reset();

    symbol.element_size_bytes = 0;
    EXPECT_NE(SymbolReadUnsupportedReason(symbol).find("固定宽度"),
              std::string::npos);
    symbol.element_size_bytes = 4;

    symbol.xcp_address =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
        1U;
    EXPECT_NE(SymbolReadUnsupportedReason(symbol).find("32 位"),
              std::string::npos);
}

TEST(XcpMasterSymbolPolicyTest,
     AllowsMeasurementArrayDetailAndFixedCurvePoints) {
    SymbolInfo array;
    array.kind = SymbolKind::Measurement;
    array.element_size_bytes = 4;
    array.dimensions.push_back({0, 128, 4});
    EXPECT_TRUE(SymbolReadUnsupportedReason(array).find("数组") !=
                std::string::npos);
    EXPECT_TRUE(SymbolDetailReadUnsupportedReason(array).empty());

    SymbolInfo curve;
    curve.kind = SymbolKind::Characteristic;
    curve.characteristic_type = CharacteristicType::Curve;
    curve.element_size_bytes = 4;
    curve.read_write = true;
    curve.dimensions.push_back({0, 11, 4});
    curve.fixed_curve_axis = FixedCurveAxisInfo{0.0, 1.0, 11, {}};
    EXPECT_TRUE(SymbolDetailReadUnsupportedReason(curve).empty());
    EXPECT_TRUE(CurvePointWriteUnsupportedReason(curve, 10).empty());
    EXPECT_NE(CurvePointWriteUnsupportedReason(curve, 11).find("索引"),
              std::string::npos);
    curve.read_write = false;
    EXPECT_EQ(CurvePointWriteUnsupportedReason(curve, 10),
              "A2L INSTANCE 未标记为可写");
}

TEST(XcpMasterSymbolPolicyTest, BoundsElementPagesAndAddresses) {
    SymbolInfo array;
    array.kind = SymbolKind::Measurement;
    array.xcp_address = 0x1000;
    array.element_size_bytes = 4;
    array.byte_order = calmcar::xcp::a2l::ByteOrder::MsbLast;
    array.dimensions.push_back({0, 128, 4});

    EXPECT_TRUE(ElementRangeLayoutUnsupportedReason(
                    array, 0, 16, 1, calmcar::xcp::ByteOrder::Intel)
                    .empty());
    EXPECT_NE(ElementRangeLayoutUnsupportedReason(
                  array, 120, 16, 1, calmcar::xcp::ByteOrder::Intel)
                  .find("超出"),
              std::string::npos);
    array.xcp_address = std::numeric_limits<std::uint32_t>::max() - 3U;
    EXPECT_NE(ElementRangeLayoutUnsupportedReason(
                  array, 0, 2, 1, calmcar::xcp::ByteOrder::Intel)
                  .find("地址范围"),
              std::string::npos);
}

TEST(XcpMasterSymbolPolicyTest, RequiresStaticAndRuntimePageWritePermission) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.element_size_bytes = 2;
    symbol.read_write = true;
    MemorySegmentInfo segment;
    segment.name = "kParameters";
    segment.segment_number = 1;
    segment.has_xcp_segment = true;
    segment.pages = {{0, 0, 0, XcpPageWriteAccess::DontCare},
                     {1, 0, 0, XcpPageWriteAccess::NotAllowed}};
    symbol.memory_segment = segment;

    EXPECT_TRUE(CalibrationPageWriteUnsupportedReason(symbol, 0,
                                                      PageAccessType::DontCare)
                    .empty());
    EXPECT_NE(CalibrationPageWriteUnsupportedReason(
                  symbol, 0, PageAccessType::WithoutOtherAccess)
                  .find("Slave"),
              std::string::npos);
    EXPECT_NE(CalibrationPageWriteUnsupportedReason(symbol, 1,
                                                    PageAccessType::DontCare)
                  .find("A2L PAGE"),
              std::string::npos);
    EXPECT_NE(CalibrationPageWriteUnsupportedReason(symbol, 2,
                                                    PageAccessType::DontCare)
                  .find("未在 A2L"),
              std::string::npos);
    symbol.memory_segment.reset();
    EXPECT_NE(CalibrationPageWriteUnsupportedReason(symbol, 0,
                                                    PageAccessType::DontCare)
                  .find("唯一映射"),
              std::string::npos);
}

/** @brief 验证详情静态策略只依赖A2L声明，不探测ECU活动页。 */
TEST(XcpMasterSymbolPolicyTest, StaticWritePolicyUsesOnlyA2lMetadata) {
    SymbolInfo value;
    value.kind = SymbolKind::Characteristic;
    value.characteristic_type = CharacteristicType::Value;
    value.element_size_bytes = 4;
    value.read_write = true;

    EXPECT_NE(SymbolStaticWriteUnsupportedReason(value).find("MEMORY_SEGMENT"),
              std::string::npos);

    MemorySegmentInfo segment;
    segment.name = "kParameters";
    segment.segment_number = 1;
    segment.has_xcp_segment = true;
    segment.pages = {{0, 0, 0, XcpPageWriteAccess::DontCare}};
    value.memory_segment = segment;
    EXPECT_TRUE(SymbolStaticWriteUnsupportedReason(value).empty());

    value.memory_segment->pages.front().xcp_write_access =
        XcpPageWriteAccess::NotAllowed;
    EXPECT_NE(SymbolStaticWriteUnsupportedReason(value).find("PAGE"),
              std::string::npos);

    SymbolInfo curve;
    curve.kind = SymbolKind::Characteristic;
    curve.characteristic_type = CharacteristicType::Curve;
    curve.element_size_bytes = 4;
    curve.read_write = true;
    curve.dimensions.push_back({0, 4, 4});
    curve.fixed_curve_axis = FixedCurveAxisInfo{0.0, 1.0, 4, {}};
    curve.memory_segment = segment;
    curve.memory_segment->pages.front().xcp_write_access =
        XcpPageWriteAccess::DontCare;
    EXPECT_TRUE(SymbolStaticWriteUnsupportedReason(curve).empty());
}

TEST(XcpMasterSymbolPolicyTest, ParsesStrictZeroBasedElementIndices) {
    ASSERT_TRUE(ParseElementIndex("0").has_value());
    EXPECT_EQ(*ParseElementIndex("0"), 0U);
    ASSERT_TRUE(ParseElementIndex("4095").has_value());
    EXPECT_EQ(*ParseElementIndex("4095"), 4095U);

    EXPECT_FALSE(ParseElementIndex("").has_value());
    EXPECT_FALSE(ParseElementIndex("-1").has_value());
    EXPECT_FALSE(ParseElementIndex("+1").has_value());
    EXPECT_FALSE(ParseElementIndex(" 1").has_value());
    EXPECT_FALSE(ParseElementIndex("1 ").has_value());
    EXPECT_FALSE(ParseElementIndex("18446744073709551616").has_value());
}

TEST(XcpMasterSymbolPolicyTest, BoundsEightElementArrayPages) {
    const auto first = MakeElementPageRange(128, 0, 8);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->first_index, 0U);
    EXPECT_EQ(first->count, 8U);

    const auto last = MakeElementPageRange(128, 120, 8);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->first_index, 120U);
    EXPECT_EQ(last->count, 8U);

    const auto partial = MakeElementPageRange(130, 128, 8);
    ASSERT_TRUE(partial.has_value());
    EXPECT_EQ(partial->count, 2U);
    EXPECT_FALSE(MakeElementPageRange(128, 7, 8).has_value());
    EXPECT_FALSE(MakeElementPageRange(128, 128, 8).has_value());
    EXPECT_FALSE(MakeElementPageRange(128, 0, 0).has_value());

    const auto next_first = NextElementPageStart(128, 0, 8);
    ASSERT_TRUE(next_first.has_value());
    EXPECT_EQ(*next_first, 8U);
    const auto next_last = NextElementPageStart(128, 120, 8);
    ASSERT_TRUE(next_last.has_value());
    EXPECT_EQ(*next_last, 120U);
    const auto next_partial = NextElementPageStart(130, 120, 8);
    ASSERT_TRUE(next_partial.has_value());
    EXPECT_EQ(*next_partial, 128U);
    const auto overflow_safe = NextElementPageStart(
        128, std::numeric_limits<std::uint64_t>::max() - 7U, 8);
    ASSERT_TRUE(overflow_safe.has_value());
    EXPECT_EQ(*overflow_safe, 120U);
    EXPECT_FALSE(NextElementPageStart(128, 7, 8).has_value());
    EXPECT_FALSE(NextElementPageStart(128, 0, 0).has_value());
}

}  // namespace
