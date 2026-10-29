/**
 * @file tcp_framing_verification.hpp
 * @brief 用库自身协议栈验证单个 TCP 帧布局候选（`--tcp-profile auto` 的判定侧）。
 *
 * 依据 code-plan/TCP_auto证据收敛与候选验证_设计.md 第 C2 节实现。
 *
 * @par 为什么验证必须落在协议层
 *   帧布局（Profile + PACKET_ALIGNMENT）只影响**字节如何切分与填充**，错配时的
 *   表现是"长度非法 / 响应解析失败 / 对端直接断开"，这些都是协议层语义。
 *   传输层只搬字节，无法判定；因此本组件构造一个全新的 `XcpMaster`，用真实
 *   握手来取证。
 *
 * @par 每次验证都用全新连接
 *   绝不在同一条连接上反复改布局：错配的候选可能已在流中留下半个包，继续
 *   复用会把上一次的残留字节解释成下一次的帧头。验证结束后连接**必须**被
 *   彻底关闭，正式会话随后另建连接（见设计文档 C1）。
 *
 * @par 不在这里做候选排序
 *   排序属于 `tcp_framing_candidates.hpp`，验证只回答"这一个候选行不行"。
 */

#ifndef CALMCAR_XCP_TCP_FRAMING_VERIFICATION_HPP_
#define CALMCAR_XCP_TCP_FRAMING_VERIFICATION_HPP_

#include <cstdint>
#include <string>

#include "libxcp/tcp_framing_negotiation.hpp"
#include "libxcp/tcp_framing_verdict.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief 验证候选所需的固定连接参数（与布局无关的部分）
 */
struct TcpFramingProbeConfig {
    /// @brief 对端 IPv4 地址（如 "127.0.0.1"）
    std::string host;

    /// @brief 对端 TCP 端口（0 为非法值）
    std::uint16_t port{0};

    /// @brief 建立 TCP 连接的超时（毫秒），必须大于 0
    std::uint32_t connect_timeout_ms{3000};

    /**
     * @brief 单条 XCP 命令的响应超时（毫秒），必须大于 0
     * @details 验证阶段故意取较短值：错配候选通常"永不回包"，长超时会让
     *          auto 的四个候选累加出分钟级等待。
     */
    std::uint32_t command_timeout_ms{1000};
};

/**
 * @brief 把库内错误分类映射为三态结论（纯函数，便于单元测试）
 *
 * @param category 捕获到的 `XcpException` 分类
 * @param connection_established 抛错时是否已建立 XCP 会话（即 CONNECT 已成功）
 * @return `FramingVerdict::Rejected` / `Inconclusive`
 *
 * @details 判定规则（与设计文档 C2 一致）：
 *          - `Timeout`：对端没回话 → **未决**（无协议层违规证据）；
 *          - `TransportError` 且**连接未建立**：连接阶段失败（不可达 / 对端
 *            复位）→ **未决**，此时无法区分"不支持该布局"与"根本没在听"；
 *          - `TransportError` 且**连接已建立**：对端在交换中主动断开 →
 *            被拒（这是明确的拒绝证据，XCPlite V1.2 在长度非法时正是如此）；
 *          - `OperationOutcomeUnknown`：命令可能已生效但响应丢失，等同于
 *            超时 → **未决**；
 *          - 其余（`MalformedPacket` / `ProtocolError` / `InvalidState` /
 *            `InvalidArgument` / `RecoveryFailed` / `UnsupportedFeature` /
 *            `Cancelled`）：均为明确的"这一候选不可用"→ 被拒。
 * @note `Accepted` 不会从本函数返回。
 */
[[nodiscard]] FramingVerdict ClassifyFramingFailure(
    ErrorCategory category, bool connection_established) noexcept;

/**
 * @brief 用全新连接验证单个候选是否可用
 *
 * @param config 连接参数（地址、端口、超时）
 * @param candidate 待验证候选（Profile 与对齐都必须已显式选定）
 * @return 三态结论 + 中文原因 + 耗时；函数本身**不抛异常**
 *
 * @details 验证序列：`TcpTransport.Open` → `XcpMaster::Connect`（库内已含
 *          CONNECT + GET_COMM_MODE_INFO + GET_STATUS）→ 再补一次
 *          `QueryStatus()`（确认连续两次往返稳定）→ `Disconnect`。
 *          无论成功失败，返回前都关闭连接与 transport。
 * @note 候选非法（`Unknown`）时直接返回 `Rejected` 并说明是配置错误，不建连接。
 */
[[nodiscard]] FramingVerificationResult VerifyTcpFramingCandidate(
    const TcpFramingProbeConfig& config,
    const TcpFramingCandidate& candidate);

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_TCP_FRAMING_VERIFICATION_HPP_