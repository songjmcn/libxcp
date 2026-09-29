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
    Absolute,            ///< 绝对 ODT 号：byte0 = 绝对 ODT 号
    RelativeByte,        ///< byte0 = 相对 ODT 号 + byte1 = DAQ 扩展字节
    RelativeWord,        ///< WORD 识别：低字节 DAQ 号，高字节相对 ODT（Intel）
    RelativeWordAligned, ///< WORD 识别且 4 字节对齐（扩展表按 DWORD 对齐）
};

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