/**
 * @file measurement_adapter_test.cpp
 * @brief A2L 测量数据库适配器（A2lMeasurementDatabase）L3 测试（v0.5）。
 *
 * 覆盖计划 §5.5 / §5.6 适配层验收项（用内存 Fake IA2lDatabase，不加载真实
 * A2L 文件）：
 *   - Find：xcp_address 低 32 位收窄、address_extension 透传、标量/数组
 *     element_count（乘积 + 255 截断）、event_channel=0（v0.5 默认）；
 *   - 桥接层错误码 → 核心 MeasurementErrorCode 全映射表（NotFound /
 *     AmbiguousName / RawSizeMismatch / UnsupportedConversion /
 *     ConversionNotInvertible / ConversionOutOfRange / UnsupportedOperation /
 *     AddressOverflow / NotReady / Internal）；
 *   - ToPhysical：五分支 variant 等形复制（int64/uint64/double/string/bool）；
 *   - 错误 message/symbol 透传。
 *
 * A2L 栈为必编组件，本目标随测试构建无条件生成（独立 MeasurementAdapterTest）。
 */

#include "adapter/a2l/a2l_measurement_database.hpp"
#include "adapter/a2l/measurement_runtime_profile.hpp"

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/measurement/measurement_types.hpp"
#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace calmcar::xcp {
namespace {

using a2l::Dimension;
using a2l::Error;
using a2l::ErrorCode;
using a2l::Phase;
using a2l::PhysicalValue;
using a2l::Result;
using a2l::SymbolInfo;

/// @brief 内存版 A2L 数据库：符号表 + 换算表 + 可注入错误表
class FakeA2l final : public a2l::IA2lDatabase {
public:
    Result<SymbolInfo> Find(std::string_view name) const override {
        const std::string key(name);
        const auto e = find_errors.find(key);
        if (e != find_errors.end()) {
            return Error(MakeErr(e->second, key));
        }
        const auto it = symbols.find(key);
        if (it == symbols.end()) {
            return Error(MakeErr(ErrorCode::NotFound, key));
        }
        return it->second;
    }

    Result<std::vector<SymbolInfo>> Search(
        std::string_view /*pattern*/,
        std::size_t /*max_count*/) const override {
        return std::vector<SymbolInfo>{};
    }

    Result<std::size_t> Count() const override { return symbols.size(); }

    Result<std::size_t> ByteSizeOf(std::string_view /*name*/) const override {
        return static_cast<std::size_t>(0);
    }

    Result<PhysicalValue> ToPhysical(std::string_view name,
                                     a2l::BytesView raw) const override {
        const std::string key(name);
        const auto e = conv_errors.find(key);
        if (e != conv_errors.end()) {
            return Error(MakeErr(e->second, key));
        }
        const auto it = conversions.find(key);
        if (it != conversions.end()) {
            return it->second;
        }
        // 缺省换算：raw[0] → double（用例可精确核对）
        return PhysicalValue{static_cast<double>(
            raw.empty() ? 0U : static_cast<unsigned>(raw[0]))};
    }

    /// @brief Fake 的元素换算按数组下标返回独立物理值，验证适配器不丢失下标。
    Result<PhysicalValue> ToPhysicalElement(std::string_view name,
                                            std::uint64_t element_index,
                                            a2l::BytesView raw) const override {
        const std::string key(name);
        observed_element_indices.push_back(element_index);
        const auto array_it = element_conversions.find(key);
        if (array_it != element_conversions.end()) {
            const auto value_it = array_it->second.find(element_index);
            if (value_it != array_it->second.end()) return value_it->second;
        }
        return ToPhysical(name, raw);
    }

    Result<a2l::Bytes> FromPhysical(
        std::string_view /*name*/,
        const PhysicalValue& /*value*/) const override {
        return Error(MakeErr(ErrorCode::UnsupportedOperation, "from"));
    }

    /// @brief Fake 的元素编码复用标量拒绝行为；不模拟数组布局。
    Result<a2l::Bytes> FromPhysicalElement(
        std::string_view name, std::uint64_t /*element_index*/,
        const PhysicalValue& value) const override {
        return FromPhysical(name, value);
    }

