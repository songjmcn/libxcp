#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::tools {

/**
 * @brief 一个 A2L 符号的 UI 类型描述。
 */
struct SymbolTypeDescription {
    std::string base_type;      ///< 基础数据类型名称。
    std::string data_shape;     ///< 数据形态及已知维度。
    std::string element_width;  ///< 单元素字节宽度或未知状态。
};

/**
 * @brief 将归一化 A2L 数据类型映射为显示名称。
 * @param type A2L 归一化数据类型。
 * @return 标准基础类型名；未知值返回“未知”。
 */
[[nodiscard]] inline std::string AsamDataTypeName(
    calmcar::xcp::a2l::AsamDataType type) {
    using calmcar::xcp::a2l::AsamDataType;
    switch (type) {
        case AsamDataType::UByte:
            return "UBYTE";
        case AsamDataType::SByte:
            return "SBYTE";
        case AsamDataType::UWord:
            return "UWORD";
        case AsamDataType::SWord:
            return "SWORD";
        case AsamDataType::ULong:
            return "ULONG";
        case AsamDataType::SLong:
            return "SLONG";
        case AsamDataType::ULong64:
            return "A_UINT64";
        case AsamDataType::SLong64:
            return "A_INT64";
        case AsamDataType::Float16:
            return "FLOAT16_IEEE";
        case AsamDataType::Float32:
            return "FLOAT32_IEEE";
        case AsamDataType::Float64:
            return "FLOAT64_IEEE";
        case AsamDataType::Boolean:
            return "BOOLEAN";
        case AsamDataType::BitText:
            return "BIT_TEXT";
        case AsamDataType::String:
            return "STRING";
        case AsamDataType::Unknown:
        default:
            return "未知";
    }
}

namespace detail {

/**
 * @brief 格式化符号维度，遇到零或缺失范围时不猜测。
 * @param symbol A2L 符号。
 * @return 乘号分隔的维度长度；无法确定时返回空串。
 */
[[nodiscard]] inline std::string DimensionExtents(
    const calmcar::xcp::a2l::SymbolInfo& symbol) {
    if (symbol.dimensions.empty()) return {};

    std::string result;
    for (std::size_t index = 0; index < symbol.dimensions.size(); ++index) {
        const std::uint64_t extent = symbol.dimensions[index].extent;
        if (extent == 0U) return {};
        if (!result.empty()) result += "×";
        result += std::to_string(extent);
    }
    return result;
}

/**
 * @brief 将对象类别缩短为适合窄终端的标签。
 * @param kind_name 完整 A2L 对象类别名。
 * @return 窄屏类别标签。
 */
[[nodiscard]] inline std::string CompactKindName(std::string_view kind_name) {
    if (kind_name == "MEASUREMENT") return "MEAS";
    if (kind_name == "STRUCTURE") return "STRUCT";
    constexpr std::string_view characteristic_prefix = "CHARACTERISTIC/";
    if (kind_name.starts_with(characteristic_prefix)) {
        return "CHAR/" +
               std::string(kind_name.substr(characteristic_prefix.size()));
    }
    if (kind_name == "CHARACTERISTIC") return "CHAR";
    return std::string(kind_name);
}

/**
 * @brief 由 A2L 维度信息生成通用数组形态。
 * @param symbol A2L 符号。
 * @return 数组形态；维度缺失或不完整时返回“数组/维度未解析”。
 */
[[nodiscard]] inline std::string ArrayShape(
    const calmcar::xcp::a2l::SymbolInfo& symbol) {
    const std::string extents = DimensionExtents(symbol);
    if (extents.empty()) return "数组/维度未解析";
    const std::size_t dimension_count = symbol.dimensions.size();
    if (dimension_count == 1U) return "一维数组[" + extents + "]";
    return std::to_string(dimension_count) + "维数组[" + extents + "]";
}

}  // namespace detail

/**
 * @brief 从已有 A2L 元数据生成基础类型、数据形态和元素宽度说明。
 * @param symbol A2L 符号信息。
 * @return 不根据字节宽度推断基础类型的显示描述。
 */
