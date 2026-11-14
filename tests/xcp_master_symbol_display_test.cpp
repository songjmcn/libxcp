#include "tools/xcp_master/symbol_display.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using calmcar::xcp::a2l::AsamDataType;
using calmcar::xcp::a2l::CharacteristicType;
using calmcar::xcp::a2l::Dimension;
using calmcar::xcp::a2l::FixedCurveAxisInfo;
using calmcar::xcp::a2l::SymbolInfo;
using calmcar::xcp::a2l::SymbolKind;
using calmcar::xcp::tools::AsamDataTypeName;
using calmcar::xcp::tools::DescribeSymbolType;
using calmcar::xcp::tools::FormatSymbolTypeLabel;
using calmcar::xcp::tools::detail::ShortDataTypeName;
using calmcar::xcp::tools::detail::ShortKindName;

/**
 * @brief 验证归一化 A2L 基础类型的显示名称。
 */
TEST(XcpMasterSymbolDisplayTest, MapsAsamDataTypesToNames) {
    EXPECT_EQ(AsamDataTypeName(AsamDataType::Unknown), "未知");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::UByte), "UBYTE");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::SByte), "SBYTE");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::UWord), "UWORD");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::SWord), "SWORD");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::ULong), "ULONG");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::SLong), "SLONG");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::ULong64), "A_UINT64");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::SLong64), "A_INT64");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::Float16), "FLOAT16_IEEE");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::Float32), "FLOAT32_IEEE");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::Float64), "FLOAT64_IEEE");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::Boolean), "BOOLEAN");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::BitText), "BIT_TEXT");
    EXPECT_EQ(AsamDataTypeName(AsamDataType::String), "STRING");
}

/**
 * @brief 验证无维度的测量量显示为标量并保留已知元素宽度。
 */
TEST(XcpMasterSymbolDisplayTest, DescribesScalarMeasurement) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Measurement;
    symbol.data_type = AsamDataType::UWord;
    symbol.element_size_bytes = 2;

    const auto description = DescribeSymbolType(symbol);
    EXPECT_EQ(description.base_type, "UWORD");
    EXPECT_EQ(description.data_shape, "标量");
    EXPECT_EQ(description.element_width, "2 B");
}

/**
 * @brief 验证一维测量数组的维度与紧凑标签。
 */
TEST(XcpMasterSymbolDisplayTest, DescribesOneDimensionalArray) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Measurement;
    symbol.data_type = AsamDataType::ULong;
    symbol.element_size_bytes = 4;
    symbol.dimensions.push_back(Dimension{0, 128, 4});

    const auto description = DescribeSymbolType(symbol);
    EXPECT_EQ(description.data_shape, "一维数组[128]");
    EXPECT_EQ(FormatSymbolTypeLabel(symbol, "MEASUREMENT", false),
              "测量 U32 一维数组[128]");
    EXPECT_EQ(FormatSymbolTypeLabel(symbol, "MEASUREMENT", true),
              "测量 U32 [128]");
}

/**
 * @brief 验证二维 MAP 使用两个显式维度显示形态。
 */
TEST(XcpMasterSymbolDisplayTest, DescribesMapDimensions) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Map;
    symbol.data_type = AsamDataType::Float32;
    symbol.element_size_bytes = 4;
    symbol.dimensions.push_back(Dimension{0, 4, 16});
    symbol.dimensions.push_back(Dimension{0, 8, 4});

    EXPECT_EQ(DescribeSymbolType(symbol).data_shape, "MAP[4×8]");
    EXPECT_EQ(FormatSymbolTypeLabel(symbol, "CHARACTERISTIC/MAP", true),
              "特性/MAP F32 MAP[4×8]");
}

/**
 * @brief 验证固定轴点数优先用于曲线形态标签。
 */
TEST(XcpMasterSymbolDisplayTest, DescribesFixedAxisCurve) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Curve;
    symbol.data_type = AsamDataType::Float64;
    symbol.element_size_bytes = 8;
    symbol.dimensions.push_back(Dimension{0, 4, 8});
    symbol.fixed_curve_axis = FixedCurveAxisInfo{0.0, 1.0, 4, {}};

    EXPECT_EQ(DescribeSymbolType(symbol).data_shape, "曲线[4点]");
    EXPECT_EQ(FormatSymbolTypeLabel(symbol, "CHARACTERISTIC/CURVE", false),
              "特性/CURVE F64 曲线[4点]");
}

/**
 * @brief 验证零维度与未知类型显式显示未知状态，不从宽度推断。
 */