    /// @brief 构造注入错误（symbol 置为查询名，核对透传）
    static Error MakeErr(ErrorCode code, const std::string& symbol) {
        Error err;
        err.code = code;
        err.phase = Phase::Query;
        err.message = "injected:" + symbol;
        err.symbol = symbol;
        return err;
    }

    std::map<std::string, SymbolInfo> symbols;         ///< 符号表
    std::map<std::string, PhysicalValue> conversions;  ///< 换算值表
    std::map<std::string, std::map<std::uint64_t, PhysicalValue>>
        element_conversions;  ///< 按数组下标注入元素换算值
    mutable std::vector<std::uint64_t> observed_element_indices;  ///< 观察调用下标
    std::map<std::string, ErrorCode> find_errors;      ///< Find 注入错误
    std::map<std::string, ErrorCode> conv_errors;      ///< ToPhysical 注入错误
};

/// @brief 构造标量符号（地址/扩展/元素宽）
SymbolInfo MakeScalar(std::uint64_t xcp_address, std::uint8_t extension,
                      std::uint8_t element_size_bytes) {
    SymbolInfo info;
    info.name = "sym";
    info.xcp_address = xcp_address;
    info.address_extension = extension;
    info.element_size_bytes = element_size_bytes;
    return info;
}

class RuntimeProfileSlave {
public:
    std::uint8_t key_byte{0xC0};
    std::uint8_t connect_mode_basic{0xC0};
    std::uint8_t event_max_daq_lists{0xFF};
    std::uint16_t max_daq{4};
    std::uint16_t max_event_channel{0xFF};
    std::uint8_t min_daq{0};
    std::uint8_t daq_properties{0x11};
    std::uint8_t timestamp_mode{0x0C};
    std::uint16_t timestamp_ticks{3};
    std::uint8_t daq_granularity{1};
    std::uint8_t max_daq_entry_size{8};
    std::uint8_t event_cycle{5};
    std::uint8_t event_unit{2};
    std::uint8_t resource_protection{0};
    bool event_supported{true};
    std::vector<std::uint16_t> queried_events;

    Bytes operator()(BytesView packet) {
        if (packet.empty()) return {};
        const auto command = static_cast<CommandCode>(packet[0]);
        switch (command) {
            case CommandCode::Connect:
                return Res(
                    {0x15, connect_mode_basic, 0x08, 0x00, 0x08, 0x10, 0x10});
            case CommandCode::GetStatus:
                return Res({0x00, resource_protection, 0x01, 0x07, 0x00});
            case CommandCode::GetCommModeInfo:
                return Res({0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13});
            case CommandCode::GetPagProcessorInfo:
            case CommandCode::Synch:
                return Err(calmcar::xcp::ErrorCode::CmdUnknown);
            case CommandCode::Disconnect:
                return Res({});
            case CommandCode::GetDaqProcessorInfo:
                return Res(
                    {daq_properties, static_cast<std::uint8_t>(max_daq & 0xFFU),
                     static_cast<std::uint8_t>(max_daq >> 8U),
                     static_cast<std::uint8_t>(max_event_channel & 0xFFU),
                     static_cast<std::uint8_t>(max_event_channel >> 8U),
                     min_daq, key_byte});
            case CommandCode::GetDaqResolutionInfo:
                return Res({daq_granularity, max_daq_entry_size, 0x01, 0x08,
                            timestamp_mode,
                            static_cast<std::uint8_t>(timestamp_ticks & 0xFFU),
                            static_cast<std::uint8_t>(timestamp_ticks >> 8U)});
            case CommandCode::GetDaqEventInfo:
                if (!event_supported)
                    return Err(calmcar::xcp::ErrorCode::CmdUnknown);
                if (packet.size() >= 4U) {
                    queried_events.push_back(
                        static_cast<std::uint16_t>(packet[2]) |
                        (static_cast<std::uint16_t>(packet[3]) << 8U));
                }
                return Res({0x01, event_max_daq_lists, 0x05, event_cycle,
                            event_unit, 0xFF});
            default:
                return Err(calmcar::xcp::ErrorCode::CmdUnknown);
        }
    }

private:
    static Bytes Res(std::initializer_list<std::uint8_t> body) {
        Bytes bytes{static_cast<std::uint8_t>(PacketType::Res)};
        bytes.insert(bytes.end(), body.begin(), body.end());
        return bytes;
    }
    static Bytes Err(calmcar::xcp::ErrorCode code) {
        return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                     static_cast<std::uint8_t>(code)};
    }
};

