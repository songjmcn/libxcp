/**
 * @file measurement_sample.hpp
 * @brief 测量会话的单样本与帧模型（v0.5，核心自有类型，A2L-free）。
 *
 * 对应测量子系统代码增长计划 §5.4。`MeasurementValue` 来自
 * `measurement_types.hpp`（核心自有五选一变体，镜像桥接层 PhysicalValue）。
 * 本文件只定义数据载体，不涉及任何解码/换算逻辑。
 */

#ifndef LIBXCP_MEASUREMENT_SAMPLE_HPP
#define LIBXCP_MEASUREMENT_SAMPLE_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "libxcp/measurement/measurement_types.hpp"
#include "libxcp/protocol_types.hpp"

namespace calmcar::xcp {

/**
 * @brief 单个测量样本
 *
 * 一条 DTO 净荷切片对一个待测变量的一次取值。`value` 是经
 * `IMeasurementDatabase::ToPhysical` 换算后的物理值；换算或解码失败时
 * 仅该样本 `valid=false`，**不**整体丢弃所在帧（架构文档 §11）。
 */
struct MeasurementSample {
    std::string name;                        ///< 待测变量名（与 A2L 符号一致）
    std::chrono::nanoseconds timestamp{};    ///< 该样本时间戳（v0.5 直传原始时间戳）
    MeasurementValue value{};                ///< 换算后的物理值
    Bytes raw;                               ///< 解码得到的原始字节切片副本
    bool valid = true;                       ///< 该样本换算/解码失败时为 false
};

/**
 * @brief 一个 DTO 帧解码得到的测量帧（1 DTO = 1 frame，架构文档 §11）
 * @details v0.6 起 timestamp 不再直传原始计数，而是
 *          `DaqTimestampConverter` 的回卷安全单调纳秒（架构文档 §12）：
 *          `timestamp_valid=false` 表示单位/位宽未知或未带时间戳段，此时
 *          `timestamp` 无意义、`timestamp_raw` 保留原始计数供上层自行处置
 *          （不猜，B-3/R13）。
 */
struct MeasurementFrame {
    std::uint16_t daq_list = 0;              ///< 所属 DAQ List 号
    std::uint8_t odt = 0;                    ///< 所属 ODT 号（0 基）
    std::chrono::nanoseconds timestamp{};    ///< 回卷安全的单调纳秒（valid 时可用）
    std::uint64_t timestamp_raw = 0;         ///< 原始计数（无时间戳段时为 0）
    bool timestamp_valid = false;            ///< 换算成功（位宽+单位均已知）
    std::vector<MeasurementSample> samples;  ///< 本帧包含的样本
};

/// @brief 测量帧消费回调（worker 线程执行，非 RX 线程）
using MeasurementCallback = std::function<void(const MeasurementFrame&)>;

}  // namespace calmcar::xcp

#endif  // LIBXCP_MEASUREMENT_SAMPLE_HPP