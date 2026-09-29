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
 * 仅在 LIBXCP_BUILD_A2L=ON 时构建（独立 MeasurementAdapterTest 目标）。
 */

#include "adapter/a2l/a2l_measurement_database.hpp"

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/measurement/measurement_types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace calmcar::xcp {
namespace {

using a2l::Dimension;
using a2l::ErrorCode;
using a2l::Error;
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

  Result<std::vector<SymbolInfo>> Search(std::string_view /*pattern*/,
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
    return PhysicalValue{
        static_cast<double>(raw.empty() ? 0U : static_cast<unsigned>(raw[0]))};
  }

  Result<a2l::Bytes> FromPhysical(std::string_view /*name*/,
                                  const PhysicalValue& /*value*/) const override {
    return Error(MakeErr(ErrorCode::UnsupportedOperation, "from"));
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

  std::map<std::string, SymbolInfo> symbols;       ///< 符号表
  std::map<std::string, PhysicalValue> conversions; ///< 换算值表
  std::map<std::string, ErrorCode> find_errors;     ///< Find 注入错误
  std::map<std::string, ErrorCode> conv_errors;     ///< ToPhysical 注入错误
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

// ---------------------------------------------------------------------------
// Find 字段映射
// ---------------------------------------------------------------------------

TEST(A2lMeasurementDatabase, FindMapsScalarFieldsWithAddressNarrowing) {
  FakeA2l db;
  // 64 位 xcp_address 带高 32 位脏数据：核心 Address 仅取低 32 位
  db.symbols["EngineSpeed"] = MakeScalar(0x00000001DEADBEEFULL, 0x22U, 4U);
  A2lMeasurementDatabase adapter(db);

  const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("EngineSpeed");
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r.Value().address, 0xDEADBEEFU);
  EXPECT_EQ(r.Value().extension, 0x22U);
  EXPECT_EQ(r.Value().element_size_bytes, 4U);
  EXPECT_EQ(r.Value().element_count, 1U);  // 标量：无维度 → 1
  EXPECT_EQ(r.Value().event_channel, 0U);  // v0.5 默认（SymbolInfo 无事件通道）
}

TEST(A2lMeasurementDatabase, FindArrayElementCountIsProductAndClamped) {
  FakeA2l db;
  SymbolInfo arr = MakeScalar(0x4000U, 0U, 2U);
  arr.dimensions = {Dimension{0, 2, 2}, Dimension{0, 3, 4}};  // 2×3 = 6
  db.symbols["Arr"] = arr;
  SymbolInfo big = MakeScalar(0x5000U, 0U, 1U);
  big.dimensions = {Dimension{0, 16, 16}, Dimension{0, 16, 256}};  // 256 → 截断
  db.symbols["Big"] = big;
  A2lMeasurementDatabase adapter(db);

  const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("Arr");
  ASSERT_TRUE(r.HasValue());
  EXPECT_EQ(r.Value().element_count, 6U);

  const MeasurementResult<MeasurementSymbolInfo> r2 = adapter.Find("Big");
  ASSERT_TRUE(r2.HasValue());
  EXPECT_EQ(r2.Value().element_count, 255U);
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
    const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("missing");
    ASSERT_FALSE(r.HasValue());
    EXPECT_EQ(r.ErrorInfo().code, MeasurementErrorCode::NotFound);
    EXPECT_EQ(r.ErrorInfo().symbol, "missing");  // 透传
    EXPECT_EQ(r.ErrorInfo().message, "injected:missing");
  }
  {
    const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("ambig");
    ASSERT_FALSE(r.HasValue());
    EXPECT_EQ(r.ErrorInfo().code, MeasurementErrorCode::AmbiguousName);
  }
  {
    // 未登记名字走 Fake 默认 NotFound
    const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("unknown");
    ASSERT_FALSE(r.HasValue());
    EXPECT_EQ(r.ErrorInfo().code, MeasurementErrorCode::NotFound);
  }
  {
    const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("bad");
    ASSERT_FALSE(r.HasValue());
    EXPECT_EQ(r.ErrorInfo().code, MeasurementErrorCode::NotFound);  // BadArgument→NotFound
  }
  {
    const MeasurementResult<MeasurementSymbolInfo> r = adapter.Find("addr");
    ASSERT_FALSE(r.HasValue());
    EXPECT_EQ(r.ErrorInfo().code, MeasurementErrorCode::InvalidLayout);  // AddressOverflow
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
  const BytesView raw8{std::array<std::uint8_t, 8>{}};

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

}  // namespace
}  // namespace calmcar::xcp