std::unique_ptr<XcpMaster> MakeProfileMaster(RuntimeProfileSlave& slave) {
    auto transport = std::make_unique<test::MockTransport>();
    transport->SetResponse(
        [&slave](BytesView packet) { return slave(packet); });
    auto master =
        std::make_unique<XcpMaster>(std::move(transport), XcpMasterOptions{});
    master->Connect();
    return master;
}

// ---------------------------------------------------------------------------
// Find 字段映射
// ---------------------------------------------------------------------------

TEST(A2lMeasurementDatabase, FindMapsScalarFieldsWithAddressNarrowing) {
    FakeA2l db;
    // 64 位 xcp_address 带高 32 位脏数据：核心 Address 仅取低 32 位
    db.symbols["EngineSpeed"] = MakeScalar(0x00000001DEADBEEFULL, 0x22U, 4U);
    A2lMeasurementDatabase adapter(db);

    const MeasurementResult<MeasurementSymbolInfo> r =
        adapter.Find("EngineSpeed");
    ASSERT_TRUE(r.HasValue());
    EXPECT_EQ(r.Value().address, 0xDEADBEEFU);
    EXPECT_EQ(r.Value().extension, 0x22U);
    EXPECT_EQ(r.Value().element_size_bytes, 4U);
    EXPECT_EQ(r.Value().element_count, 1U);  // 标量：无维度 → 1
    EXPECT_EQ(r.Value().event_channel,
              0U);  // v0.5 默认（SymbolInfo 无事件通道）
}

TEST(A2lMeasurementDatabase, FindPreservesOneElementAndOneDimensionalArrayShape) {
    FakeA2l db;
    SymbolInfo scalar = MakeScalar(0x3000U, 0U, 4U);
    scalar.kind = a2l::SymbolKind::Measurement;
    db.symbols["Scalar"] = scalar;
    SymbolInfo one = MakeScalar(0x3100U, 0U, 4U);
    one.kind = a2l::SymbolKind::Measurement;
    one.dimensions = {Dimension{0, 1, 4}};
    db.symbols["One"] = one;
    SymbolInfo array = MakeScalar(0x4000U, 0U, 2U);
    array.kind = a2l::SymbolKind::Measurement;
    array.dimensions = {Dimension{0, 6, 2}};
    db.symbols["Array"] = array;
    A2lMeasurementDatabase adapter(db);

    const MeasurementResult<MeasurementSymbolInfo> scalar_info =
        adapter.Find("Scalar");
    const MeasurementResult<MeasurementSymbolInfo> one_info = adapter.Find("One");
    const MeasurementResult<MeasurementSymbolInfo> array_info = adapter.Find("Array");
    ASSERT_TRUE(scalar_info.HasValue());
    ASSERT_TRUE(one_info.HasValue());
    ASSERT_TRUE(array_info.HasValue());
    EXPECT_FALSE(scalar_info.Value().is_array);
    EXPECT_TRUE(one_info.Value().is_array);
    EXPECT_EQ(one_info.Value().element_count, 1U);
    EXPECT_TRUE(array_info.Value().is_array);
    EXPECT_EQ(array_info.Value().element_count, 6U);
    EXPECT_EQ(array_info.Value().element_size_bytes, 2U);
}

