/**
 * @file dto_envelope_types.hpp
 * @brief 统一 DTO 信封类型：识别字段模式、运行时信封布局、信封结构。
 *
 * 依据 code-plan/libxcp_测量子系统代码增长计划.md §5.1（v0.2）。
 * 本组类型是 **A2L-free** 的核心类型：布局只来自会话参数（COMM_MODE_BASIC、
 * GET_DAQ_PROCESSOR_INFO 的 DAQ_KEY_BYTE、GET_DAQ_RESOLUTION_INFO 的时间戳宽度）
 * 与本端 DAQ 账本回填的 pid，绝不 #include 任何 libxcp/a2l 头。
 */

#ifndef CALMCAR_XCP_DAQ_DTO_ENVELOPE_TYPES_HPP_
#define CALMCAR_XCP_DAQ_DTO_ENVELOPE_TYPES_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "libxcp/protocol_types.hpp"  // BytesView / Address 等核心基础类型

namespace calmcar::xcp {

/**
 * @brief DAQ 识别字段类型（DTO 前导段的四种 PID/Identification 模式）
 *
 * 对应 GET_DAQ_PROCESSOR_INFO 响应 DAQ_KEY_BYTE 中 identification 位所声明的
 * 运行时真值（docs L1445）。核心自持此枚举，不依赖 A2L 侧的对应概念。
 */
enum class IdentificationFieldType : std::uint8_t {
    Absolute,            ///< 绝对 ODT 号：识别字段 = PID（1 字节）
    RelativeByte,        ///< PID + DAQ(BYTE)：2 字节，无 FILL
    RelativeWord,        ///< PID + DAQ(WORD)：3 字节，无 FILL
    RelativeWordAligned, ///< PID + FILL + DAQ(WORD)：4 字节（FILL 用于对齐）
};

/**
 * @brief 由 DAQ_KEY_BYTE 的 identification 位域换算识别字段模式
 *
 * 依据原始 PDF 第 96 页 Figure 51/52/53 与第 181-183 页 Table 143：
 *  wire code `00` → Absolute；`01` → RelativeByte；`10` → RelativeWord；
 *  `11` → RelativeWordAligned。
 *
 * @param key_byte_identification_field_type DAQ_KEY_BYTE 的 bit6-7 原值
 * @return 对应模式；取值超出 0..3 时返回 nullopt（不由本函数抛异常，
 *         便于调用方区分「Slave 申报非法」与「解析失败」）
 *
 * @warning 调用方**必须**使用本函数，不得自行按位比较或猜测模式——
 *          F-08 的缺陷正是「解析了 key byte 却没有映射消费」。
 */
[[nodiscard]] constexpr std::optional<IdentificationFieldType>
IdentificationFieldTypeFromKeyByte(
    std::uint8_t key_byte_identification_field_type) noexcept {
    switch (key_byte_identification_field_type) {
        case 0U:
            return IdentificationFieldType::Absolute;
        case 1U:
            return IdentificationFieldType::RelativeByte;
        case 2U:
            return IdentificationFieldType::RelativeWord;
        case 3U:
            return IdentificationFieldType::RelativeWordAligned;
        default:
            return std::nullopt;
    }
}

/**
 * @brief 识别字段的**最小**字节数（不含时间戳、不含计数器）
 *
 * 依据原始 PDF 第 95-96 页 Figure 51/52/53：
 *  - Absolute：1（仅 PID）
 *  - RelativeByte：2（PID + DAQ(BYTE)）
 *  - RelativeWord：3（PID + DAQ(WORD)）
 *  - RelativeWordAligned：4（PID + FILL + DAQ(WORD)）
 *
 * @note 计数器（CTR）**不属于**本长度：对 `RelativeWordAligned`，CTR 会
 *       **取代 FILL 字节**（原始 PDF 第 97-98 页 Figure 56），故头长不变；
 *       其余模式下 CTR 位于识别字段**之后**，由解码器单独消费。
 */
[[nodiscard]] constexpr std::size_t IdentificationFieldMinimumBytes(
    IdentificationFieldType type) noexcept {
    switch (type) {
        case IdentificationFieldType::Absolute:
            return 1U;
        case IdentificationFieldType::RelativeByte:
            return 2U;
        case IdentificationFieldType::RelativeWord:
            return 3U;
        case IdentificationFieldType::RelativeWordAligned:
            return 4U;
    }
    return 1U;
}

/**
 * @brief 该模式的 CTR 是否合并进识别字段（取代 FILL 字节）
 *
 * 依据原始 PDF 第 97-98 页：仅 `(WORD, aligned)` 采用该做法，目的是
 * 「keep the alignment without introducing additional fill bytes」。
 *
 * @param type 识别字段模式
 * @return true 表示 CTR 占用识别字段内的 FILL 槽位，解码器不得在其后
 *         再消费计数器字节
 */
[[nodiscard]] constexpr bool IdentificationFieldMergesCounter(
    IdentificationFieldType type) noexcept {
    return type == IdentificationFieldType::RelativeWordAligned;
}

/**
 * @brief 运行时信封布局（校验描述后续字节如何切分）
 *
 * 本结构由**对话程调用方**依据会话取证构造，解码器只按它解析，不自行取证。
 * 布局来源：
 *  - identification_field_type：来自 GET_DAQ_PROCESSOR_INFO 的 DAQ_KEY_BYTE；
 *  - first_odt：各 DAQ List 第一个 ODT 的绝对号（Absolute 模式换算绝对 ODT）；
 *  - counter_enabled / timestamp_enabled / overflow_indicator / pid_off：
 *    会话 DAQ List 模式的配置位（DaqListModeBit）；
 *  - timestamp_size_bits：来自 GET_DAQ_RESOLUTION_INFO 的时间戳宽度取证
 *    （DaqTimestampBytesCached() 缓存值，0=无/未知，绝不猜测）。
 */
struct DtoFrameLayout {
    IdentificationFieldType identification_field_type =
        IdentificationFieldType::Absolute;  ///< 识别模式
    std::uint8_t first_odt =
        0;  ///< Absolute 模式 FIRST_PID（绝对 ODT 号换算用）
    bool counter_enabled = false;  ///< DTO 计数器使能（DaqListModeBit::kDtoCounter）
    bool timestamp_enabled = false;  ///< 时间戳使能（DaqListModeBit::kTimestamp）
    std::uint8_t timestamp_size_bits =
        0;  ///< 时间戳位宽（bit；来自分辨率取证，不猜）
    bool timestamp_relative_first_only =
        false;  ///< 时间戳只在**本事件首个 ODT 帧**出现（XCPlite 实然 D13：
                ///< odt_rel==0 的帧才带时间戳）。true 时，相对模式下
                ///< identity.odt != first_odt 的帧跳过时间戳段（
                ///< raw_timestamp=nullopt、净荷不前移）；默认 false 保持
                ///< v0.2 口径（每帧都带）。Absolute 模式不受影响。
    bool overflow_indicator = false;  ///< OVERLOAD_INDICATOR 指示位
    bool pid_off = false;  ///< 无识别字段（解码侧须拒绝，B-7）
    std::size_t header_bytes = 1;  ///< 含时间戳在内的信封头长（预检口径）
};

/**
 * @brief DTO 的身份标识：解码后定位到 DAQ List + ODT
 */
struct DtoIdentity {
    std::uint16_t daq_list = 0;  ///< DAQ List 号
    std::uint8_t odt = 0;        ///< ODT 号（0 基 / Absolute 模式为绝对号）
};

/**
 * @brief 统一 DTO 信封解码结果
 *
 * @note payload 是 BytesView，指向调用方传入的 dto 缓冲内部，**不持有底层字节**；
 *       其生命周期不得超过传入的 dto。
 */
struct DtoEnvelope {
    DtoIdentity identity;                        ///< DAQ/ODT 身份
    std::optional<std::uint8_t> counter;         ///< DTO 计数器（若使能）
    std::optional<std::uint64_t> raw_timestamp;  ///< 原始时间戳（若使能与可解析）
    BytesView payload;  ///< 去掉信封头后的净荷段（含/不含扩展字节视模式而定）
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_DAQ_DTO_ENVELOPE_TYPES_HPP_