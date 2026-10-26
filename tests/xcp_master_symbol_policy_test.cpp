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
using calmcar::xcp::tools::DaqEventSource;
using calmcar::xcp::tools::DaqEventSourceName;
using calmcar::xcp::tools::ElementRangeLayoutUnsupportedReason;
using calmcar::xcp::tools::MakeElementPageRange;
using calmcar::xcp::tools::NextElementPageStart;
using calmcar::xcp::tools::ParseElementIndex;
using calmcar::xcp::tools::ResolveDaqEventCandidate;
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

/// @brief 造一个可通过读准入的标量 MEASUREMENT，供事件来源用例复用。
static SymbolInfo MakeMeasuredScalar() {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Measurement;
    symbol.element_size_bytes = 4;
    return symbol;
}

/// @brief 规则 1：fixed 唯一时优先采用 fixed，即使 default 也声明了。
TEST(XcpMasterSymbolPolicyTest, DaqPrefersUniqueFixedEvent) {
    const std::vector<std::uint16_t> channels{3, 4};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_fixed = {3};
    symbol.daq_event_default = {4};

    EXPECT_TRUE(SymbolDaqObservationUnsupportedReason(symbol, channels).empty());
    const auto candidate = ResolveDaqEventCandidate(symbol);
    EXPECT_EQ(candidate.source, DaqEventSource::Fixed);
    EXPECT_EQ(candidate.channels, (std::vector<std::uint16_t>{3}));
    EXPECT_TRUE(candidate.error.empty());
    EXPECT_STREQ(DaqEventSourceName(candidate.source), "fixed");
}

/// @brief 规则 3：fixed 为空时退到唯一 default（这是本方案的核心行为变更）。
TEST(XcpMasterSymbolPolicyTest, DaqFallsBackToUniqueDefaultEvent) {
    const std::vector<std::uint16_t> channels{3};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_default = {3};

    EXPECT_TRUE(SymbolDaqObservationUnsupportedReason(symbol, channels).empty());
    const auto candidate = ResolveDaqEventCandidate(symbol);
    EXPECT_EQ(candidate.source, DaqEventSource::Default);
    EXPECT_EQ(candidate.channels, (std::vector<std::uint16_t>{3}));
    EXPECT_STREQ(DaqEventSourceName(candidate.source), "default");
}

/// @brief 规则 2：fixed 多值必须拒绝，且不得退回 default（规则 2 优先于 3）。
TEST(XcpMasterSymbolPolicyTest, DaqRejectsAmbiguousFixedEvents) {
    const std::vector<std::uint16_t> channels{3, 4};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_fixed = {3, 4};
    symbol.daq_event_default = {3};

    const auto reason =
        SymbolDaqObservationUnsupportedReason(symbol, channels);
    EXPECT_NE(reason.find("FIXED_EVENT_LIST"), std::string::npos);
    EXPECT_NE(reason.find("多个"), std::string::npos);
    const auto candidate = ResolveDaqEventCandidate(symbol);
    EXPECT_EQ(candidate.source, DaqEventSource::None);
    EXPECT_TRUE(candidate.channels.empty());
    EXPECT_FALSE(candidate.error.empty());
}

/// @brief 规则 4：default 多值同样拒绝（不猜）。
TEST(XcpMasterSymbolPolicyTest, DaqRejectsAmbiguousDefaultEvents) {
    const std::vector<std::uint16_t> channels{3, 4};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_default = {3, 4};

    const auto reason =
        SymbolDaqObservationUnsupportedReason(symbol, channels);
    EXPECT_NE(reason.find("DEFAULT_EVENT_LIST"), std::string::npos);
    EXPECT_NE(reason.find("多个"), std::string::npos);
    EXPECT_TRUE(ResolveDaqEventCandidate(symbol).channels.empty());
}

