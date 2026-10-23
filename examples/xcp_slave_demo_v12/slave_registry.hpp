/**
 * @file slave_registry.hpp
 * @brief XCPlite V1.2 从站的 DAQ 事件、标定段与 A2L 注册。
 *
 * 与 `slave_model.hpp` 的分工：模型层是纯 C++（可单独单测），本层负责把模型
 * 变量登记到 XCPlite V1.2 的 A2L 生成器与 DAQ 事件上。
 *
 * @note 本文件包含 XCPlite V1.2 的 C 头文件，只允许被示例可执行程序使用，
 *       不得被 `tests/` 的模型单测包含（计划 §8）。
 */
#ifndef XCP_SLAVE_DEMO_V12_SLAVE_REGISTRY_HPP_
#define XCP_SLAVE_DEMO_V12_SLAVE_REGISTRY_HPP_

#include <optional>
#include <string>

#include "slave_model.hpp"
// XCPlite V1.2 公共头：tXcpEventId / tXcpCalSegIndex 等类型定义在此。
// 注意包含顺序：xcplib.hpp 的 CalSegGuard 调用 A2lLock/A2lUnlock，
// 而 xcplib.h 不包含 a2l.h，故必须先包含 a2l.h。
#include "a2l.h"
#include "xcplib.h"
#include "xcplib.hpp"

namespace xcp_slave_demo_v12 {

/// @brief 快速事件周期，单位纳秒（10 ms）。
inline constexpr std::uint32_t kFastCycleNs = 10'000'000U;
/// @brief 慢速事件周期，单位纳秒（100 ms）。
inline constexpr std::uint32_t kSlowCycleNs = 100'000'000U;
/// @brief 快速事件在 A2L 中的稳定名称。
inline constexpr char kFastEventName[] = "meas_fast_10ms";
/// @brief 慢速事件在 A2L 中的稳定名称。
inline constexpr char kSlowEventName[] = "meas_slow_100ms";
/// @brief 标定段在 A2L 中的稳定名称。
inline constexpr char kCalibrationSegmentName[] = "cal";

/**
 * @brief 从站注册表：持有 XCPlite 侧的事件与标定段句柄。
 */
struct SlaveRegistry {
    tXcpEventId fast_event_id{XCP_UNDEFINED_EVENT_ID};  ///< 10 ms 事件句柄。
    tXcpEventId slow_event_id{XCP_UNDEFINED_EVENT_ID};  ///< 100 ms 事件句柄。
    /// @brief 10 ms 测量块基址；相对寻址时事件触发必须原样回传该基址。
    const std::uint8_t* fast_base{nullptr};
    /// @brief 100 ms 测量块基址；同上。
    const std::uint8_t* slow_base{nullptr};
    /// @brief 标定段（由 XCPlite 维护参考页/工作页两份副本）。
    std::optional<xcplib::CalSeg<CalibrationParameters>> calibration;
};

/**
 * @brief 注册事件、标定段，并按需登记 A2L。
 * @param registry 输出：事件与标定段句柄。
 * @param fast 快速测量块（A2L 只读取其布局；对象须在服务期间持续存活）。
 * @param slow 慢速测量块（同上）。
 * @param create_a2l 是否登记 A2L 对象（`--generate-a2l` 与服务模式都为 true，
 *                   仅模型单元测试场景为 false）。
 * @param error_message 失败时的中文说明。
 * @return 成功返回 true。
 * @note 前置条件：`XcpInit()` 已调用且 `XcpIsActivated()` 为 true。
 * @note 测量量使用**事件相对地址模式**（`XCP_ADDR_EXT_DYN + 1`，即 addr_ext=3）：
 *       A2L 中记录的是 `(事件号 << 16) | 相对基址偏移`，基址是本进程内的测量块
 *       对象地址，因此 A2L 必须在**每次启动时重新生成**
 *       （`A2L_MODE_WRITE_ALWAYS`），否则基址与本次进程不匹配。
 * @note 为什么不用绝对地址模式（addr_ext=1）：XCPlite V1.2 的
 *       `src/xcp_cfg.h:202` 无条件把 `XCP_ADDR_EXT_APP` 也定义成 0x01，而 CASDD
 *       模式下 `XCP_ADDR_EXT_ABS` 同样是 0x01；`src/xcpLite.c:2052` 的采样循环
 *       会把 `ext == XCP_ADDR_EXT_APP` 的 DAQ 条目整段填零，结果是 DAQ 收到的
 *       净荷恒为 0（POLL 走 MTA 路径，不受影响）。改用 addr_ext=3 后既避开该
 *       冲突，也仍然只依赖 XCPlite 的公开 API。
 */
bool InitializeSlaveRegistry(SlaveRegistry& registry,
                             const FastMeasurements& fast,
                             const SlowMeasurements& slow, bool create_a2l,
                             std::string* error_message);

/**
 * @brief 触发快速事件（采样快速测量块的当前值）。
 * @param registry 已初始化的注册表。
 */
void TriggerFastEvent(const SlaveRegistry& registry) noexcept;

/**
 * @brief 触发慢速事件（采样慢速测量块的当前值）。
 * @param registry 已初始化的注册表。
 */
void TriggerSlowEvent(const SlaveRegistry& registry) noexcept;

/**
 * @brief 读取当前**活动标定页**的一致性快照。
 * @param registry 已初始化的注册表。
 * @param parameters 输出：当前页参数副本。
 * @return 标定段存在时返回 true。
 * @note 通过 `XcpLockCalSeg` 加锁读取，故返回的是页切换后的一致快照；本程序
 *       主循环每个周期调用一次，把结果交给模型，保证"写标定 → 输出变化"可见。
 */
bool CopyCalibrationSnapshot(const SlaveRegistry& registry,
                             CalibrationParameters& parameters) noexcept;

}  // namespace xcp_slave_demo_v12

#endif  // XCP_SLAVE_DEMO_V12_SLAVE_REGISTRY_HPP_