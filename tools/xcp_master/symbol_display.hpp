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
 * @brief 将对象类别缩短为两字中文标签，供列表行使用。
 * @param kind_name 完整 A2L 对象类别名（`KindName()` 输出）。
 * @return 缩写后的类别标签；未知类别原样返回以便暴露新增类型。
 * @details 列表行要在单行内容纳名称、类型与值，类别必须让位给中文缩写；
 *          完整名称在详情弹窗（F3）里仍可见，因此这里不做信息损失补偿。
 */
[[nodiscard]] inline std::string ShortKindName(std::string_view kind_name) {
    if (kind_name == "MEASUREMENT") return "测量";
    if (kind_name == "STRUCTURE") return "结构";
    constexpr std::string_view characteristic_prefix = "CHARACTERISTIC/";
    if (kind_name.starts_with(characteristic_prefix)) {
        return "特性/" +
               std::string(kind_name.substr(characteristic_prefix.size()));
    }
    if (kind_name == "CHARACTERISTIC") return "特性";
    return std::string(kind_name);
}

/**
 * @brief 将 A2L 基础类型缩短为列表行可用的短名。
 * @param type 归一化后的 A2L 基础数据类型。
 * @return 短名（如 `FLOAT32_IEEE` → `F32`、`UBYTE` → `U8`）；未知类型返回
 *         `?`，与详情弹窗里的「未知」区分开的是上下文而非用词。
 * @details 缩写表刻意保持一对一映射，不做任何宽度推断：`F16`/`F32`/`F64`
 *          分别对应三种 IEEE 浮点，`U`/`S` 前缀加位宽对应整型。
 */
[[nodiscard]] inline std::string ShortDataTypeName(
    calmcar::xcp::a2l::AsamDataType type) {
    using calmcar::xcp::a2l::AsamDataType;
    switch (type) {
        case AsamDataType::UByte:
            return "U8";
        case AsamDataType::SByte:
            return "S8";
        case AsamDataType::UWord:
            return "U16";
        case AsamDataType::SWord:
            return "S16";
        case AsamDataType::ULong:
            return "U32";
        case AsamDataType::SLong:
            return "S32";
        case AsamDataType::ULong64:
            return "U64";
        case AsamDataType::SLong64:
            return "S64";
        case AsamDataType::Float16:
            return "F16";
        case AsamDataType::Float32:
            return "F32";
        case AsamDataType::Float64:
            return "F64";
        case AsamDataType::Boolean:
            return "BOOL";
        case AsamDataType::BitText:
            return "BIT";
        case AsamDataType::String:
            return "STR";
        case AsamDataType::Unknown:
        default:
            return "?";
    }
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
 * @brief 组合列表行显示的对象类别、基础类型和数据形态标签。
 * @param symbol A2L 符号信息。
 * @param kind_name 已有类别格式化结果（`KindName()` 的完整输出）。
 * @param compact 是否为窄终端（跳过「数组」二字的重复修饰）。
 * @return 类别、基础类型和形态组成的短标签，例如
 *         `特性/VALUE F32 标量`、`测量 U16 [128]`。
 * @details 列表行是宽度瓶颈，因此三类信息都用短名并只用单空格分隔；
 *          标量形态省略后缀——没有形态后缀即表示标量，这是列表里最常见的
 *          情况，省去后每行平均少 6 列。完整名称（`MEASUREMENT`、
 *          `FLOAT32_IEEE`、`一维数组[128]`）在详情弹窗里照常显示。
 */
[[nodiscard]] inline std::string FormatSymbolTypeLabel(
    const calmcar::xcp::a2l::SymbolInfo& symbol, std::string_view kind_name,
    bool compact) {
    const SymbolTypeDescription description = DescribeSymbolType(symbol);
    const std::string kind = detail::ShortKindName(kind_name);
    std::string shape = description.data_shape;
    // 标量是默认形态，省略后缀即可；其余形态必须保留，否则会与标量混淆。
    if (shape == "标量") {
        shape.clear();
    } else if (compact) {
        // 窄屏去掉「数组」修饰：「一维数组[128]」「数组/维度未解析」都只留
        // 方括号与斜杠后的部分；二维及以上必须保留「N维」，否则维度数丢失。
        for (const std::string_view marker :
             {std::string_view("数组/"), std::string_view("一维数组")}) {
            if (shape.starts_with(marker)) {
                shape = shape.substr(marker.size());
                break;
            }
        }
    }
    std::string label = kind + " " +
                        detail::ShortDataTypeName(symbol.data_type);
    if (!shape.empty()) label += " " + shape;
    return label;
}

}  // namespace calmcar::xcp::tools