/// @brief 规则 5c：三张表皆空 → 拒绝，原因里三张表名都要出现（便于现场自查）。
TEST(XcpMasterSymbolPolicyTest, DaqRejectsWhenNoEventDeclared) {
    const std::vector<std::uint16_t> channels{3};
    const auto symbol = MakeMeasuredScalar();

    const auto reason =
        SymbolDaqObservationUnsupportedReason(symbol, channels);
    EXPECT_NE(reason.find("FIXED_EVENT_LIST"), std::string::npos);
    EXPECT_NE(reason.find("DEFAULT_EVENT_LIST"), std::string::npos);
    EXPECT_NE(reason.find("CONSISTENCY_EVENT_LIST"), std::string::npos);
    EXPECT_NE(reason.find("POLL"), std::string::npos);
}

/// @brief 从站未上报该事件通道时仍要拒绝（运行期 EVENT_CHANNEL 元数据缺失）。
TEST(XcpMasterSymbolPolicyTest, DaqRejectsEventOutsideEventChannelList) {
    const std::vector<std::uint16_t> channels{3};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_fixed = {9};

    // 候选本身合格（A2L 侧无矛盾），失败发生在运行期通道表核对。
    EXPECT_TRUE(ResolveDaqEventCandidate(symbol).error.empty());
    EXPECT_NE(SymbolDaqObservationUnsupportedReason(symbol, channels)
                  .find("EVENT_CHANNEL"),
              std::string::npos);
}

/// @brief 同一事件号重复声明只计一个（沿用既有去重口径）。
TEST(XcpMasterSymbolPolicyTest, ResolveDaqEventCandidateDeduplicates) {
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_fixed = {3, 3};

    const auto candidate = ResolveDaqEventCandidate(symbol);
    EXPECT_EQ(candidate.source, DaqEventSource::Fixed);
    EXPECT_EQ(candidate.channels, (std::vector<std::uint16_t>{3}));
}

/// @brief 规则 5：无 default 时按规范用 consistency 兜底。
TEST(XcpMasterSymbolPolicyTest, DaqFallsBackToConsistencyWhenDefaultAbsent) {
    const std::vector<std::uint16_t> channels{2};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_consistency_event = {2};

    EXPECT_TRUE(SymbolDaqObservationUnsupportedReason(symbol, channels).empty());
    const auto candidate = ResolveDaqEventCandidate(symbol);
    EXPECT_EQ(candidate.source, DaqEventSource::Consistency);
    EXPECT_EQ(candidate.channels, (std::vector<std::uint16_t>{2}));
    EXPECT_STREQ(DaqEventSourceName(candidate.source), "consistency");
}

/// @brief 规则 5b：consistency 多值同样无法唯一确定。
TEST(XcpMasterSymbolPolicyTest, DaqRejectsAmbiguousConsistencyEvents) {
    const std::vector<std::uint16_t> channels{2, 3};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_consistency_event = {2, 3};

    const auto reason =
        SymbolDaqObservationUnsupportedReason(symbol, channels);
    EXPECT_NE(reason.find("CONSISTENCY_EVENT_LIST"), std::string::npos);
    EXPECT_NE(reason.find("多个"), std::string::npos);
}

/// @brief 规则 0：default 越出 available 视为 A2L 自相矛盾。
TEST(XcpMasterSymbolPolicyTest, DaqRejectsDefaultOutsideAvailable) {
    const std::vector<std::uint16_t> channels{4};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_default = {4};
    symbol.daq_event_available = {3};

    const auto reason =
        SymbolDaqObservationUnsupportedReason(symbol, channels);
    EXPECT_NE(reason.find("自相矛盾"), std::string::npos);
    EXPECT_NE(reason.find("DEFAULT_EVENT_LIST"), std::string::npos);
}

/// @brief 规则 0：consistency 越出 available 同样自相矛盾。
TEST(XcpMasterSymbolPolicyTest, DaqRejectsConsistencyOutsideAvailable) {
    const std::vector<std::uint16_t> channels{4};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_consistency_event = {4};
    symbol.daq_event_available = {3};

    EXPECT_NE(SymbolDaqObservationUnsupportedReason(symbol, channels)
                  .find("自相矛盾"),
              std::string::npos);
}

/// @brief 规则 0 放行：default 是 available 的子集时合格。
TEST(XcpMasterSymbolPolicyTest, DaqAcceptsDefaultSubsetOfAvailable) {
    const std::vector<std::uint16_t> channels{3, 4};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_default = {3};
    symbol.daq_event_available = {3, 4};

    EXPECT_TRUE(SymbolDaqObservationUnsupportedReason(symbol, channels).empty());
    EXPECT_EQ(ResolveDaqEventCandidate(symbol).source, DaqEventSource::Default);
}

