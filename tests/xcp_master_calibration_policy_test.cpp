#include <string>

#include "gtest/gtest.h"
#include "libxcp/a2l/a2l_types.hpp"
#include "libxcp/protocol_types.hpp"
#include "tools/xcp_master/calibration_policy.hpp"

namespace {

using calmcar::xcp::ByteOrder;
using A2lByteOrder = calmcar::xcp::a2l::ByteOrder;
using calmcar::xcp::a2l::SymbolInfo;
using calmcar::xcp::tools::EncodedScalarUnsupportedReason;
using calmcar::xcp::tools::ScalarLayoutUnsupportedReason;

TEST(XcpMasterCalibrationPolicyTest, AcceptsMatchingScalarLayout) {
    SymbolInfo symbol;
    symbol.element_size_bytes = 4;
    symbol.byte_order = A2lByteOrder::MsbLast;

    EXPECT_TRUE(
        ScalarLayoutUnsupportedReason(symbol, 2, ByteOrder::Intel).empty());
    EXPECT_TRUE(EncodedScalarUnsupportedReason(symbol, 4, 2).empty());
}

TEST(XcpMasterCalibrationPolicyTest,
     RefusesReadOnlyAndNonValueCharacteristics) {
    SymbolInfo symbol;
    symbol.kind = calmcar::xcp::a2l::SymbolKind::Characteristic;
    symbol.characteristic_type = calmcar::xcp::a2l::CharacteristicType::Value;
    symbol.element_size_bytes = 4;
    symbol.byte_order = A2lByteOrder::MsbLast;
    symbol.read_write = false;

    EXPECT_NE(calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol).find(
                  "未标记为可写"),
              std::string::npos);

    symbol.read_write = true;
    symbol.characteristic_type = calmcar::xcp::a2l::CharacteristicType::Curve;
    EXPECT_FALSE(
        calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol).empty());
}

TEST(XcpMasterCalibrationPolicyTest, RejectsGranularityWidthAndEndianMismatch) {
    SymbolInfo symbol;
    symbol.element_size_bytes = 3;
    symbol.byte_order = A2lByteOrder::MsbLast;

    EXPECT_NE(
        ScalarLayoutUnsupportedReason(symbol, 2, ByteOrder::Intel).find("粒度"),
        std::string::npos);
    EXPECT_NE(ScalarLayoutUnsupportedReason(symbol, 1, ByteOrder::Motorola)
                  .find("字节序"),
              std::string::npos);
    EXPECT_NE(EncodedScalarUnsupportedReason(symbol, 4, 2).find("宽度"),
              std::string::npos);
    EXPECT_FALSE(EncodedScalarUnsupportedReason(symbol, 3, 0).empty());
}

}  // namespace