TEST(XcpMasterSymbolDisplayTest, DoesNotInferUnknownTypeOrDimensions) {
    SymbolInfo unknown_type;
    unknown_type.kind = SymbolKind::Measurement;
    unknown_type.data_type = AsamDataType::Unknown;
    unknown_type.element_size_bytes = 0;

    const auto unknown_description = DescribeSymbolType(unknown_type);
    EXPECT_EQ(unknown_description.base_type, "未知");
    EXPECT_EQ(unknown_description.data_shape, "标量");
    EXPECT_EQ(unknown_description.element_width, "未知");

    SymbolInfo unknown_dimensions;
    unknown_dimensions.kind = SymbolKind::Measurement;
    unknown_dimensions.data_type = AsamDataType::ULong;
    unknown_dimensions.dimensions.push_back(Dimension{0, 0, 4});
    EXPECT_EQ(DescribeSymbolType(unknown_dimensions).data_shape,
              "数组/维度未解析");
    EXPECT_EQ(FormatSymbolTypeLabel(unknown_dimensions, "MEASUREMENT", true),
              "测量 U32 维度未解析");
}

/**
 * @brief 验证非定宽类型保留基础类型名称并显示宽度未提供。
 */
TEST(XcpMasterSymbolDisplayTest, KeepsVariableWidthTypeNames) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Measurement;
    symbol.data_type = AsamDataType::String;
    symbol.element_size_bytes = 0;

    const auto description = DescribeSymbolType(symbol);
    EXPECT_EQ(description.base_type, "STRING");
    EXPECT_EQ(description.element_width, "未提供固定宽度");
}

/**
 * @brief 验证其他 CHARACTERISTIC 形态不会被误标为数组或标量。
 */
TEST(XcpMasterSymbolDisplayTest, ReportsBlockAndAsciiShapesExplicitly) {
    SymbolInfo block;
    block.kind = SymbolKind::Characteristic;
    block.characteristic_type = CharacteristicType::ValBlk;
    EXPECT_EQ(DescribeSymbolType(block).data_shape, "数据块/维度未解析");

    SymbolInfo ascii;
    ascii.kind = SymbolKind::Characteristic;
    ascii.characteristic_type = CharacteristicType::Ascii;
    ascii.dimensions.push_back(Dimension{0, 16, 1});
    EXPECT_EQ(DescribeSymbolType(ascii).data_shape, "ASCII文本[16字符]");
}

/**
 * @brief 验证短类型名是一对一映射，不做任何宽度推断。
 */
TEST(XcpMasterSymbolDisplayTest, ShortDataTypeNamesAreOneToOne) {
    EXPECT_EQ(ShortDataTypeName(AsamDataType::UByte), "U8");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::SByte), "S8");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::UWord), "U16");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::SWord), "S16");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::ULong), "U32");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::SLong), "S32");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::ULong64), "U64");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::SLong64), "S64");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::Float16), "F16");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::Float32), "F32");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::Float64), "F64");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::Boolean), "BOOL");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::String), "STR");
    EXPECT_EQ(ShortDataTypeName(AsamDataType::Unknown), "?");
}

/**
 * @brief 验证类别缩写覆盖全部 CHARACTERISTIC 形态。
 */
TEST(XcpMasterSymbolDisplayTest, ShortKindNamesCoverAllKinds) {
    EXPECT_EQ(ShortKindName("MEASUREMENT"), "测量");
    EXPECT_EQ(ShortKindName("STRUCTURE"), "结构");
    EXPECT_EQ(ShortKindName("CHARACTERISTIC"), "特性");
    EXPECT_EQ(ShortKindName("CHARACTERISTIC/VALUE"), "特性/VALUE");
    EXPECT_EQ(ShortKindName("CHARACTERISTIC/MAP"), "特性/MAP");
    EXPECT_EQ(ShortKindName("CHARACTERISTIC/CURVE"), "特性/CURVE");
    EXPECT_EQ(ShortKindName("CHARACTERISTIC/VAL_BLK"), "特性/VAL_BLK");
    // 未知类别必须原样透出，避免新增类型被静默吞成固定文案。
    EXPECT_EQ(ShortKindName("SOMETHING_NEW"), "SOMETHING_NEW");
}

/**
 * @brief 标量形态省略「标量」后缀：无后缀即标量。
 */
TEST(XcpMasterSymbolDisplayTest, ScalarShapeIsOmitted) {
    SymbolInfo symbol;
    symbol.kind = SymbolKind::Characteristic;
    symbol.characteristic_type = CharacteristicType::Value;
    symbol.data_type = AsamDataType::Float32;
    symbol.element_size_bytes = 4;

    EXPECT_EQ(FormatSymbolTypeLabel(symbol, "CHARACTERISTIC/VALUE", false),
              "特性/VALUE F32");
    // 完整形态信息仍在 DescribeSymbolType 里，只是不再进列表行。
    EXPECT_EQ(DescribeSymbolType(symbol).data_shape, "标量");
    // 基础类型全名同样保留给详情弹窗使用。
    EXPECT_EQ(AsamDataTypeName(symbol.data_type), "FLOAT32_IEEE");
}

}  // namespace