TEST(A2lMeasurementDatabase, FindRejectsUnsupportedArrayShapesAndCounts) {
    FakeA2l db;
    SymbolInfo multidimensional = MakeScalar(0x4000U, 0U, 2U);
    multidimensional.kind = a2l::SymbolKind::Measurement;
    multidimensional.dimensions = {Dimension{0, 2, 2}, Dimension{0, 3, 4}};
    db.symbols["Multidimensional"] = multidimensional;
    SymbolInfo too_many = MakeScalar(0x5000U, 0U, 1U);
    too_many.kind = a2l::SymbolKind::Measurement;
    too_many.dimensions = {Dimension{0, 256, 1}};
    db.symbols["TooMany"] = too_many;
    SymbolInfo strided = MakeScalar(0x6000U, 0U, 2U);
    strided.kind = a2l::SymbolKind::Measurement;
    strided.dimensions = {Dimension{0, 4, 4}};
    db.symbols["Strided"] = strided;
    SymbolInfo wrong_kind = MakeScalar(0x7000U, 0U, 2U);
    wrong_kind.kind = a2l::SymbolKind::Characteristic;
    wrong_kind.dimensions = {Dimension{0, 4, 2}};
    db.symbols["CharacteristicArray"] = wrong_kind;
    A2lMeasurementDatabase adapter(db);

    for (const std::string& name : {"Multidimensional", "TooMany", "Strided",
                                    "CharacteristicArray"}) {
        const MeasurementResult<MeasurementSymbolInfo> result = adapter.Find(name);
        ASSERT_FALSE(result.HasValue()) << name;
        EXPECT_EQ(result.ErrorInfo().code, MeasurementErrorCode::InvalidLayout)
            << name;
    }
}

// ---------------------------------------------------------------------------
// Find 错误映射
// ---------------------------------------------------------------------------

TEST(A2lMeasurementDatabase, FindErrorCodesMapToCoreCodes) {
    FakeA2l db;
    db.find_errors["missing"] = ErrorCode::NotFound;
    db.find_errors["ambig"] = ErrorCode::AmbiguousName;
    db.find_errors["bad"] = ErrorCode::BadArgument;
    db.find_errors["addr"] = ErrorCode::AddressOverflow;
    A2lMeasurementDatabase adapter(db);

    {
        const MeasurementResult<MeasurementSymbolInfo> r =
            adapter.Find("missing");
        ASSERT_FALSE(r.HasValue());
        EXPECT_EQ(r.ErrorInfo().code, MeasurementErrorCode::NotFound);
        EXPECT_EQ(r.ErrorInfo().symbol, "missing");  // 透传
        EXPECT_EQ(r.ErrorInfo().message, "injected:missing");
    }
    {
        const MeasurementResult<MeasurementSymbolInfo> r =
            adapter.Find("ambig");
        ASSERT_FALSE(r.HasValue());
        EXPECT_EQ(r.ErrorInfo().code, MeasurementErrorCode::AmbiguousName);
    }
    {
        // 未登记名字走 Fake 默认 NotFound
        const MeasurementResult<MeasurementSymbolInfo> r =
            adapter.Find("unknown");
        ASSERT_FALSE(r.HasValue());
        EXPECT_EQ(r.ErrorInfo().code, MeasurementErrorCode::NotFound);
    }
    {
        const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("bad");
        ASSERT_FALSE(r.HasValue());
        EXPECT_EQ(r.ErrorInfo().code,
                  MeasurementErrorCode::NotFound);  // BadArgument→NotFound
    }
    {
        const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("addr");
        ASSERT_FALSE(r.HasValue());
        EXPECT_EQ(r.ErrorInfo().code,
                  MeasurementErrorCode::InvalidLayout);  // AddressOverflow
    }
}

// ---------------------------------------------------------------------------
// ToPhysical variant 复制与错误映射
// ---------------------------------------------------------------------------

TEST(A2lMeasurementDatabase, ToPhysicalCopiesAllVariantAlternatives) {
    FakeA2l db;
    db.symbols["d"] = MakeScalar(0x100U, 0U, 8U);
    db.symbols["i"] = MakeScalar(0x110U, 0U, 8U);
    db.symbols["u"] = MakeScalar(0x120U, 0U, 8U);
    db.symbols["s"] = MakeScalar(0x130U, 0U, 1U);
    db.symbols["b"] = MakeScalar(0x140U, 0U, 1U);
    db.conversions["d"] = PhysicalValue{3.25};
    db.conversions["i"] = PhysicalValue{std::int64_t{-7}};
    db.conversions["u"] = PhysicalValue{std::uint64_t{42}};
    db.conversions["s"] = PhysicalValue{std::string("rpm")};
    db.conversions["b"] = PhysicalValue{true};
    A2lMeasurementDatabase adapter(db);

    const Bytes one{0x01};
    const std::array<std::uint8_t, 8> eight{};
    const BytesView raw8{eight};

    auto rd = adapter.ToPhysical("d", raw8);
    ASSERT_TRUE(rd.HasValue());
    EXPECT_EQ(std::get<double>(rd.Value()), 3.25);

    auto ri = adapter.ToPhysical("i", raw8);
    ASSERT_TRUE(ri.HasValue());
    EXPECT_EQ(std::get<std::int64_t>(ri.Value()), -7);

    auto ru = adapter.ToPhysical("u", raw8);
    ASSERT_TRUE(ru.HasValue());
    EXPECT_EQ(std::get<std::uint64_t>(ru.Value()), 42U);

    auto rs = adapter.ToPhysical("s", one);
    ASSERT_TRUE(rs.HasValue());
    EXPECT_EQ(std::get<std::string>(rs.Value()), "rpm");

    auto rb = adapter.ToPhysical("b", one);
    ASSERT_TRUE(rb.HasValue());
    EXPECT_EQ(std::get<bool>(rb.Value()), true);
}

