/**
 * @file measurement_planner.hpp
 * @brief 测量规划器 `MeasurementPlanner`（v0.4，A2L-free）。
 *
 * 依架构文档 §8/§9 与测量子系统代码增长计划 §5.3：`names[] → MeasurementPlan`，
 * 完成"分组 → 装箱 → 路由"规划，输出可直接喂 `ConfigureDaqListsDynamic`
 * 的 `DaqListSpec[]` 与供 ODT Router 绑定的 `MeasurementRoute[]`。
 */

#ifndef CALMCAR_XCP_MEASUREMENT_MEASUREMENT_PLANNER_HPP_
#define CALMCAR_XCP_MEASUREMENT_MEASUREMENT_PLANNER_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_master.hpp"  // DaqListSpec/DaqOdtSpec/DaqEntrySpec

namespace calmcar::xcp {

/**
 * @brief 单个测量的路由（供 DTO 净荷切片绑定 name）
 * @details (daq_list, odt) + payload_offset 定位净荷内一段；size 为该测量
 *          实占字节数（元素宽度 × 元素数）。回调/转换按 name 索引样本。
 */
struct MeasurementRoute {
    std::string name;             ///< 测量名（回调样本用的键）
    std::uint16_t daq_list{0};    ///< DAQ List 号
    std::uint8_t odt{0};          ///< ODT 号（0 基）
    std::size_t payload_offset{0};  ///< 净荷内字节偏移
    std::size_t size{0};          ///< 实占字节数
};

/**
 * @brief 一次 Build 的完整规划结果
 * @details daq_lists 可直接喂 `XcpMaster::ConfigureDaqListsDynamic`；
 *          routes 与 daq_lists 的 (daq_list, odt, 净荷偏移) 一一对应、可回查 name。
 */
struct MeasurementPlan {
    std::vector<DaqListSpec> daq_lists;   ///< 直接可喂 ConfigureDaqListsDynamic
    std::vector<MeasurementRoute> routes;  ///< ODT Router 绑定 name 用
};

/**
 * @brief 变量名列表 → 完整 DAQ 规划（分组/装箱/路由一次成型）
 * @details 装箱策略（架构文档 §9，确定性、不最优）：
 *          1. 按 event_channel 分组（`0=不由通道触发` 单独成组）；
 *          2. 保持用户变量顺序；
 *          3. 顺序填充当前 ODT；单条目入门即超 `max_dto - timestamp_bytes`
 *             时报 `DaqConfigurationError`；
 *          4. 超限开新 ODT；一个变量**不跨 ODT**；一个 Entry = 一个 Measurement；
 *          5. 需要多 ODT/多 List 时校验 Slave 容量（由调用方在 v0.5 会话层核对
 *             `QueryDaqProcessorInfo`/`QueryDaqListInfo`；本类只做确定性装箱）。
 */
class MeasurementPlanner {
public:
    /**
     * @brief 构造规划器
     * @param database 核心窄接口视图（适配器提供；生命周期须长于本对象）
     * @param max_dto  会话 MAX_DTO（来自 `GetSessionParameters().max_dto`）
     * @param address_granularity 会话 AG（来自 `GetSessionParameters()`）
     * @param timestamp_bytes `DaqTimestampBytesCached()` 缓存（0=无/未知，不猜）
     */
    explicit MeasurementPlanner(const IMeasurementDatabase& database,
                                std::uint16_t max_dto,
                                AddressGranularity address_granularity,
                                std::size_t timestamp_bytes = 0);
    ~MeasurementPlanner();

    MeasurementPlanner(const MeasurementPlanner&) = delete;
    MeasurementPlanner& operator=(const MeasurementPlanner&) = delete;

    /// @brief 变量名列表 → 完整规划；任一符号未找到/歧义/无法装箱 → 返回错误
    [[nodiscard]] MeasurementResult<MeasurementPlan> Build(
        const std::vector<std::string>& names) const;

private:
    const IMeasurementDatabase& m_database_;  ///< 只读窄接口视图（非拥有）
    std::uint16_t m_max_dto_{0};              ///< 会话 MAX_DTO
    std::size_t m_ag_bytes_{1};               ///< AG 字节数（AgToBytes）
    std::size_t m_timestamp_bytes_{0};        ///< 信封时间戳头字节（0=无）
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_MEASUREMENT_MEASUREMENT_PLANNER_HPP_