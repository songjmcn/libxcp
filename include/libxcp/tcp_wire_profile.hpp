/**
 * @file tcp_wire_profile.hpp
 * @brief TCP 承载的 XCP 线路 **Wire Profile** 枚举与相关查询。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 1.2 节实现（B-T1 §4.2）。
 *
 * @par 为什么需要 Profile 而不是"自动识别"
 *   XCP on Ethernet 的历史版本在线路布局上**不兼容**：
 *   - AsamV15（TL V1.5）：LEN 是真实 XCP Packet 长度，PACKET_ALIGNMENT 的
 *     FILL 补在 LEN **之外**；
 *   - AsamV14（XCPlite 参考实现）：每帧严格 `[LEN][CTR][LEN bytes]`，不存在
 *     LEN 外 Tail。
 *
 *   在 `PACKET_ALIGNMENT_8` 下两种布局**数值上恰好等价**，这使人容易误以为
 *   可以"自动探测"。本库**明令禁止**任何自动探测：按首帧内容猜测、以超时
 *   窥探 Tail、按填充值（如"尾部是 0 就当 Tail"）识别，都会在边界条件下
 *   静默吞掉下一帧的 Header，产生极难定位的失步。因此 Profile 必须是
 *   **显式、连接级不可变**的配置。
 *
 * @par 禁止事项（计划 §1.2）
 *   - 禁止让 `Unknown` 进入 decoder；
 *   - 禁止用 PL/TL 版本号、A2L 的 `PACKET_ALIGNMENT` 或首帧内容单独推断
 *     Profile。这些信息只允许进入**诊断**（日志/错误文本），不得替调用方
 *     完成选择。
 */

#ifndef CALMCAR_XCP_TCP_WIRE_PROFILE_HPP_
#define CALMCAR_XCP_TCP_WIRE_PROFILE_HPP_

#include <cstdint>
#include <string_view>

namespace calmcar::xcp {

/**
 * @brief XCP on TCP 的线路布局 Profile（**必须显式选择**）
 *
 * @details 本枚举是连接级不可变属性：`Open()` 成功后不得修改。`Unknown`
 *          表示调用方尚未完成配置，此时 `Open()` 必须返回 `InvalidArgument`
 *          （fail closed），不得回退到任何默认值。
 */
enum class TcpWireProfile : std::uint8_t {
    /// @brief 未配置：`Open()` 必须拒绝，绝不自动选择
    Unknown = 0,

    /// @brief ASAM Ethernet Transport Layer V1.5 语义
    /// @details LEN 为真实 XCP Packet 长度（不含 Header 与 Tail）；
    ///          PACKET_ALIGNMENT 的 FILL 位于 LEN **之外**，用于让后继
    ///          XCP Packet 的起始偏移满足对齐。
    AsamV15,

    /// @brief ASAM V1.4 兼容语义（以 `thirdparty/XCPliteV1.2` 为互操作基线）
    /// @details 每帧严格为 `[LEN][CTR][LEN bytes]`；**不读取也不发送**
    ///          LEN 外 Tail。对端可能把 DTO/异步 CRM 的 message 总长按 4
    ///          字节扩长并**计入 LEN**——那属于 LEN 内的合法数据，不得与
    ///          AsamV15 的 LEN 外 Tail 混为一谈。
    AsamV14,
};

/**
 * @brief 判断 Profile 是否已显式选定（即非 `Unknown`）
 * @param profile 待检查的 Profile
 * @return true 表示可作为 decoder/transport 的有效配置
 */
[[nodiscard]] constexpr bool IsTcpWireProfileResolved(
    TcpWireProfile profile) noexcept {
    return profile != TcpWireProfile::Unknown;
}

/**
 * @brief 渲染 Profile 名称（日志与诊断用）
 * @param profile 待渲染的 Profile
 * @return `"Unknown"` / `"AsamV15"` / `"AsamV14"` 的静态字符串
 *
 * @note 本函数只用于**诊断输出**：把 Profile 名称写进日志以便审计，绝不
 *       用于反向解析 Profile。
 */
[[nodiscard]] constexpr std::string_view TcpWireProfileName(
    TcpWireProfile profile) noexcept {
    switch (profile) {
        case TcpWireProfile::AsamV15:
            return "AsamV15";
        case TcpWireProfile::AsamV14:
            return "AsamV14";
        case TcpWireProfile::Unknown:
        default:
            return "Unknown";
    }
}

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_TCP_WIRE_PROFILE_HPP_