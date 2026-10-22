#include "slave_registry.hpp"

#include <atomic>
#include <cstddef>

#include "a2l.h"
#include "libxcp/log.hpp"
#include "xcplib.h"

namespace xcp_slave_demo {
namespace {

/// @brief 上一次观察到的 XCP 连接状态，用于主循环边沿检测。
std::atomic<bool> g_last_connected{false};
/// @brief 上一次观察到的 DAQ 运行状态，用于主循环边沿检测。
std::atomic<bool> g_last_daq_running{false};

/// @brief 快速测量事件的稳定名称。
constexpr char kFastEventName[] = "meas_fast_10ms";
/// @brief 慢速测量事件的稳定名称。
constexpr char kSlowEventName[] = "meas_slow_100ms";
/// @brief 标定段的稳定名称。
constexpr char kCalibrationSegmentName[] = "cal";

/// @brief 供 A2L typedef 元数据生成使用的默认标定布局。
const CalibrationParameters kDefaultCalibrationParameters =
    MakeDefaultCalibrationParameters();
/// @brief 被快速 DAQ 事件测量的全局对象。
FastMeasurements meas_fast{};
/// @brief 被慢速 DAQ 事件测量的全局对象。
SlowMeasurements meas_slow{};

/**
 * @brief 注册基础测量数据结构的 A2L typedef。
 */
void RegisterBasicMeasurementTypedef() {
    A2lTypedefBegin(BasicMeasurementValues, &meas_slow.basic,
                    "包含八种 C++ 基础类型的慢速测量值");
    A2lTypedefMeasurementComponent(signed_8, "有符号 8 位测量值");
    A2lTypedefMeasurementComponent(unsigned_8, "无符号 8 位测量值");
    A2lTypedefMeasurementComponent(signed_16, "有符号 16 位测量值");
    A2lTypedefMeasurementComponent(unsigned_16, "无符号 16 位测量值");
    A2lTypedefMeasurementComponent(signed_32, "有符号 32 位测量值");
    A2lTypedefMeasurementComponent(unsigned_32, "无符号 32 位测量值");
    A2lTypedefMeasurementComponent(real_32, "32 位浮点测量值");
    A2lTypedefMeasurementComponent(real_64, "64 位浮点测量值");
    A2lTypedefEnd();
}

/**
 * @brief 注册快速测量状态及完整快速测量对象的嵌套 A2L typedef。
 */
void RegisterFastMeasurementTypedef() {
    A2lTypedefBegin(FastMeasurementState, &meas_fast.state,
                    "快速测量算法状态结构");
    A2lTypedefMeasurementComponent(source_tick, "算法使用的快速采样序号");
    A2lTypedefMeasurementComponent(raw_output, "限幅前的算法输出");
    A2lTypedefMeasurementComponent(diagnostic_code, "标定参数校验诊断码");
    A2lTypedefEnd();

    A2lTypedefBegin(FastMeasurements, &meas_fast, "10 ms 快速事件测量对象");
    A2lTypedefMeasurementComponent(tick, "快速事件采样序号");
    A2lTypedefMeasurementComponent(input, "模拟算法输入");
    A2lTypedefMeasurementComponent(output, "标定后的算法输出");
    A2lTypedefComponent(state, FastMeasurementState, 1);
    A2lTypedefMeasurementArrayComponent(samples, "八通道快速测量数组");
    A2lTypedefEnd();
}

/**
 * @brief 注册慢速统计结构及完整慢速测量对象的嵌套 A2L typedef。
 */
void RegisterSlowMeasurementTypedef() {
    A2lTypedefBegin(SlowMeasurementStatistics, &meas_slow.statistics,
                    "慢速统计结果结构");
    A2lTypedefMeasurementComponent(fast_cycle_count, "最近快速循环序号");
    A2lTypedefMeasurementComponent(sample_count, "累计慢速样本数");
    A2lTypedefMeasurementComponent(mean_output, "快速输出累计均值");
    A2lTypedefMeasurementComponent(last_diagnostic_code, "最近诊断码");
    A2lTypedefEnd();

    A2lTypedefBegin(SlowMeasurements, &meas_slow, "100 ms 慢速事件测量对象");
    A2lTypedefComponent(basic, BasicMeasurementValues, 1);
    A2lTypedefComponent(statistics, SlowMeasurementStatistics, 1);
    A2lTypedefMeasurementArrayComponent(history, "八元素快速采样历史");
    A2lTypedefEnd();
}

/**
 * @brief 注册基础标定值的 A2L typedef。
 */
void RegisterBasicCalibrationTypedef() {
    A2lTypedefBegin(BasicCalibrationValues,
                    &kDefaultCalibrationParameters.basic,
                    "包含八种 C++ 基础类型的标定值");
    A2lTypedefParameterComponent(signed_8, "有符号 8 位标定值", "", -128, 127);
    A2lTypedefParameterComponent(unsigned_8, "无符号 8 位标定值", "", 0, 255);
    A2lTypedefParameterComponent(signed_16, "有符号 16 位标定值", "", -32768,
                                 32767);
    A2lTypedefParameterComponent(unsigned_16, "无符号 16 位标定值", "", 0,
                                 65535);
    A2lTypedefParameterComponent(signed_32, "有符号 32 位标定值", "",
                                 -2147483648.0, 2147483647.0);
    A2lTypedefParameterComponent(unsigned_32, "无符号 32 位标定值", "", 0,
                                 4294967295.0);
    A2lTypedefParameterComponent(real_32, "32 位浮点标定值", "", -1000000,
                                 1000000);
    A2lTypedefParameterComponent(real_64, "64 位浮点标定值", "", -1000000,
                                 1000000);
    A2lTypedefEnd();
}

/**
 * @brief 注册算法模型和完整标定段对象的嵌套 A2L typedef。
 */
void RegisterCalibrationTypedef() {
    A2lTypedefBegin(CalibrationModel, &kDefaultCalibrationParameters.model,
                    "模拟算法的模型标定结构");
    A2lTypedefParameterComponent(gain, "算法增益", "", 0, 10);
    A2lTypedefParameterComponent(offset, "算法偏置", "", -100, 100);
    A2lTypedefParameterComponent(lower_limit, "算法输出下限", "", -1000, 1000);
    A2lTypedefParameterComponent(upper_limit, "算法输出上限", "", -1000, 1000);
    A2lTypedefEnd();

    A2lTypedefBegin(CalibrationParameters, &kDefaultCalibrationParameters,
                    "包含基础值、模型结构与数组的完整标定对象");
    A2lTypedefComponent(basic, BasicCalibrationValues, 1);
    A2lTypedefComponent(model, CalibrationModel, 1);
    A2lTypedefCurveComponent(corrections, kSampleCount,
                             "每个测量通道的标定修正量", "", -5, 5);
    A2lTypedefEnd();
}

/**
 * @brief 在已经初始化的 A2L writer 中注册测量及标定实例。
 * @param registry 已初始化的事件和标定段句柄。
 */
void RegisterA2lInstances(SlaveRegistry& registry) {
    A2lSetRelativeAddrMode_i(registry.fast_event_id, &meas_fast);
    RegisterFastMeasurementTypedef();
    A2lCreateTypedefInstance(meas_fast, FastMeasurements, "10 ms 快速测量组");

    A2lSetRelativeAddrMode_i(registry.slow_event_id, &meas_slow);
    RegisterBasicMeasurementTypedef();
    RegisterSlowMeasurementTypedef();
    A2lCreateTypedefInstance(meas_slow, SlowMeasurements, "100 ms 慢速测量组");

    RegisterBasicCalibrationTypedef();
    RegisterCalibrationTypedef();
    registry.calibration_segment->CreateA2lTypedefInstance(
        "CalibrationParameters", "独立标定段");
}

}  // namespace

bool InitializeSlaveRegistry(SlaveRegistry& registry, bool create_a2l,
                             std::string* error_message) {
    if (error_message != nullptr) {
        error_message->clear();
    }
    if (!XcpIsActivated()) {
        if (error_message != nullptr) {
            *error_message = "XCP 尚未激活，无法注册事件和标定段";
        }
        LIBXCP_LOG_ERROR("从站注册前置条件不满足：XCP 尚未激活");
        return false;
    }

    // 事件创建顺序是生成进程与服务进程共享的元数据契约。
    registry.fast_event_id =
        XcpCreateEvent(kFastEventName, kFastEventCycleNs, 0U);
    registry.slow_event_id =
        XcpCreateEvent(kSlowEventName, kSlowEventCycleNs, 0U);
    if (registry.fast_event_id == XCP_UNDEFINED_EVENT_ID ||
        registry.slow_event_id == XCP_UNDEFINED_EVENT_ID) {
        if (error_message != nullptr) {
            *error_message = "XCPlite 固定 DAQ 事件注册失败";
        }
        LIBXCP_LOG_ERROR(
            "从站固定 DAQ 事件注册失败：fast={} slow={} 周期 fast={} ns slow={} "
            "ns",
            static_cast<unsigned>(registry.fast_event_id),
            static_cast<unsigned>(registry.slow_event_id), kFastEventCycleNs,
            kSlowEventCycleNs);
        return false;
    }

    registry.calibration_segment.emplace(kCalibrationSegmentName,
                                         &kDefaultCalibrationParameters);
    if (registry.calibration_segment->getIndex() == XCP_UNDEFINED_CALSEG) {
        if (error_message != nullptr) {
            *error_message = "XCPlite 标定段注册失败";
        }
        LIBXCP_LOG_ERROR("从站标定段注册失败：段名={}", kCalibrationSegmentName);
        return false;
    }

    if (create_a2l) {
        RegisterA2lInstances(registry);
    }
    LIBXCP_LOG_INFO(
        "从站注册完成：fast_event={} slow_event={} calseg_index={} 生成A2L={}",
        static_cast<unsigned>(registry.fast_event_id),
        static_cast<unsigned>(registry.slow_event_id),
        static_cast<unsigned>(registry.calibration_segment->getIndex()),
        create_a2l);
    return true;
}

void TriggerFastEvent(const SlaveRegistry& registry) noexcept {
    DaqTriggerEventExt_i(registry.fast_event_id, &meas_fast);
}

void TriggerSlowEvent(const SlaveRegistry& registry) noexcept {
    DaqTriggerEventExt_i(registry.slow_event_id, &meas_slow);
}

FastMeasurements& GetFastMeasurements() noexcept { return meas_fast; }

SlowMeasurements& GetSlowMeasurements() noexcept { return meas_slow; }

bool CopyCalibrationSnapshot(const SlaveRegistry& registry,
                             CalibrationParameters& parameters) noexcept {
    if (!registry.calibration_segment.has_value()) {
        return false;
    }
    {
        const auto guard = registry.calibration_segment->lock();
        parameters = *guard;
    }
    return true;
}

namespace {

/**
 * @brief XCPlite CONNECT 回调，记录主站连接请求的准入结果。
 *
 * 只记录"收到请求"这一事件本身；连接状态的跳变统一由
 * ObserveSlaveProtocolState() 轮询判定，避免两套机制互相抵消。
 * @param mode CONNECT 命令的 mode 字节（0 表示 normal）。
 * @return 始终返回 true，保持默认的"连接总是被接受"语义。
 * @note 该参数是 XCPlite 回调的固定签名要求，不能删除或改名；它仅在日志
 *       语句中被消费，而日志关闭时 `LIBXCP_LOG_INFO` 展开为空操作、不求值
 *       参数，故标注 `[[maybe_unused]]` 而非删除参数。
 */
bool OnSlaveConnectCallback([[maybe_unused]] std::uint8_t mode) {
    LIBXCP_LOG_INFO("从站协议事件：收到 CONNECT 请求，mode={}，已接受连接",
                    static_cast<unsigned>(mode));
    return true;
}

/**
 * @brief XCPlite DAQ 启动回调，记录 DAQ 已完成启动。
 * @return 固定返回 0，表示成功。
 */
std::uint8_t OnSlaveStartDaqCallback() {
    LIBXCP_LOG_INFO("从站协议事件：DAQ 已启动");
    return 0U;
}

/**
 * @brief XCPlite DAQ 停止回调，记录 DAQ 已停止。
 */
void OnSlaveStopDaqCallback() {
    LIBXCP_LOG_INFO("从站协议事件：DAQ 已停止");
}

}  // namespace

void RegisterSlaveProtocolCallbacks() noexcept {
    ApplXcpRegisterConnectCallback(OnSlaveConnectCallback);
    ApplXcpRegisterStartDaqCallback(OnSlaveStartDaqCallback);
    ApplXcpRegisterStopDaqCallback(OnSlaveStopDaqCallback);
    LIBXCP_LOG_INFO("从站协议回调已注册：CONNECT、START_DAQ、STOP_DAQ");
}

void ObserveSlaveProtocolState() noexcept {
    const bool connected = XcpIsConnected();
    const bool daq_running = XcpIsDaqRunning();

    // 仅在状态跳变时记录，避免 10 ms 级主循环刷日志。
    if (connected != g_last_connected.exchange(connected)) {
        LIBXCP_LOG_INFO("从站协议状态变化：连接 {}",
                        connected ? "已建立" : "已断开");
    }
    if (daq_running != g_last_daq_running.exchange(daq_running)) {
        LIBXCP_LOG_INFO("从站协议状态变化：DAQ {}",
                        daq_running ? "运行中" : "已停止");
    }
}

}  // namespace xcp_slave_demo