[[nodiscard]] inline SymbolTypeDescription DescribeSymbolType(
    const calmcar::xcp::a2l::SymbolInfo& symbol) {
    using calmcar::xcp::a2l::CharacteristicType;
    using calmcar::xcp::a2l::SymbolKind;

    SymbolTypeDescription description;
    description.base_type = AsamDataTypeName(symbol.data_type);
    if (symbol.element_size_bytes != 0U) {
        description.element_width =
            std::to_string(symbol.element_size_bytes) + " B";
    } else if (symbol.data_type == calmcar::xcp::a2l::AsamDataType::Unknown) {
        description.element_width = "未知";
    } else {
        description.element_width = "未提供固定宽度";
    }

    if (symbol.kind == SymbolKind::Structure) {
        description.data_shape = "结构体";
        return description;
    }
    if (symbol.kind == SymbolKind::Measurement) {
        description.data_shape =
            symbol.dimensions.empty() ? "标量" : detail::ArrayShape(symbol);
        return description;
    }

    switch (symbol.characteristic_type) {
        case CharacteristicType::Value:
            description.data_shape =
                symbol.dimensions.empty() ? "标量" : detail::ArrayShape(symbol);
            break;
        case CharacteristicType::Curve: {
            std::uint64_t point_count = 0;
            if (symbol.fixed_curve_axis.has_value()) {
                point_count = symbol.fixed_curve_axis->point_count;
            }
            if (point_count == 0U && !symbol.dimensions.empty()) {
                point_count = symbol.dimensions.front().extent;
            }
            description.data_shape =
                point_count == 0U
                    ? "曲线/点数未解析"
                    : "曲线[" + std::to_string(point_count) + "点]";
            break;
        }
        case CharacteristicType::Map: {
            const std::string extents = detail::DimensionExtents(symbol);
            description.data_shape =
                symbol.dimensions.size() == 2U && !extents.empty()
                    ? "MAP[" + extents + "]"
                    : "MAP/维度未解析";
            break;
        }
        case CharacteristicType::Cuboid:
        case CharacteristicType::Cube4:
        case CharacteristicType::Cube5: {
            const std::string extents = detail::DimensionExtents(symbol);
            const char* name = "多维曲线";
            if (symbol.characteristic_type == CharacteristicType::Cuboid) {
                name = "CUBOID";
            } else if (symbol.characteristic_type ==
                       CharacteristicType::Cube4) {
                name = "CUBE_4";
            } else {
                name = "CUBE_5";
            }
            description.data_shape =
                extents.empty() ? std::string(name) + "/维度未解析"
                                : std::string(name) + "[" + extents + "]";
            break;
        }
        case CharacteristicType::ValBlk: {
            const std::string extents = detail::DimensionExtents(symbol);
            description.data_shape = extents.empty()
                                         ? "数据块/维度未解析"
                                         : "数据块[" + extents + "]";
            break;
        }
        case CharacteristicType::Ascii: {
            const std::string extents = detail::DimensionExtents(symbol);
            description.data_shape =
                symbol.dimensions.size() == 1U && !extents.empty()
                    ? "ASCII文本[" + extents + "字符]"
                    : "ASCII文本/长度未解析";
            break;
        }
        case CharacteristicType::None:
        default:
            description.data_shape = symbol.dimensions.empty()
                                         ? "形态未解析"
                                         : detail::ArrayShape(symbol);
            break;
    }
    return description;
}

/**
 * @brief 组合列表中显示的对象类别、基础类型和数据形态标签。
 * @param symbol A2L 符号信息。
 * @param kind_name 已有类别格式化结果。
 * @param compact 是否使用窄屏短标签。
 * @return 类别、基础类型和形态组成的单行标签。
 */
[[nodiscard]] inline std::string FormatSymbolTypeLabel(
    const calmcar::xcp::a2l::SymbolInfo& symbol, std::string_view kind_name,
    bool compact) {
    const SymbolTypeDescription description = DescribeSymbolType(symbol);
    const std::string kind =
        compact ? detail::CompactKindName(kind_name) : std::string(kind_name);
    std::string shape = description.data_shape;
    if (compact) {
        const std::size_t array_label = shape.find("数组[");
        if (array_label != std::string::npos) {
            shape = shape.substr(array_label);
        }
    }
    return kind + " · " + description.base_type + " · " + shape;
}

}  // namespace calmcar::xcp::tools
