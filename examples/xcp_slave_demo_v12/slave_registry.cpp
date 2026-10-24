/**
 * @file slave_registry.cpp
 * @brief DAQ 事件、标定段与 A2L 登记的 XCPlite V1.2 实现。
 */
#include "slave_registry.hpp"

#include <cstdint>

#include "a2l.h"

namespace xcp_slave_demo_v12 {
namespace {

/**
 * @brief 标定段的参考页默认值（静态存储期）。
 * @note A2L 的参数量登记的是**默认页**布局的地址偏移；必须用静态对象而不是
 *       临时对象，否则登记期间地址失效。
 */
const CalibrationParameters kDefaultCalibrationParameters =
    MakeDefaultCalibrationParameters();

/**
 * @brief 登记快速测量块及其 A2L typedef。
 * @param registry 已创建事件的注册表（提供快速事件句柄）。
 * @param fast 快速测量块。
 */
void RegisterFastA2l(SlaveRegistry& registry, const FastMeasurements& fast) {
    // 事件相对地址模式：A2L 记录 (事件号 << 16) | 相对基址偏移，基址即 fast 对象地址。
    // 为什么不用绝对地址模式见头文件 slave_registry.hpp 的说明（V1.2 的 APP/ABS 位撞车）。
    A2lSetRelativeAddrMode_i(registry.fast_event_id, registry.fast_base);

    A2lTypedefBegin(FastMeasurements, &fast, "10 ms 快速事件测量对象");
    A2lTypedefMeasurementComponent(counter, "快速周期计数");
    A2lTypedefMeasurementComponent(sine, "经标定变换的正弦波形");
    A2lTypedefMeasurementComponent(square, "经标定变换的方波波形");
    A2lTypedefMeasurementComponent(saw, "经标定变换的锯齿波形");
    A2lTypedefMeasurementArrayComponent(samples, "八通道正弦叠加逐通道修正量");
    A2lTypedefEnd();

    A2lCreateTypedefInstance(fast, FastMeasurements, "10 ms 快速测量组");
}

/**
 * @brief 登记慢速测量块及其两个嵌套 typedef。
 * @param registry 已创建事件的注册表（提供慢速事件句柄）。
 * @param slow 慢速测量块。
 */
void RegisterSlowA2l(SlaveRegistry& registry, const SlowMeasurements& slow) {
    // 同快速块：事件相对地址模式，基址为 slow 对象地址。
    A2lSetRelativeAddrMode_i(registry.slow_event_id, registry.slow_base);

    // 嵌套类型必须先登记，慢速块 typedef 才能引用它们。
    A2lTypedefBegin(BasicMeasurementValues, &slow.basic, "八种 C++ 基础类型测量值");
    A2lTypedefMeasurementComponent(u8, "无符号 8 位");
    A2lTypedefMeasurementComponent(s8, "有符号 8 位");
    A2lTypedefMeasurementComponent(u16, "无符号 16 位");
    A2lTypedefMeasurementComponent(s16, "有符号 16 位");
    A2lTypedefMeasurementComponent(u32, "无符号 32 位");
    A2lTypedefMeasurementComponent(s32, "有符号 32 位");
    A2lTypedefMeasurementComponent(f32, "32 位浮点");
    A2lTypedefMeasurementComponent(f64, "64 位浮点");
    A2lTypedefEnd();

    A2lTypedefBegin(MeasurementStatistics, &slow.stats, "慢速统计结构");
    A2lTypedefMeasurementComponent(fast_cycles, "已执行的快速周期数");
    A2lTypedefMeasurementComponent(slow_cycles, "已执行的慢速周期数");
    A2lTypedefMeasurementComponent(mean_output, "快速输出累计均值");
    A2lTypedefMeasurementComponent(last_diagnostic, "最近参数校验诊断码");
    A2lTypedefEnd();

    A2lTypedefBegin(SlowMeasurements, &slow, "100 ms 慢速事件测量对象");
    A2lTypedefComponent(basic, BasicMeasurementValues, 1);
    A2lTypedefComponent(stats, MeasurementStatistics, 1);
    A2lTypedefMeasurementArrayComponent(history, "最近八次快速正弦样本");
    A2lTypedefMeasurementComponent(event_tick, "慢速周期计数");
    A2lTypedefEnd();

    A2lCreateTypedefInstance(slow, SlowMeasurements, "100 ms 慢速测量组");
}

/**
 * @brief 登记标定段的 A2L 描述（段相对寻址，主站按地址扩展访问）。
 * @param registry 已创建标定段的注册表。
 */
void RegisterCalibrationA2l(SlaveRegistry& registry) {
    A2lTypedefBegin(CalibrationParameters, &kDefaultCalibrationParameters,
                    "从站标定参数");
    A2lTypedefParameterComponent(gain, "波形增益", "", 0.0, 10.0);
    A2lTypedefParameterComponent(offset, "波形直流偏置", "", -100.0, 100.0);
    A2lTypedefParameterComponent(limit, "输出限幅上界", "", 0.0, 1000.0);
    A2lTypedefParameterComponent(run, "波形推进开关（0=冻结）", "", 0.0, 1.0);
    A2lTypedefCurveComponent(corrections, static_cast<int>(kCorrectionCount),
                             "逐通道修正量", "", -5.0, 5.0);
    A2lTypedefEnd();

    // CalSeg::CreateA2lTypedefInstance 内部会先 A2lLock、切换到段相对寻址模式，
    // 再登记实例，避免与上面测量量的绝对地址模式互相干扰。
    registry.calibration->CreateA2lTypedefInstance("CalibrationParameters",
                                                   "从站标定段（参考页/工作页）");
}

}  // namespace

bool InitializeSlaveRegistry(SlaveRegistry& registry,
                             const FastMeasurements& fast,
                             const SlowMeasurements& slow, bool create_a2l,
                             std::string* error_message) {
    if (error_message != nullptr) {
        error_message->clear();
    }
    if (!XcpIsActivated()) {
        if (error_message != nullptr) {
            *error_message = "XCP 尚未激活，无法注册事件与标定段";
        }
        return false;
    }

    // 事件创建顺序是 A2L 与服务进程共享的元数据契约，不可随运行变化。
    registry.fast_event_id =
        XcpCreateEvent(kFastEventName, kFastCycleNs, 0U);
    registry.slow_event_id = XcpCreateEvent(kSlowEventName, kSlowCycleNs, 0U);
    // 相对寻址的基址必须与 A2L 登记、每次事件触发使用同一份指针。
    registry.fast_base = reinterpret_cast<const std::uint8_t*>(&fast);
    registry.slow_base = reinterpret_cast<const std::uint8_t*>(&slow);
    if (!XcpIsActivated() ||
        registry.fast_event_id == XCP_UNDEFINED_EVENT_ID ||
        registry.slow_event_id == XCP_UNDEFINED_EVENT_ID) {
        if (error_message != nullptr) {
            *error_message = "XCPlite 事件创建失败（fast/slow 句柄无效）";
        }
        return false;
    }

    registry.calibration.emplace(kCalibrationSegmentName,
                                 &kDefaultCalibrationParameters);
    if (registry.calibration->getIndex() == XCP_UNDEFINED_CALSEG) {
        if (error_message != nullptr) {
            *error_message =
                "XCPlite 标定段创建失败（请确认已定义 OPTION_CAL_SEGMENTS）";
        }
        return false;
    }

    if (create_a2l) {
        RegisterFastA2l(registry, fast);
        RegisterSlowA2l(registry, slow);
        RegisterCalibrationA2l(registry);
    }
    return true;
}

void TriggerFastEvent(const SlaveRegistry& registry) noexcept {
    // addr_ext=3 的基址必须随事件传入：XCPlite 的 XcpEventExt_Var 把第 1 个变参
    // 放到 bases[2]（addr_ext=2，栈帧），第 2 个放到 bases[3]（addr_ext=3，指针相对），
    // 我们用后者，因此第一个变参占位传空指针。
    // 不用 DaqTriggerEventExt_i 宏：它内部展开出的静态句柄标识符不随调用点区分，
    // 同一进程内多个事件共用会导致句柄错乱。
    XcpEventExt_Var(registry.fast_event_id, 2U,
                    static_cast<const std::uint8_t*>(nullptr),
                    registry.fast_base);
}

void TriggerSlowEvent(const SlaveRegistry& registry) noexcept {
    XcpEventExt_Var(registry.slow_event_id, 2U,
                    static_cast<const std::uint8_t*>(nullptr),
                    registry.slow_base);
}

bool CopyCalibrationSnapshot(const SlaveRegistry& registry,
                             CalibrationParameters& parameters) noexcept {
    if (!registry.calibration.has_value()) {
        return false;
    }
    const auto guard = registry.calibration->lock();
    parameters = *guard;
    return true;
}

}  // namespace xcp_slave_demo_v12