TEST(A2lMeasurementDatabase, ToPhysicalDefaultConversionUsesRawFirstByte) {
    FakeA2l db;
    db.symbols["x"] = MakeScalar(0x200U, 0U, 2U);
    A2lMeasurementDatabase adapter(db);
    const Bytes raw{0x2A, 0x00};
    const MeasurementResult<MeasurementValue> r = adapter.ToPhysical("x", raw);
    ASSERT_TRUE(r.HasValue());
    EXPECT_EQ(std::get<double>(r.Value()), 42.0);  // Fake：raw[0]=0x2A
}

TEST(EventBoundMeasurementDatabase, RequiresExplicitUnambiguousBinding) {
    FakeA2l db;
    db.symbols["x"] = MakeScalar(0x100U, 0U, 1U);
    A2lMeasurementDatabase inner(db);

    EventBoundMeasurementDatabase unbound(inner, {});
    const auto missing = unbound.Find("x");
    ASSERT_FALSE(missing.HasValue());
    EXPECT_EQ(missing.ErrorInfo().code, MeasurementErrorCode::InvalidLayout);

    EventBoundMeasurementDatabase ambiguous(inner, {{"x", {0U, 1U}}});
    const auto conflict = ambiguous.Find("x");
    ASSERT_FALSE(conflict.HasValue());
    EXPECT_EQ(conflict.ErrorInfo().code, MeasurementErrorCode::InvalidLayout);

    EventBoundMeasurementDatabase overflow(inner, {{"x", {0x10000U}}});
    const auto too_large = overflow.Find("x");
    ASSERT_FALSE(too_large.HasValue());
    EXPECT_EQ(too_large.ErrorInfo().code, MeasurementErrorCode::InvalidLayout);
}

TEST(EventBoundMeasurementDatabase, AcceptsChannelZeroAndDelegatesConversion) {
    FakeA2l db;
    db.symbols["x"] = MakeScalar(0x100U, 0U, 1U);
    A2lMeasurementDatabase inner(db);
    EventBoundMeasurementDatabase bound(inner, {{"x", {0U, 0U}}});

    const auto symbol = bound.Find("x");
    ASSERT_TRUE(symbol.HasValue());
    EXPECT_EQ(symbol.Value().event_channel, 0U);

    const Bytes raw{0x2A};
    const auto physical = bound.ToPhysical("x", raw);
    ASSERT_TRUE(physical.HasValue());
    EXPECT_EQ(std::get<double>(physical.Value()), 42.0);
}

