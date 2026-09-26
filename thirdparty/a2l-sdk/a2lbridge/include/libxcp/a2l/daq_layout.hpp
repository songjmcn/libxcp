// =============================================================================
// daq_layout.hpp —— DAQ 能力、Event Channel 与 DTO 布局（设计文档 §4.4）
//
// 本头文件被 if_data_xcp.hpp 复用（DaqInfo/EventChannelInfo 前置于其中），
// 因此不依赖 SDK：领域类型独立定义，映射在 src/daq_layout_impl.cpp。
// =============================================================================

#ifndef LIBXCP_A2L_DAQ_LAYOUT_HPP_
#define LIBXCP_A2L_DAQ_LAYOUT_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libxcp/a2l/a2l_result.hpp"
#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::a2l {

/**
 * @brief DAQ 处理器能力（Allocator 的输入约束）
 * @details 首里程碑只启用 STATIC 路径（B-5）；dynamic_supported 仅保留能力位，
 *          使用动态配置时返回 UnsupportedOperation。
 */
struct DaqInfo {
    bool static_supported = false;   ///< 首里程碑唯一启用的执行路径（B-5）
    bool dynamic_supported = false;  ///< 仅保留能力；DynamicDaqNotImplemented
    std::uint16_t max_daq = 0;       ///< MAX_DAQ
    std::uint16_t max_event_channel = 0;  ///< MAX_EVENT_CHANNEL
    std::uint8_t min_daq = 0;             ///< MIN_DAQ
    std::uint8_t odt_entry_min_size_bytes =
        1;                                ///< GRANULARITY_ODT_ENTRY_SIZE_DAQ
    std::uint8_t max_odt_entry_size = 0;  ///< MAX_ODT_ENTRY_SIZE_DAQ

    /// @brief OPTIMISATION_TYPE → ALLOCATOR 打包对齐策略
    enum class OdtType : std::uint8_t {
        Default,
        Odt16,
        Odt32,
        Odt64,
        Alignment,
        MaxEntrySize
    };
    /// @brief ADDRESS_EXTENSION 声明模式
    enum class AddrExtMode : std::uint8_t { Free, PerOdt, PerDaq };
    /// @brief IDENTIFICATION_FIELD_TYPE（DTO envelope 判定依据）
    enum class IdFieldType : std::uint8_t {
        Absolute,
        RelativeByte,
        RelativeWord,
        RelativeWordAligned
    };

    OdtType odt_type = OdtType::Default;  ///< OPTIMISATION_TYPE
    AddrExtMode address_extension_mode =
        AddrExtMode::Free;  ///< ADDRESS_EXTENSION
    IdFieldType identification_field_type =
        IdFieldType::Absolute;             ///< IDENTIFICATION_FIELD_TYPE
    bool dto_counter_supported = false;    ///< DTO_CTR_SUPPORTED
    bool pid_off_supported = false;        ///< PID_OFF_SUPPORTED
    bool prescaler_supported = false;      ///< PRESCALER_SUPPORTED
    bool resume_supported = false;         ///< RESUME_SUPPORTED
    bool overflow_flag_supported = false;  ///< OVERLOAD_INDICATION != NONE
    std::optional<std::uint32_t>
        timestamp_max_size_bits;  ///< TIMESTAMP_SIZE（影响可用载荷）
};

/**
 * @brief Event Channel 描述（含 TIMING 与 DAQ_LIST 引用）
 */
struct EventChannelInfo {
    std::string name;  ///< EVENT_CHANNEL 名字（DAQ_EVENT 引用它）
    std::uint16_t channel_number = 0;  ///< 通道号
    double cycle_time_us = 0.0;        ///< MIN_CYCLE_TIME / CYCLE_TIME
    std::vector<std::uint16_t>
        daq_list_numbers;          ///< 该事件下要 START 的 DAQ LIST
    bool has_consistency = false;  ///< 是否声明 CONSISTENCY
};

/**
 * @brief 一条 DTO 条目的解析结果（供 DtoDecoder 回调使用）
 */