/// @brief 规则 0 无条件生效：即使 fixed 唯一（本次决策不需要 default），
///        只要 default 越出 available 仍拒绝 —— 基于自相矛盾的 A2L 做出的
///        任何事件选择都不可信，与多实例 fail closed 同源。
TEST(XcpMasterSymbolPolicyTest, RejectsInconsistentA2lEvenWhenFixedIsUnique) {
    const std::vector<std::uint16_t> channels{3};
    auto symbol = MakeMeasuredScalar();
    symbol.daq_event_fixed = {3};
    symbol.daq_event_default = {9};
    symbol.daq_event_available = {3};

    const auto reason =
        SymbolDaqObservationUnsupportedReason(symbol, channels);
    EXPECT_NE(reason.find("自相矛盾"), std::string::npos);
    EXPECT_EQ(ResolveDaqEventCandidate(symbol).source, DaqEventSource::None);
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

    // P2：段页是可选能力。A2L 没有 MEMORY_SEGMENT 时静态策略放行，写入资格
    // 交由运行时活动页校验（见 CliApp::CheckCalibrationPageWriteLocked）。
    EXPECT_TRUE(SymbolStaticWriteUnsupportedReason(value).empty());

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

/// @brief 界面必须能读到 A2L 声明的限值，且不能把「未声明」渲染成 0。
TEST(XcpMasterSymbolPolicyTest, RendersDeclaredLimitRangeText) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.element_size_bytes = 4;
    symbol.read_write = true;
    symbol.have_limit = true;
    symbol.lower_limit = 0.0;
    symbol.upper_limit = 10.0;

    EXPECT_EQ(calmcar::xcp::tools::CalibrationLimitText(symbol), "0 … 10");

    symbol.have_limit = false;
    EXPECT_EQ(calmcar::xcp::tools::CalibrationLimitText(symbol),
              "A2L 未声明限值");
}

/// @brief 越界目标值必须即时提示，口径与换算层 B-15 一致。
TEST(XcpMasterSymbolPolicyTest, HintsTargetValueBeyondDeclaredLimit) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.element_size_bytes = 4;
    symbol.read_write = true;
    symbol.have_limit = true;
    symbol.lower_limit = 0.0;
    symbol.upper_limit = 10.0;

    const auto beyond = calmcar::xcp::tools::CalibrationTargetLimitHint(
        symbol, "20");
    EXPECT_NE(beyond.find("超出 A2L 允许范围"), std::string::npos);
    EXPECT_NE(beyond.find("B-15"), std::string::npos);

    const auto below = calmcar::xcp::tools::CalibrationTargetLimitHint(symbol,
                                                                     "-1");
    EXPECT_NE(below.find("超出 A2L 允许范围"), std::string::npos);

    EXPECT_TRUE(
        calmcar::xcp::tools::CalibrationTargetLimitHint(symbol, "5").empty());
    EXPECT_TRUE(calmcar::xcp::tools::CalibrationTargetLimitHint(symbol, "0")
                    .empty());
    EXPECT_TRUE(calmcar::xcp::tools::CalibrationTargetLimitHint(symbol, "10")
                    .empty());
    EXPECT_TRUE(calmcar::xcp::tools::CalibrationTargetLimitHint(symbol, "abc")
                    .empty());
    EXPECT_TRUE(calmcar::xcp::tools::CalibrationTargetLimitHint(symbol, "5x")
                    .empty());
    EXPECT_TRUE(calmcar::xcp::tools::CalibrationTargetLimitHint(symbol, "")
                    .empty());

    symbol.have_limit = false;
    EXPECT_TRUE(calmcar::xcp::tools::CalibrationTargetLimitHint(symbol, "20")
                    .empty());
}