TEST(A2lMeasurementDatabase, ToPhysicalErrorCodesMapToCoreCodes) {
    FakeA2l db;
    db.conv_errors["size"] = ErrorCode::RawSizeMismatch;
    db.conv_errors["conv"] = ErrorCode::UnsupportedConversion;
    db.conv_errors["inv"] = ErrorCode::ConversionNotInvertible;
    db.conv_errors["oor"] = ErrorCode::ConversionOutOfRange;
    db.conv_errors["op"] = ErrorCode::UnsupportedOperation;
    db.conv_errors["ready"] = ErrorCode::NotReady;
    db.conv_errors["int"] = ErrorCode::Internal;
    A2lMeasurementDatabase adapter(db);

    const Bytes one{0x01};
    struct Case {
        const char* key;
        MeasurementErrorCode want;
    };
    const Case cases[] = {
        {"size", MeasurementErrorCode::RawSizeMismatch},
        {"conv", MeasurementErrorCode::UnsupportedConversion},
        {"inv", MeasurementErrorCode::UnsupportedConversion},
        {"oor", MeasurementErrorCode::UnsupportedConversion},
        {"op", MeasurementErrorCode::InvalidLayout},
        {"ready", MeasurementErrorCode::Busy},
        {"int", MeasurementErrorCode::Fatal},
    };
    for (const Case& c : cases) {
        const MeasurementResult<MeasurementValue> r =
            adapter.ToPhysical(c.key, one);
        ASSERT_FALSE(r.HasValue()) << c.key;
        EXPECT_EQ(r.ErrorInfo().code, c.want) << c.key;
        EXPECT_EQ(r.ErrorInfo().symbol, std::string(c.key)) << c.key;
    }
}

TEST(MeasurementRuntimeProfile,
     AcceptsOnlyAbsoluteTimestampFreeProfile) {
    RuntimeProfileSlave slave;
    slave.key_byte = 0x00U;
    slave.timestamp_mode = 0x00U;
    auto master = MakeProfileMaster(slave);
    const std::array<A2lEventMetadata, 1> events{{
        {"event0", "event0", 0U, 5U, 2U, 1U, 0xFFU},
    }};

    const auto profile = BuildMeasurementRuntimeProfile(*master, events);
    ASSERT_TRUE(profile.HasValue()) << profile.ErrorInfo().message;
    EXPECT_EQ(profile.Value().identification_field_type,
              IdentificationFieldType::Absolute);
    EXPECT_EQ(profile.Value().identification_header_bytes, 1U);
    EXPECT_EQ(profile.Value().daq_entry_granularity_bytes, 1U);
    EXPECT_EQ(profile.Value().max_daq_entry_size_elements, 8U);
    EXPECT_EQ(slave.queried_events, (std::vector<std::uint16_t>{0U}));
}

TEST(MeasurementRuntimeProfile, RejectsUnverifiedModesAndEvents) {
    const std::array<A2lEventMetadata, 1> event{{
        {"event0", "event0", 0U, 5U, 2U, 1U, 0xFFU},
    }};
    {
        RuntimeProfileSlave slave;
        slave.key_byte = 0x10U;  // 地址扩展模式为 ODT，本库仅支持 FREE(0)。
        slave.timestamp_mode = 0x00U;
        auto master = MakeProfileMaster(slave);
        EXPECT_FALSE(
            BuildMeasurementRuntimeProfile(*master, event).HasValue());
    }
    {
        RuntimeProfileSlave slave;
        slave.key_byte = 0x00U;
        slave.timestamp_mode = 0x0CU;  // 固定 32-bit 时间戳：C1 后受支持
        auto master = MakeProfileMaster(slave);
        auto profile = BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_TRUE(profile.HasValue()) << profile.ErrorInfo().message;
        // 固定时间戳按 XCPlite 厂商约定换算，并如实记录采用了该约定。
        EXPECT_TRUE(profile.Value().used_xcplite_timestamp_unit_convention);
        EXPECT_GT(profile.Value().timestamp_unit_ns, 0U);
    }
    {
        RuntimeProfileSlave slave;
        slave.key_byte = 0x00U;
        slave.timestamp_mode = 0x00U;
        auto master = MakeProfileMaster(slave);
        auto no_daq_event = event;
        no_daq_event[0].max_daq_list = 0U;
        EXPECT_FALSE(
            BuildMeasurementRuntimeProfile(*master, no_daq_event)
                .HasValue());
    }
}

