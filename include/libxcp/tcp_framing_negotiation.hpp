/**
 * @file tcp_framing_negotiation.hpp
 * @brief TCP 帧布局的**连接级候选试连**机制（`--tcp-profile auto`）。
 *
 * 依据 code-plan/传输帧布局自动协商_实施计划.md 第 6 节实现（P3）。
 *
 * @par 为什么可以自动，而"嗅探"仍然禁止
 *   `tcp_wire_profile.hpp` 禁止的是**同一条连接内**按字节内容猜测并中途切换
 *   Profile：那会静默吞掉下一帧 Header。本文件提供的是另一种形式——每个候选
 *   用**全新连接**试一次，失败即断开，绝不污染已有流；候选数量与顺序固定，
 *   且必须由调用方显式开启。
 *
 * @par 为什么判定谓词要由调用方注入
 *   transport 只搬字节，无法判断"这段字节是不是合法 CONNECT 响应"。协议语义
 *   属于上层（`XcpMaster`），因此本机制只负责**顺序、预算、记录**，真正的
 *   判定由调用方以 `FramingAttemptFn` 注入——通常是"构造 Master 并 Connect()"。
 */

#ifndef CALMCAR_XCP_TCP_FRAMING_NEGOTIATION_HPP_
#define CALMCAR_XCP_TCP_FRAMING_NEGOTIATION_HPP_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_wire_profile.hpp"

namespace calmcar::xcp {

/**
 * @brief 一个候选帧布局：Profile + PACKET_ALIGNMENT 的组合
 */
struct TcpFramingCandidate {
    /// @brief 线路布局 Profile（不得为 `Unknown`）
    TcpWireProfile profile = TcpWireProfile::AsamV14;

    /// @brief 对齐位宽（不得为 `Unknown`）
    PacketAlignment alignment = PacketAlignment::Bit8;
};

/**
 * @brief 默认候选序列（顺序固定，**不得**在运行时重排）
 *
 * @details 顺序为 `{AsamV14, 8bit}` → `{AsamV15, 8bit}` →
 *          `{AsamV15, 16bit}` → `{AsamV15, 32bit}`。
 *
 *          8 bit 下两种 Profile 的字节布局完全相同，因此 `{AsamV14, 8bit}`
 *          排在首位意味着"8 bit 对端一律按 V1.4 语义处理"：收发字节一致，
 *          差别只在 CTR 严格性（V1.4 更宽松）。需要严格 CTR 的调用方应显式
 *          指定 `AsamV15`。
 *
 * @return 固定 4 个候选
 */
[[nodiscard]] std::vector<TcpFramingCandidate> DefaultFramingCandidates();

/**
 * @brief 渲染候选为 `"AsamV14/8bit"` 这样的诊断文本
 * @param candidate 待渲染的候选
 * @return 静态拼接出的可读名称（日志用）
 */
[[nodiscard]] std::string FramingCandidateName(
    const TcpFramingCandidate& candidate);

/**
 * @brief 单次候选尝试的完整结果（供日志与错误文本使用）
 */
struct FramingAttemptOutcome {
    /// @brief 本次尝试的候选
    TcpFramingCandidate candidate{};

    /// @brief 是否被接受
    bool accepted{false};

    /// @brief 被拒原因；被接受时为空
    std::string failure_reason;

    /// @brief 本次尝试耗时（毫秒），含建立连接与判定
    long long elapsed_ms{0};
};

/**
 * @brief 判定谓词：对给定候选尝试一次真实连接
 *
 * @param candidate 待尝试的候选
 * @param out_reason 失败时写入中文原因（可为 nullptr）
 * @return true 表示该候选可用
 *
 * @note 实现方必须在返回 false 前**彻底断开**（关闭 socket），否则下一次
 *       尝试会带着上一条连接的残留状态运行。
 */
using FramingAttemptFn = std::function<bool(
    const TcpFramingCandidate& candidate, std::string* out_reason)>;

/**
 * @brief 协商结果
 */
struct TcpFramingNegotiationResult {
    /// @brief 胜出候选；全部失败时为空
    std::optional<TcpFramingCandidate> selected;

    /// @brief 按尝试顺序记录的全部结果（含胜出那次）
    std::vector<FramingAttemptOutcome> attempts;

    /**
     * @brief 生成中文摘要（供日志与错误文本）
     * @details 成功时形如
     *          `TCP 帧布局协商成功：AsamV14/8bit（第 1 次尝试，共 4 个候选）`；
     *          失败时逐行列出每个候选的失败原因。
     */
    [[nodiscard]] std::string Summary() const;
};

/**
 * @brief 依序尝试候选，首个被接受者胜出
 *
 * @param candidates 候选序列（通常来自 `DefaultFramingCandidates()`）
 * @param attempt 判定谓词（不得为空）
 * @return 协商结果（`attempts` 覆盖所有已尝试的候选）
 *
 * @details 语义约定：
 *          - **不做重试**：每个候选只尝试一次，失败即进入下一个；
 *          - **不重排**：严格按 `candidates` 给出的顺序；
 *          - 非法候选（`Unknown` Profile / `Unknown` 对齐）不调用谓词，直接
 *            记为拒绝并说明原因，避免把配置错误伪装成"对端不支持"；
 *          - 谓词抛出的异常被视为该候选失败，异常文本进入 `failure_reason`，
 *            异常**不会**外泄给调用方。
 */
[[nodiscard]] TcpFramingNegotiationResult NegotiateTcpFraming(
    const std::vector<TcpFramingCandidate>& candidates,
    const FramingAttemptFn& attempt);

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_TCP_FRAMING_NEGOTIATION_HPP_