/// @brief A2L 未声明 MEMORY_SEGMENT 时不得再以「缺段页」为由拒绝写入（P2）。
TEST(XcpMasterSymbolPolicyTest, RelaxesStaticWriteGateWithoutMemorySegment) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.element_size_bytes = 1;
    symbol.read_write = true;

    // 无段页事实：静态策略必须放行，页校验改由运行时决定。
    EXPECT_FALSE(calmcar::xcp::tools::HasCalibrationPageFacts(symbol));
    EXPECT_TRUE(calmcar::xcp::tools::SymbolStaticWriteUnsupportedReason(symbol)
                    .empty());
}

/// @brief 有段页事实但没有任何 DontCare 页时，静态策略仍须拒绝。
TEST(XcpMasterSymbolPolicyTest, KeepsStaticWriteGateWithReadOnlyPages) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.element_size_bytes = 1;
    symbol.read_write = true;
    MemorySegmentInfo segment;
    segment.name = "cal";
    segment.segment_number = 0;
    segment.has_xcp_segment = true;
    segment.pages = {{0, 0, 0, XcpPageWriteAccess::NotAllowed}};
    symbol.memory_segment = segment;

    EXPECT_TRUE(calmcar::xcp::tools::HasCalibrationPageFacts(symbol));
    EXPECT_EQ(calmcar::xcp::tools::SymbolStaticWriteUnsupportedReason(symbol),
              "A2L未声明允许无条件XCP写入的PAGE");
}

/// @brief OPTIONAL_CMD 必须能解析成页命令能力事实。
TEST(XcpMasterSymbolPolicyTest, ParsesCalibrationPageFactsFromOptionalCommands) {
    using calmcar::xcp::CommandCode;
    using calmcar::xcp::tools::A2lDeclaresCommand;
    using calmcar::xcp::tools::MakeCalibrationPageFacts;

    const std::vector<std::uint8_t> commands{
        static_cast<std::uint8_t>(CommandCode::GetCalPage),
        static_cast<std::uint8_t>(CommandCode::GetPageInfo)};
    EXPECT_TRUE(A2lDeclaresCommand(commands, CommandCode::GetCalPage));
    EXPECT_FALSE(A2lDeclaresCommand(commands, CommandCode::SetCalPage));

    const auto facts = MakeCalibrationPageFacts(commands);
    EXPECT_TRUE(facts.declares_get_cal_page);
    EXPECT_TRUE(facts.declares_get_page_info);
    EXPECT_FALSE(facts.declares_set_cal_page);

    const auto empty = MakeCalibrationPageFacts({});
    EXPECT_FALSE(empty.declares_get_cal_page);
    EXPECT_FALSE(empty.declares_get_page_info);
}

/// @brief 页判定依据的文案必须区分三种情形，不能都渲染成空。
TEST(XcpMasterSymbolPolicyTest, ExplainsCalibrationPageDecisionBasis) {
    using calmcar::xcp::tools::CalibrationPageFacts;
    using calmcar::xcp::tools::CalibrationPageNote;

    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.element_size_bytes = 1;
    symbol.read_write = true;

    // 1) 无段页事实且未声明任何页命令：按单页直写。
    const auto none = CalibrationPageNote(symbol, CalibrationPageFacts{});
    EXPECT_NE(none.find("单页直写"), std::string::npos);

    // 2) 无段页事实但声明了 GET_CAL_PAGE：按 0 号段查询活动页。
    CalibrationPageFacts with_cal_page;
    with_cal_page.declares_get_cal_page = true;
    const auto with_query = CalibrationPageNote(symbol, with_cal_page);
    EXPECT_NE(with_query.find("0 号段"), std::string::npos);

    // 3) 有段页事实但未声明 GET_PAGE_INFO：页写权限未取证。
    MemorySegmentInfo segment;
    segment.name = "cal";
    segment.segment_number = 0;
    segment.has_xcp_segment = true;
    segment.pages = {{0, 0, 0, XcpPageWriteAccess::DontCare}};
    symbol.memory_segment = segment;
    const auto no_page_info = CalibrationPageNote(symbol, with_cal_page);
    EXPECT_NE(no_page_info.find("未声明 GET_PAGE_INFO"), std::string::npos);

    // 4) 有段页事实且声明了 GET_PAGE_INFO：无需附注。
    with_cal_page.declares_get_page_info = true;
    EXPECT_TRUE(CalibrationPageNote(symbol, with_cal_page).empty());
}

}  // namespace
