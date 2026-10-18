/**
 * @file slave_registry.hpp
 * @brief XCPlite 事件、标定段与 A2L 元数据注册接口。
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "slave_model.hpp"
#include "xcplib.hpp"

namespace xcp_slave_demo {

/// @brief 快速 DAQ 事件周期，单位为纳秒。
inline constexpr std::uint32_t kFastEventCycleNs = 10000000U;
/// @brief 慢速 DAQ 事件周期，单位为纳秒。
inline constexpr std::uint32_t kSlowEventCycleNs = 100000000U;

/**
 * @brief 本示例中注册后的 XCPlite 事件及标定段句柄。
 */
struct SlaveRegistry {
    tXcpEventId fast_event_id{XCP_UNDEFINED_EVENT_ID};  ///< 快速 DAQ 事件编号。
    tXcpEventId slow_event_id{XCP_UNDEFINED_EVENT_ID};  ///< 慢速 DAQ 事件编号。
    std::optional<xcp::CalSeg<CalibrationParameters>>
        calibration_segment;  ///< 标定段 RAII 句柄。
};

/**
 * @brief 初始化固定顺序的快速事件、慢速事件及标定段。
 * @param registry 用于保存初始化后的注册句柄。
 * @param create_a2l 是否同时注册运行时 A2L 类型和实例。
 * @param error_message 可选的中文错误说明输出位置。
 * @return 全部注册成功时返回 true。
 */
[[nodiscard]] bool InitializeSlaveRegistry(
    SlaveRegistry& registry, bool create_a2l,
    std::string* error_message = nullptr);

/**
 * @brief 在快速事件中报告测量对象的当前地址基址并触发 DAQ。
 * @param registry 已初始化的事件注册表。
 */
void TriggerFastEvent(const SlaveRegistry& registry) noexcept;

/**
 * @brief 在慢速事件中报告测量对象的当前地址基址并触发 DAQ。
 * @param registry 已初始化的事件注册表。
 */
void TriggerSlowEvent(const SlaveRegistry& registry) noexcept;

/**
 * @brief 获取全局快速测量对象。
 * @return 程序生命周期内有效的快速测量对象引用。
 */
[[nodiscard]] FastMeasurements& GetFastMeasurements() noexcept;

/**
 * @brief 获取全局慢速测量对象。
 * @return 程序生命周期内有效的慢速测量对象引用。
 */
[[nodiscard]] SlowMeasurements& GetSlowMeasurements() noexcept;

/**
 * @brief 在 CalSeg 读锁作用域内复制一份标定参数快照。
 * @param registry 已初始化的事件注册表。
 * @param parameters 用于接收参数快照的对象。
 * @return 标定段存在并成功复制时返回 true。
 */
[[nodiscard]] bool CopyCalibrationSnapshot(
    const SlaveRegistry& registry, CalibrationParameters& parameters) noexcept;

}  // namespace xcp_slave_demo
