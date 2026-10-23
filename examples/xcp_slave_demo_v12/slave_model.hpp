/**
 * @file slave_model.hpp
 * @brief XCPlite V1.2 从站测试程序的纯 C++ 测量/标定模型。
 *
 * @note **禁止**在本文件及 `slave_model.cpp` 中包含任何 XCPlite 或 libxcp 头文件。
 *       模型层需要被 `tests/` 的单元测试直接编译（不链接 `xcplite_v12`），
 *       一旦引入 C 头文件就会强制链接，并与 `XcpliteIntegration` 已链接的
 *       V2.2 `xcplite` 产生符号冲突（计划 §8）。
 *
 * 设计约束：
 * - 波形只依赖**整数推进序号与确定性公式**，不依赖墙钟，保证 DAQ 与 POLL 在
 *   同一时刻可对齐断言（计划 §6）。
 * - 标定参数必须**被算法真实消费**，否则联调只能验证"写成功"，验证不到"起作用"
 *   （计划 §5.2）。
 */
#ifndef XCP_SLAVE_DEMO_V12_SLAVE_MODEL_HPP_
#define XCP_SLAVE_DEMO_V12_SLAVE_MODEL_HPP_

#include <cstddef>
#include <cstdint>

namespace xcp_slave_demo_v12 {

/// @brief 数组型测量/标定的元素个数。
inline constexpr std::size_t kSampleCount = 8U;
/// @brief 每个测量通道的标定修正量个数。
inline constexpr std::size_t kCorrectionCount = 8U;
/// @brief 波形基频的每周期采样点数（决定波形刻意取值的离散度）。
inline constexpr std::uint32_t kPhaseCount = 64U;

/**
 * @brief 标定参数结构（A2L 中作为一个标定段暴露）。
 * @note 必须是平凡可拷贝的 POD，且成员顺序即 A2L 记录布局顺序；禁止加入
 *       构造/析构函数或虚函数，否则 XCPlite 按 `sizeof` 复制整段会与 A2L 布局不一致。
 */
struct CalibrationParameters {
    float gain{1.0F};                          ///< 波形增益，参与所有波形输出。
    float offset{0.0F};                        ///< 波形直流偏置。
    float limit{100.0F};                       ///< 输出限幅上界（绝对值）。
    std::uint8_t run{1U};                      ///< 0=冻结波形推进，非 0=正常运行。
    float corrections[kCorrectionCount]{};     ///< 逐通道修正量，加到 samples[i]。
};

/**
 * @brief 慢速事件的统计结构。
 */
struct MeasurementStatistics {
    std::uint32_t fast_cycles{0U};    ///< 已执行的快速周期数。
    std::uint32_t slow_cycles{0U};    ///< 已执行的慢速周期数。
    float mean_output{0.0F};          ///< 最近快速输出的滑动均值。
    std::int16_t last_diagnostic{0};  ///< 最近一次参数校验诊断码（0=正常）。
};

/**
 * @brief 八种基础类型各一个的测量结构，用于验证 A2L 类型映射。
 */
struct BasicMeasurementValues {
    std::uint8_t u8{0U};     ///< 无符号 8 位。
    std::int8_t s8{0};       ///< 有符号 8 位。
    std::uint16_t u16{0U};   ///< 无符号 16 位。
    std::int16_t s16{0};     ///< 有符号 16 位。
    std::uint32_t u32{0U};   ///< 无符号 32 位。
    std::int32_t s32{0};     ///< 有符号 32 位。
    float f32{0.0F};         ///< 32 位浮点。
    double f64{0.0};         ///< 64 位浮点。
};

/**
 * @brief 快速事件（10 ms）测量的变量块。
 */
struct FastMeasurements {
    std::uint32_t counter{0U};                    ///< 快速周期计数（单调递增）。
    float sine{0.0F};                             ///< 正弦波形，经 gain/offset/limit 变换。
    float square{0.0F};                           ///< 方波波形，同上。
    float saw{0.0F};                              ///< 锯齿波形，同上。
    float samples[kSampleCount]{};                ///< samples[i] = sine + corrections[i]。
};

/**
 * @brief 慢速事件（100 ms）测量的变量块。
 */
struct SlowMeasurements {
    BasicMeasurementValues basic{};               ///< 八种基础类型测量值。
    MeasurementStatistics stats{};                ///< 统计结构。
    float history[kSampleCount]{};                ///< 最近八个快速正弦样本（环形）。
    std::uint32_t event_tick{0U};                 ///< 慢速周期计数。
};

/**
 * @brief 返回出厂默认标定参数。
 * @return 参考页默认值（gain=1、offset=0、limit=100、run=1、corrections 全 0）。
 */
[[nodiscard]] CalibrationParameters MakeDefaultCalibrationParameters() noexcept;

/**
 * @brief 从站测量与标定模型。
 *
 * 生命周期：构造 → 每 10 ms 调 `StepFast()`、每 100 ms 调 `StepSlow()`；
 * 标定值变化时调 `SetCalibration()`（通常来自当前活动标定页的快照）。
 * 本类**不做任何加锁**：调用方须保证单线程推进（本程序由两个专用线程各自调用，
 * 且 `SetCalibration` 只在主循环中调用）。
 */
class SlaveModel {
public:
    /**
     * @brief 构造模型。
     * @param parameters 初始标定参数。
     */
    explicit SlaveModel(const CalibrationParameters& parameters = {}) noexcept;

    /**
     * @brief 覆盖当前标定参数。
     * @param parameters 新的标定快照（来自当前活动标定页）。
     */
    void SetCalibration(const CalibrationParameters& parameters) noexcept;

    /**
     * @brief 读取当前标定参数。
     * @return 最近一次 SetCalibration 的副本。
     */
    [[nodiscard]] const CalibrationParameters& Calibration() const noexcept;

    /// @brief 推进一个快速（10 ms）周期并更新 `Fast()`。
    void StepFast() noexcept;

    /// @brief 推进一个慢速（100 ms）周期并更新 `Slow()`。
    void StepSlow() noexcept;

    /// @brief 快速测量块（只读）。
    [[nodiscard]] const FastMeasurements& Fast() const noexcept;

    /// @brief 慢速测量块（只读）。
    [[nodiscard]] const SlowMeasurements& Slow() const noexcept;

    /// @brief 已推进的快速周期数（用于测试断言确定性）。
    [[nodiscard]] std::uint32_t FastTick() const noexcept;

    /// @brief 已推进的慢速周期数（用于测试断言确定性）。
    [[nodiscard]] std::uint32_t SlowTick() const noexcept;

private:
    /// @brief 按当前标定参数把归一化波形值变换为物理输出（含限幅）。
    /// @param normalized 归一化输入，取值区间 [-1, 1]。
    /// @return gain*input+offset，并按 ±limit 限幅。
    [[nodiscard]] float Shape(float normalized) const noexcept;

    static_assert(sizeof(float) == 4U, "XCP REAL32 需要 4 字节 float");
    static_assert(sizeof(double) == 8U, "XCP REAL64 需要 8 字节 double");

    CalibrationParameters parameters_;  ///< 当前标定快照。
    FastMeasurements fast_{};           ///< 快速测量块。
    SlowMeasurements slow_{};           ///< 慢速测量块。
    std::uint32_t fast_tick_{0U};       ///< 快速周期计数。
    std::uint32_t slow_tick_{0U};       ///< 慢速周期计数。
    float mean_accumulator_{0.0F};      ///< 快速输出累计量（用于均值）。
};

}  // namespace xcp_slave_demo_v12

#endif  // XCP_SLAVE_DEMO_V12_SLAVE_MODEL_HPP_