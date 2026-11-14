/**
 * @file poll_types.hpp
 * @brief Poll（主站主动轮询）采集路径的公开值类型。
 *
 * Poll 模式与 DAQ 模式的根本区别：这里**没有**从机侧事件概念，也没有
 * DAQ List / ODT / DTO。主站在本地按固定周期主动读取变量内存
 * （SHORT_UPLOAD / UPLOAD），因此"event"完全由主站定义。
 * 详见 code-plan/Poll模式多Event架构分析.md 与实施计划 §3。
 */

#ifndef CALMCAR_XCP_POLL_POLL_TYPES_HPP_
#define CALMCAR_XCP_POLL_POLL_TYPES_HPP_

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "libxcp/measurement/measurement_types.hpp"

namespace calmcar::xcp {

/**
 * @brief 一个 poll 采集任务（主站自定义 event）的定义。
 * @details `name` 在同一个 `PollSession` 内必须唯一；`symbols` 由
 *          `IMeasurementDatabase::Find` 解析为地址/宽度，不由 A2L 的
 *          Event 通道驱动（`MeasurementSymbolInfo::event_channel` 在 poll
 *          路径上被忽略，它是 DAQ 路径的分组依据）。
 */
struct PollEventSpec {
    std::string name;                        ///< 任务名（会话内唯一）
    std::chrono::milliseconds period{20};    ///< 采样周期，下限 20 ms
    std::vector<std::string> symbols;        ///< 待采集变量名（经数据库解析）
};

/**
 * @brief 单个变量在一次 tick 中的采集结果。
 * @details 读取或物理换算失败时 `valid=false`，仅该变量受影响，
 *          **不**丢弃整个 tick 的其余变量（错误隔离，实施计划 §4.2）。
 */
struct PollVariableSample {
    std::string symbol;        ///< 变量名
    /// @brief 换算后的物理值
    /// @details 仅标量样本使用；数组样本的物理值在 `elements` 中，
    ///          本字段保持默认值无意义。
    MeasurementValue value{};
    bool valid{false};         ///< false=本次读取/换算失败
    std::string error;         ///< 失败原因（成功时为空）
    /// @brief 是否为数组样本（一元素数组也为 true）
    /// @details 与 DAQ 路径 `MeasurementSample::is_array` 同义：由数据库
    ///          报告的布局决定，不因换算失败而改变。
    bool is_array{false};
    /// @brief 数组各元素的物理值
    /// @details 仅当整条数组换算**全部**成功时填充；任一元素失败则清空，
    ///          不对外暴露部分结果（与 DAQ 路径语义一致）。
    std::vector<MeasurementValue> elements;
};

/**
 * @brief 一次 tick 的采集结果。
 * @details 时间戳取主站本地时钟（实施计划 §1.2 已确认），
 *          `steady_time` 用于计算周期偏差，`system_time` 用于与外部日志对齐。
 */
struct PollSample {
    std::string event_name;                             ///< 任务名
    std::chrono::steady_clock::time_point steady_time;  ///< tick 起始（单调）
    std::chrono::steady_clock::time_point steady_time_end;  ///< tick 结束（单调）
    std::chrono::system_clock::time_point system_time;  ///< tick 起始（墙钟）
    std::vector<PollVariableSample> samples;            ///< 本 tick 的变量结果
};

/**
 * @brief poll 采集统计快照。
 * @details poll 模式没有"丢样"概念，只有"整周期超期被跳过"与"读取/回调失败"，
 *          因此不设 dropped 字段（实施计划 §4.4）。
 */
struct PollStatistics {
    std::uint64_t ticks{0};             ///< 实际执行的 tick 数
    std::uint64_t skipped_overruns{0};  ///< 因超期被跳过的 tick 数
    std::uint64_t read_errors{0};       ///< 单变量读取/换算失败累计次数
    std::uint64_t callback_errors{0};   ///< 回调抛异常累计次数
};

/**
 * @brief 采集回调类型。
 * @details 回调在调度线程执行，**不得**在回调内阻塞或再次调用本会话的
 *          阻塞接口；回调抛出的任何异常都会被捕获并计入 `callback_errors`，
 *          不会终止调度线程。
 */
using PollCallback = std::function<void(const PollSample&)>;

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_POLL_POLL_TYPES_HPP_