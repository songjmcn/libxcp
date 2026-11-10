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
#include <optional>
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
    Bytes raw;                               ///< 解码得到的原始字节切片副本；数组时为整数组
    bool valid = true;                       ///< 任一元素换算/解码失败时整条样本为 false
    std::string error;                       ///< 解码/换算失败原因（成功时为空）
    bool is_array = false;                   ///< 是否为数组样本（一元素数组也为 true）
    /// @brief 数组物理值；仅当整条数组换算成功时填充，不对外暴露部分结果
    std::vector<MeasurementValue> elements;
};

/**
 * @brief 一个 DTO 帧解码得到的测量帧（1 DTO = 1 frame，架构文档 §11）
 * @details v0.6 起 timestamp 不再直传原始计数，而是
 *          `DaqTimestampConverter` 的回卷安全单调纳秒（架构文档 §12）：
 *          `timestamp_valid=false` 表示单位/位宽未知或未带时间戳段，此时
 *          `timestamp` 无意义、`timestamp_raw` 保留原始计数供上层自行处置
 *          （不猜，B-3/R13）。
 */
/// @brief 时间戳的来源（B4：区分"DTO 自带"与"按周期重建"）
enum class MeasurementTimestampSource : std::uint8_t {
    None = 0,       ///< 本帧无可用时间戳（未配置或单位/位宽未知）
    Dto = 1,        ///< 直接来自 DTO 时间戳字段（非 packed，或 packed 的锚点采样）
    Reconstructed = 2,  ///< packed 下由事件周期 × 降频因子按采样序号重建
};

struct MeasurementFrame {
    std::uint16_t daq_list = 0;              ///< 所属 DAQ List 号
    std::uint8_t odt = 0;                    ///< 所属 ODT 号（0 基）
    std::chrono::nanoseconds timestamp{};    ///< 回卷安全的单调纳秒（valid 时可用）
    std::uint64_t timestamp_raw = 0;         ///< 原始计数（无时间戳段时为 0）
    bool timestamp_valid = false;            ///< 换算成功（位宽+单位均已知）
    /// @brief DTO 计数器原始值（仅 DAQ List 模式置位 DTO_CTR 时存在）
    /// @details 计数器由 Slave 逐帧自增（模 256），本类只转交原始值，
    ///          不做任何补偿；`nullopt` 表示本会话未启用或该帧无计数器段。
    std::optional<std::uint8_t> counter;
    /// @brief 相对上一帧丢失的计数步数（0=无缺口；首帧或未启用亦为 0）
    /// @details 由相邻两帧计数器差值减一得出（模 256），仅表示**观测到的**
    ///          缺口，不等同于精确丢帧原因（传输错误/被覆盖/迟到均可能）。
    std::uint8_t counter_lost_before = 0;
    /// @brief 本帧是同一 DTO 内的第几个采样（0 基；非 packed 恒为 0）
    std::uint8_t sample_index = 0;
    /// @brief 同一 DTO 内共打包了多少个采样（非 packed 恒为 1）
    std::uint8_t sample_count = 1;
    /// @brief 时间戳来源（Dto / Reconstructed / None）
    MeasurementTimestampSource timestamp_source{
        MeasurementTimestampSource::None};
    std::vector<MeasurementSample> samples;  ///< 本帧包含的样本
};

/// @brief 测量帧消费回调（worker 线程执行，非 RX 线程）
using MeasurementCallback = std::function<void(const MeasurementFrame&)>;

}  // namespace calmcar::xcp

#endif  // LIBXCP_MEASUREMENT_SAMPLE_HPP