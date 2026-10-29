/**
 * @file tcp_framing_verdict.hpp
 * @brief 单次帧布局候选验证的三态结论（被拒 / 未决 / 接受）。
 *
 * 依据 code-plan/TCP_auto证据收敛与候选验证_设计.md 第 C3 节实现。
 *
 * @par 为什么要三态
 *   原先的判定谓词只返回 `bool`，把两类完全不同的情况压成一个 `false`：
 *   - **对端明确拒绝**（帧长度非法、响应解析失败、对端主动断开）——这是
 *     "该候选确实不适用于此对端"的证据；
 *   - **纯超时**——对端没有回话，既不能证明支持，也不能证明不支持。
 *   把后者当成"被拒绝"，会让用户在端口写错或从站卡死时看到"四种布局都不
 *   支持"这种误导性结论。三态把"未决"单独保留，错误文本与退出码据此区分。
 *
 * @par 本文件为什么单独存在
 *   协商机制（`tcp_framing_negotiation.hpp`）不应为了一个枚举而把
 *   `XcpMaster` 这类重依赖拖进调用方；因此三态类型放在这个零依赖的小头里，
 *   协商与验证两侧都只包含它。
 */

#ifndef CALMCAR_XCP_TCP_FRAMING_VERDICT_HPP_
#define CALMCAR_XCP_TCP_FRAMING_VERDICT_HPP_

#include <cstdint>
#include <string>

namespace calmcar::xcp {

/**
 * @brief 单个候选的验证结论（机器可判定）
 */
enum class FramingVerdict : std::uint8_t {
    /// @brief 对端显式拒绝：协议层违规、解析失败或对端在交换中主动断开
    Rejected,

    /// @brief 未决：没有任何协议层违规证据（典型原因是纯超时）
    Inconclusive,

    /// @brief 该候选可用
    Accepted,
};

/**
 * @brief 将结论转为中文文本（用于日志与错误文本）
 * @param verdict 待渲染的结论
 * @return 静态字符串："被拒" / "未决" / "接受"
 */
[[nodiscard]] inline const char* FramingVerdictName(
    FramingVerdict verdict) noexcept {
    switch (verdict) {
        case FramingVerdict::Rejected:
            return "被拒";
        case FramingVerdict::Inconclusive:
            return "未决";
        case FramingVerdict::Accepted:
            return "接受";
        default:
            return "未知";
    }
}

/**
 * @brief 单次验证的返回值：结论 + 人类可读原因 + 耗时
 */
struct FramingVerificationResult {
    /// @brief 结论（默认按"被拒"处理，失败关闭）
    FramingVerdict verdict{FramingVerdict::Rejected};

    /// @brief 中文原因；`Accepted` 时为空
    std::string reason;

    /// @brief 本次验证耗时（毫秒），含建立连接与交换
    long long elapsed_ms{0};
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_TCP_FRAMING_VERDICT_HPP_