struct DecodedDtoSample {
    std::string symbol_name;    ///< 来自布局快照的符号名（module::name）
    std::uint64_t address = 0;  ///< 条目基地址（ECU_ADDRESS 语义，B-1）
    std::uint8_t address_extension = 0;  ///< 独立扩展字节（B-1）
    Bytes raw;  ///< DTO 中的实际原始字节；AG 不改变字节内容
    PhysicalValue
        physical_value;         ///< 已套用 COMPU_METHOD 的 tagged value（B-14）
    std::optional<bool> valid;  ///< 质量状态；首版不由 ERROR_MASK 猜测（B-9）
};

/**
 * @brief DTO envelope 的运行时冻结布局（B-7）
 * @details envelope
 * 头部字节数由这些标志推导；布局冲突时整帧拒绝（InvalidLayout）。
 */
struct DtoFrameLayout {
    DaqInfo::IdFieldType identification_field_type =
        DaqInfo::IdFieldType::Absolute;    ///< 标识字段类型
    bool first_odt = false;                ///< 携带 FIRST_ODT 字段
    bool counter_enabled = false;          ///< 携带 DTO 计数器
    bool timestamp_enabled = false;        ///< 携带时间戳
    std::uint8_t timestamp_size_bits = 0;  ///< 时间戳位宽（TIMESTAMP_SIZE）
    bool overflow_indicator = false;       ///< 携带 OVERLOAD 指示位
    std::uint8_t header_bytes =
        1;  ///< envelope 固定头（PID）字节数，冻结后不可变
};

/// @brief 一个 ODT 内单条 entry 的冻结信息（布局快照内部元素）
struct OdtEntryLayout {
    std::uint8_t number = 0;  ///< entry 序号（1 基，A2L 源序）
    std::string symbol_name;  ///< 反查到的规范符号名；空表示无匹配（禁止猜测）
    std::uint64_t address = 0;           ///< ECU_ADDRESS 原值（B-1）
    std::uint8_t address_extension = 0;  ///< 独立扩展字节
    std::uint8_t size_bytes = 0;         ///< SIZE（字节数）
    std::uint8_t bit_offset = 0;  ///< BIT_OFFSET（B-9，与 BIT_MASK 分离）
};

/// @brief 一个 ODT 的冻结布局
struct OdtLayout {
    std::uint8_t number = 0;              ///< ODT 序号（1 基）
    std::vector<OdtEntryLayout> entries;  ///< 按 A2L 源顺序排列的条目
};

/// @brief 一个 DAQ_LIST 的冻结布局（STATIC 预定义列表）
struct DaqListLayout {
    std::uint16_t number = 0;     ///< EPK / DAQ 列表号
    std::vector<OdtLayout> odts;  ///< 预定义 ODT 序列
};

class DaqLayoutSnapshot;  // 定义见
                          // src/daq_layout_impl.cpp（不透明，避免暴露内部索引）

/**
 * @class IDaqLayout
 * @brief 冻结布局上的 DTO 解码器（STATIC 路径，B-5/B-7）。
 */
class IDaqLayout {
public:
    IDaqLayout() = default;
    virtual ~IDaqLayout() = default;

    IDaqLayout(const IDaqLayout&) = delete;
    IDaqLayout& operator=(const IDaqLayout&) = delete;
    IDaqLayout(IDaqLayout&&) = delete;
    IDaqLayout& operator=(IDaqLayout&&) = delete;

    /**
     * @brief 解码一帧 DTO
     * @param frame_layout envelope 冻结布局
     * @param dto 完整 DTO 字节（含 envelope 头）
     * @return 每个可归属 entry 的解码样本；envelope 长度与布局不符时
     *         返回 InvalidLayout（整帧拒绝，不做部分解释）
     */
    [[nodiscard]] virtual Result<std::vector<DecodedDtoSample>> Decode(
        const DtoFrameLayout& frame_layout, BytesView dto) const = 0;

    /**
     * @brief 计算指定符号集合打包后的净字节数（不含 envelope 头）
     * @param names 规范键或全库唯一裸名列表（须同属一个已冻结 DAQ_LIST）
     */
    [[nodiscard]] virtual Result<std::size_t> PackedByteSize(
        const std::vector<std::string>& names) const = 0;
};

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_DAQ_LAYOUT_HPP_
