/**
 * @file dto_payload_decoder.hpp
 * @brief ODT 净荷的字节级切片解码（v0.5，核心，A2L-free）。
 *
 * 对应测量子系统代码增长计划 §5.5。本类只做**字节切片**：给定
 * `envelope.payload`（已剥去识别字段/计数/时间戳的 ODT 变量区）与一组
 * 有序切片规格，按 `[offset, offset+size)` 取出原始字节副本。物理换算
 * 一律通过 `IMeasurementDatabase::ToPhysical` 由上层完成（架构文档 §6.3）。
 * 越界切片 → 该样本 valid=false（不整体丢弃帧，decode_errors 由上层计数）。
 */

#ifndef LIBXCP_DAQ_DTO_PAYLOAD_DECODER_HPP
#define LIBXCP_DAQ_DTO_PAYLOAD_DECODER_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp {

/**
 * @brief 单个净荷切片规格（一条 DTO 净荷对一个待测变量）
 */
struct PayloadSlice {
    std::string name;            ///< 待测变量名
    std::size_t offset{0};       ///< 相对净荷起点（envelope.payload@0）的偏移
    std::size_t size{0};         ///< 实占字节数
};

/**
 * @brief 解码得到的一个净荷切片结果
 */
struct DecodedSlice {
    std::string name;            ///< 变量名
    Bytes raw;                   ///< 原始字节副本（越界时为空）
    bool valid = true;           ///< 越界切片为 false
};

/**
 * @brief 按切片规格把净荷逐段取出（纯字节切片，不抛异常）
 * @param payload  已剥去 envelope 的 ODT 变量区字节视图
 * @param specs    有序切片规格
 * @return 与 specs 一一对应的解码结果；`offset+size > payload.size()` 的
 *         切片 valid=false 且 raw 为空
 */
[[nodiscard]] std::vector<DecodedSlice> DecodePayload(
    BytesView payload, const std::vector<PayloadSlice>& specs) noexcept;

}  // namespace calmcar::xcp

#endif  // LIBXCP_DAQ_DTO_PAYLOAD_DECODER_HPP