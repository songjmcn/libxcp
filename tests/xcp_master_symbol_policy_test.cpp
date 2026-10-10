#include <limits>

#include "gtest/gtest.h"
#include "libxcp/a2l/a2l_types.hpp"
#include "tools/xcp_master/symbol_policy.hpp"

namespace {

using calmcar::xcp::a2l::CharacteristicType;
using calmcar::xcp::a2l::SymbolInfo;
using calmcar::xcp::a2l::SymbolKind;
using calmcar::xcp::tools::SymbolReadUnsupportedReason;
using calmcar::xcp::tools::SymbolWriteUnsupportedReason;

TEST(XcpMasterSymbolPolicyTest, AllowsScalarMeasurementReadOnly) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Measurement;
    symbol.element_size_bytes = 4;

    EXPECT_TRUE(SymbolReadUnsupportedReason(symbol).empty());
    EXPECT_EQ(SymbolWriteUnsupportedReason(symbol), "仅支持 CHARACTERISTIC/VALUE 写入");
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
    EXPECT_NE(SymbolReadUnsupportedReason(symbol).find("数组"), std::string::npos);
    symbol.dimensions.clear();

    symbol.daq_bit_offset = 0;
    EXPECT_NE(SymbolReadUnsupportedReason(symbol).find("位字段"), std::string::npos);
    symbol.daq_bit_offset.reset();

    symbol.element_size_bytes = 0;
    EXPECT_NE(SymbolReadUnsupportedReason(symbol).find("固定宽度"), std::string::npos);
    symbol.element_size_bytes = 4;

    symbol.xcp_address = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;
    EXPECT_NE(SymbolReadUnsupportedReason(symbol).find("32 位"), std::string::npos);
}

}  // namespace