TEST(MeasurementRuntimeProfile,
     CrossChecksLiveProfileAndAllBoundEvents) {
    RuntimeProfileSlave slave;
    auto master = MakeProfileMaster(slave);
    const std::array<A2lEventMetadata, 2> events{{
        {"event0", "event0", 0U, 5U, 2U, 1U, 0xFFU},
        {"event4", "event4", 4U, 5U, 2U, 1U, 0xFFU},
    }};

    const auto profile = BuildMeasurementRuntimeProfile(*master, events);
    ASSERT_TRUE(profile.HasValue()) << profile.ErrorInfo().message;
    EXPECT_EQ(profile.Value().identification_field_type,
              IdentificationFieldType::RelativeWordAligned);
    EXPECT_EQ(profile.Value().identification_header_bytes, 4U);
    EXPECT_EQ(profile.Value().timestamp_unit_ns, 3U);
    EXPECT_TRUE(profile.Value().timestamp_first_odt_only);
    EXPECT_TRUE(profile.Value().daq_entry_capabilities
                    .supports_contiguous_1d_arrays);
    EXPECT_EQ(profile.Value().daq_entry_capabilities.entry_granularity_bytes,
              1U);
    EXPECT_EQ(profile.Value().daq_entry_capabilities.max_entry_size_elements, 8U);
    EXPECT_EQ(slave.queried_events, (std::vector<std::uint16_t>{0U, 4U}));

    FakeA2l db;
    A2lMeasurementDatabase inner(db);
    EventBoundMeasurementDatabase bound(inner, {{"x", {0U}}});
    MeasurementSession session(bound);
    EXPECT_NO_THROW(ApplyMeasurementRuntimeProfile(session, profile.Value()));
}

TEST(MeasurementRuntimeProfile, RejectsUnknownEntryLimitsBeforeEvents) {
    const std::array<A2lEventMetadata, 1> event{{
        {"event", "event", 0U, 5U, 2U, 1U, 0xFFU},
    }};
    for (const auto [granularity, max_entry] :
         {std::pair<std::uint8_t, std::uint8_t>{0U, 8U},
          std::pair<std::uint8_t, std::uint8_t>{1U, 0U},
          std::pair<std::uint8_t, std::uint8_t>{2U, 8U}}) {
        RuntimeProfileSlave slave;
        slave.daq_granularity = granularity;
        slave.max_daq_entry_size = max_entry;
        auto master = MakeProfileMaster(slave);
        const auto profile = BuildMeasurementRuntimeProfile(*master, event);
        EXPECT_FALSE(profile.HasValue());
        EXPECT_TRUE(slave.queried_events.empty());
    }
}

TEST(DaqRuntimeProfiles, RejectsProtectedDaqBeforeConfiguration) {
    const std::array<A2lEventMetadata, 1> event{{
        {"event", "event", 0U, 5U, 2U, 1U, 0xFFU},
    }};
    {
        RuntimeProfileSlave slave;
        slave.resource_protection = static_cast<std::uint8_t>(Resource::Daq);
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_NE(result.ErrorInfo().message.find("不发送 FREE_DAQ"),
                  std::string::npos);
    }
    {
        RuntimeProfileSlave slave;
        slave.resource_protection = static_cast<std::uint8_t>(Resource::Daq);
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_NE(result.ErrorInfo().message.find("不发送 FREE_DAQ"),
                  std::string::npos);
    }
}

