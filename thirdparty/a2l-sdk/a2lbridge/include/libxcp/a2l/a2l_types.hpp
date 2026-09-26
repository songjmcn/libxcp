// =============================================================================
// a2l_types.hpp —— 桥接层对外基础类型（设计文档 §4.1，B-1/B-2/B-3/B-8/B-14）
//
// 本文件不 include 任何 liba2l / a2llib 头：领域枚举在此独立定义，
// SDK DTO → 领域类型的显式映射在 src/a2l_database_impl.cpp 完成（R1）。
// 禁止按枚举序号或 sizeof 推断宽度（B-3）；未列出的类型一律 Unknown。
// =============================================================================

#ifndef LIBXCP_A2L_A2L_TYPES_HPP_
#define LIBXCP_A2L_A2L_TYPES_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "libxcp/a2l/a2l_result.hpp"

namespace calmcar::xcp::a2l {

/// @brief 原始字节序列（拥有语义）
using Bytes = std::vector<std::uint8_t>;

/// @brief 原始字节序列（只读视图）
using BytesView = std::span<const std::uint8_t>;

/**
 * @brief 符号类别
 * @details Structure 在首里程碑中仅保留元数据，不执行读写（B-12）。
 */
enum class SymbolKind : std::uint8_t { Measurement, Characteristic, Structure };

/**
 * @brief CHARACTERISTIC 类型（B-11）
 * @details 五型均建模；执行 Value/连续 ValBlk/定长 Ascii，Curve/Map 仅元数据。
 *          CUBOID/CUBE_4/CUBE_5 归入多维曲线类，同样仅元数据。
 */
enum class CharacteristicType : std::uint8_t {
    None,    ///< 非 CHARACTERISTIC
    Value,   ///< VALUE
    Curve,   ///< CURVE
    Map,     ///< MAP
    Cuboid,  ///< CUBOID（三维，仅元数据）
    Cube4,   ///< CUBE_4（仅元数据）
    Cube5,   ///< CUBE_5（仅元数据）
    ValBlk,  ///< VAL_BLK
    Ascii,   ///< ASCII
};

/**
 * @brief ASAM 数据类型的本工程归一化枚举
 * @details 最终成员及宽度必须由 main HEAD 的 a2lenums.h 逐项核对；
 *          禁止按枚举序号或 sizeof 推断（B-3）。Boolean/BitText/String 在本
 *          工程保留枚举位但无固定宽度，数值读写一律返回 UnsupportedDataType。
 */
enum class AsamDataType : std::uint8_t {
    Unknown,
    UByte,
    SByte,
    UWord,
    SWord,
    ULong,
    SLong,
    ULong64,
    SLong64,
    Float16,
    Float32,
    Float64,
    Boolean,
    BitText,
    String,
};

/// @brief 内存字节序（MSB_LAST = Little Endian）
enum class ByteOrder : std::uint8_t { MsbLast, MsbFirst };

/// @brief 地址粒度（一个 XCP 地址单位对应的字节数：1/2/4）
enum class AddressGranularity : std::uint8_t { Byte = 1, Word = 2, Dword = 4 };

/// @brief 数组内存主序
enum class ArrayOrder : std::uint8_t { RowMajor, ColumnMajor };

/// @brief ASAM 类型 → 单元素字节宽；无固定宽度者返回 0（调用方须判 0
/// 报错，B-3）
[[nodiscard]] constexpr std::uint8_t ElementSizeOf(AsamDataType type) noexcept;

/// @brief 地址粒度 → 每地址单位字节数
[[nodiscard]] constexpr std::uint8_t AgToBytes(AddressGranularity ag) noexcept;

/**
 * @brief 数组一维的归一化描述（B-2）
 * @details source_lower_bound 保留 A2L 原始下界；API 索引一律 0 基。
 */
struct Dimension {
    std::int64_t source_lower_bound = 0;  ///< A2L 原始下界
    std::uint64_t extent = 0;             ///< 元素数量
    std::uint64_t byte_stride = 0;        ///< 该维索引增加 1 时的实际字节跨度
};

/**
 * @brief 不丢失 64-bit 整数与文本换算结果的物理值（B-14）
 * @details IDENTICAL 整数保精度用 int64/uint64；数值换算结果用 double；
 *          TAB_VERB 用 string；布尔用 bool。
 */
using PhysicalValue =
    std::variant<std::int64_t, std::uint64_t, double, std::string, bool>;

/// @brief COMPU_METHOD 的归一化类别（B-8）
enum class ConversionKind : std::uint8_t {
    Identical,           ///< 物理值 == 原始值
    Linear,              ///< p = f + i*C + i*O
    RatFunc,             ///< p = (N1 i² + N2 i + N3)/(D1 i² + D2 i + D3)
    TabIntp,             ///< 查表线性插值
    TabNoIntp,           ///< 查表取阶梯
    TabVerb,             ///< 文本表
    FormulaUnsupported,  ///< FORM：仅保留原文，不执行（B-8）
    None,                ///< 无 COMPU_METHOD
};

/// @brief 线性系数（顺序与 A2L LINEAR 一致：C 比例、O 偏移、F 常数）
struct LinearCoefficients {
    double c = 0.0;  ///< 比例项系数
    double o = 0.0;  ///< 偏移项（加在 i*O 上）
    double f = 0.0;  ///< 常数项
};

/// @brief 有理函数系数（RAT_FUNC 六项，顺序 N1,N2,N3,D1,D2,D3）
struct RatFuncCoefficients {
    double n1 = 0.0;  ///< 分子二次项
    double n2 = 0.0;  ///< 分子一次项
    double n3 = 0.0;  ///< 分子常数项
    double d1 = 0.0;  ///< 分母二次项
    double d2 = 0.0;  ///< 分母一次项
    double d3 = 0.0;  ///< 分母常数项
};

/// @brief 数值表或文本表的一项；区间端点仅在对应 A2L 表类型存在时使用
struct ConversionTableEntry {
    double input_min = 0.0;       ///< 原始值区间下界
    double input_max = 0.0;       ///< 原始值区间上界（TAB_INTP 插值用）
    bool numeric_output = false;  ///< 输出是否为数值（TAB_VERB 为 false）
    PhysicalValue output;         ///< 换算输出（数值或文本）
};

/// @brief 与 ConversionKind 配套的载荷（tagged variant，B-8）
using ConversionPayload =
    std::variant<std::monostate, LinearCoefficients, RatFuncCoefficients,
                 std::vector<ConversionTableEntry>, std::string>;

/// @brief 物理值换算描述
struct ConversionInfo {
    std::string compu_method_name;  ///< COMPU_METHOD 名（空 = 无转换）
    std::string unit;  ///< PHYS_UNIT 优先，空时解析 UNIT_REF（B-10）
    ConversionKind kind = ConversionKind::Identical;
    ConversionPayload payload;         ///< 按 kind 选分支
    std::vector<double> lower_limits;  ///< COMPU_METHOD LOWER_LIMIT（可空）
    std::vector<double> upper_limits;  ///< COMPU_METHOD UPPER_LIMIT（可空）
};

/// @brief 单个符号的地址、类型和布局信息
struct SymbolInfo {
    std::string module_name;  ///< 所属 MODULE；规范键 module::name（B-13/B-17）
    std::string name;         ///< 符号名
    std::string description;  ///< 描述文本
    SymbolKind kind = SymbolKind::Measurement;
    CharacteristicType characteristic_type = CharacteristicType::None;
    AsamDataType data_type = AsamDataType::Unknown;
    std::uint64_t xcp_address = 0;  ///< A2L ECU_ADDRESS 原值，不乘 AG（B-1）
    std::uint8_t address_extension =
        0;  ///< 独立 8-bit 字段，不是 address 高位（B-1）
    std::uint8_t element_size_bytes = 0;  ///< 由显式类型映射表取得（B-3）
    std::vector<Dimension>
        dimensions;  ///< 空表示标量；首版执行标量/1D/规则连续 2D
    ArrayOrder array_order = ArrayOrder::RowMajor;
    ByteOrder byte_order = ByteOrder::MsbLast;
    std::optional<std::uint32_t>
        daq_bit_offset;          ///< ODT entry 位定位，与 BIT_MASK 分离（B-9）
    std::uint64_t bit_mask = 0;  ///< 数值提取；首版只接受连续掩码
    std::optional<std::uint64_t>
        error_mask;                  ///< 仅保存；首版不由它推导 valid（B-9）
    bool read_write = false;         ///< 是否可写
    ConversionInfo conversion;       ///< 换算描述
    std::string ref_memory_segment;  ///< REF_MEMORY_SEGMENT（可空）
    bool have_limit = false;         ///< 是否声明了符号级限值
    double lower_limit = 0.0;        ///< 符号级下限（have_limit 时有效）
    double upper_limit = 0.0;        ///< 符号级上限（have_limit 时有效）
};

// ---------------------------------------------------------------------------
// B-1 地址计算（公开辅助，供 DaqLayout / MemoryAccess 复用）
// ---------------------------------------------------------------------------

/**
 * @brief 按元素索引计算 XCP 元素地址（B-1）
 *
 * 公式：byte_offset = element_index * element_size_bytes；
 *       前置条件 byte_offset % AG == 0；
 *       element_address = xcp_address + byte_offset / AG。
 * ADDRESS_GRANULARITY 绝不乘入基地址；address_extension 不参与进位。
 *
 * @param symbol 符号快照（提供基地址、元素宽）
 * @param element_index 0 基元素索引（B-2）
 * @param ag 地址粒度（来自 IF_DATA XCP ProtocolLayer）
 * @return 成功时返回元素地址；乘法溢出、加法溢出或未对齐时返回结构化错误
 *         （AddressOverflow），调用方据此不得向 ECU 发包。
 */
[[nodiscard]] Result<std::uint64_t> ComputeElementAddress(
    const SymbolInfo& symbol, std::uint64_t element_index,
    AddressGranularity ag) noexcept;

/**
 * @brief 校验并返回符号的总元素数（标量返回 1）
 *
 * 规则性检查（B-2）：维度乘积溢出、任一维 stride ≠ 低维乘积 × 元素宽
 * → InvalidLayout（禁止扁平化猜测）。
 *
 * @param symbol 符号快照
 * @return 总元素数，或结构化错误
 */
[[nodiscard]] Result<std::uint64_t> CountElements(
    const SymbolInfo& symbol) noexcept;

// ---------------------------------------------------------------------------
// constexpr 实现
// ---------------------------------------------------------------------------

constexpr std::uint8_t ElementSizeOf(AsamDataType type) noexcept {
    switch (type) {
        case AsamDataType::UByte:
        case AsamDataType::SByte:
            return 1;
        case AsamDataType::UWord:
        case AsamDataType::SWord:
        case AsamDataType::Float16:
            return 2;
        case AsamDataType::ULong:
        case AsamDataType::SLong:
        case AsamDataType::Float32:
            return 4;
        case AsamDataType::ULong64:
        case AsamDataType::SLong64:
        case AsamDataType::Float64:
            return 8;
        default:
            // Unknown/Boolean/BitText/String：无固定宽度，调用方必须报
            // UnsupportedDataType（B-3 禁止猜默认宽度）
            return 0;
    }
}

constexpr std::uint8_t AgToBytes(AddressGranularity ag) noexcept {
    return static_cast<std::uint8_t>(ag);
}

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_A2L_TYPES_HPP_