TEST(MeasurementRuntimeProfile, RejectsUnknownOrMismatchedFacts) {
    const std::array<A2lEventMetadata, 1> event{{
        {"event", "event", 0U, 5U, 2U, 1U, 0xFFU},
    }};
    {
        RuntimeProfileSlave slave;
        slave.key_byte =
            0x80U;  // wire code 2 = RelativeWord：C1 后按规范自动支持
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
        EXPECT_EQ(result.Value().identification_field_type,
                  IdentificationFieldType::RelativeWord);
        // 头长按规范 Figure 52 由模式唯一决定为 3 字节。
        EXPECT_EQ(result.Value().identification_header_bytes, 3U);
    }
    {
        RuntimeProfileSlave slave;
        slave.daq_properties = 0x10U;  // 仅 kTimestamp，未声明 DYNAMIC
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_EQ(result.ErrorInfo().code,
                  MeasurementErrorCode::DaqConfigurationError);
    }
    {
        RuntimeProfileSlave slave;
        slave.daq_properties = 0x01U;  // 仅 DYNAMIC：C1 后不再要求 kTimestamp
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
    }
    {
        RuntimeProfileSlave slave;
        slave.timestamp_mode = 0x7CU;  // Unknown XCPlite unit code 7.
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_EQ(result.ErrorInfo().code,
                  MeasurementErrorCode::DaqConfigurationError);
    }
    {
        RuntimeProfileSlave slave;
        slave.event_cycle = 6U;
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_EQ(result.ErrorInfo().symbol, "event");
    }
    {
        RuntimeProfileSlave slave;
        slave.event_supported = false;
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_EQ(result.ErrorInfo().symbol, "event");
    }
    {
        RuntimeProfileSlave slave;
        slave.connect_mode_basic = 0xC1U;  // Motorola byte order.
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_NE(result.ErrorInfo().message.find("Intel 字节序"),
                  std::string::npos);
    }
    {
        RuntimeProfileSlave slave;
        slave.event_max_daq_lists = 0U;
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_EQ(result.ErrorInfo().symbol, "event");
    }
    {
        RuntimeProfileSlave slave;
        slave.max_event_channel = 2U;
        auto master = MakeProfileMaster(slave);
        const std::array<A2lEventMetadata, 1> out_of_range{{
            {"event", "event", 4U, 5U, 2U, 1U, 0xFFU},
        }};
        const auto result =
            BuildMeasurementRuntimeProfile(*master, out_of_range);
        ASSERT_FALSE(result.HasValue());
        EXPECT_EQ(result.ErrorInfo().symbol, "event");
    }
    {
        // XCPlite MAX_DAQ 是当前已分配列表数；未配置时为 0，仍应允许
        // 通过 profile 检查并在用户确认后执行动态分配。
        RuntimeProfileSlave slave;
        slave.max_daq = 0U;
        auto master = MakeProfileMaster(slave);
        const std::array<A2lEventMetadata, 2> events{{
            {"first", "first", 0U, 5U, 2U, 1U, 0xFFU},
            {"second", "second", 1U, 5U, 2U, 1U, 0xFFU},
        }};
        const auto result =
            BuildMeasurementRuntimeProfile(*master, events);
        ASSERT_TRUE(result.HasValue()) << result.ErrorInfo().message;
        EXPECT_EQ(slave.queried_events, (std::vector<std::uint16_t>{0U, 1U}));
    }
    {
        RuntimeProfileSlave slave;
        slave.min_daq = 1U;
        auto master = MakeProfileMaster(slave);
        const auto result =
            BuildMeasurementRuntimeProfile(*master, event);
        ASSERT_FALSE(result.HasValue());
        EXPECT_NE(result.ErrorInfo().message.find("从 0 开始"),
                  std::string::npos);
    }
}

TEST(MeasurementRuntimeProfile,
     RejectsEmptyEventEvidenceAndInvalidTimestamp) {
    RuntimeProfileSlave slave;
    auto master = MakeProfileMaster(slave);
    const std::span<const A2lEventMetadata> no_events;
    const auto empty =
        BuildMeasurementRuntimeProfile(*master, no_events);
    ASSERT_FALSE(empty.HasValue());
    EXPECT_EQ(empty.ErrorInfo().code,
              MeasurementErrorCode::DaqConfigurationError);

    const std::array<A2lEventMetadata, 1> event{{
        {"event", "event", 0U, 5U, 2U, 1U, 0xFFU},
    }};
    {
        RuntimeProfileSlave zero_ticks;
        zero_ticks.timestamp_ticks = 0U;
        auto zero_tick_master = MakeProfileMaster(zero_ticks);
        const auto zero_tick =
            BuildMeasurementRuntimeProfile(*zero_tick_master, event);
        ASSERT_FALSE(zero_tick.HasValue());
    }

    slave.timestamp_mode = 0x04U;  // 32-bit 字段但非固定模式。
    const auto not_fixed =
        BuildMeasurementRuntimeProfile(*master, event);
    // C1 后非固定时间戳不再拒绝：时间戳单位未知（0），
    // 由上层显式注入，绝不猜测（帧时间戳标记为 invalid）。
    ASSERT_TRUE(not_fixed.HasValue()) << not_fixed.ErrorInfo().message;
    EXPECT_EQ(not_fixed.Value().timestamp_unit_ns, 0U);
    EXPECT_FALSE(
        not_fixed.Value().used_xcplite_timestamp_unit_convention);
}

}  // namespace
}  // namespace calmcar::xcp
