/**
 * @file tcp_framing_verification.cpp
 * @brief 单候选验证与失败分类的实现（设计文档 C2）。
 */

#include "libxcp/tcp_framing_verification.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "libxcp/tcp_transport.hpp"
#include "libxcp/xcp_master.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 判断候选是否具备进入真实连接的资格
[[nodiscard]] bool IsProbeable(const TcpFramingCandidate& candidate) {
    return IsTcpWireProfileResolved(candidate.profile) &&
           candidate.alignment != PacketAlignment::Unknown;
}

/// @brief 构造一次失败结果（统一填充耗时之外的字段）
[[nodiscard]] FramingVerificationResult MakeFailure(FramingVerdict verdict,
                                                    std::string reason) {
    FramingVerificationResult result;
    result.verdict = verdict;
    result.reason = std::move(reason);
    return result;
}

}  // namespace

FramingVerdict ClassifyFramingFailure(ErrorCategory category,
                                     bool connection_established) noexcept {
    switch (category) {
        case ErrorCategory::Timeout:
            // 对端没回话：既不能证明支持，也不能证明不支持
            return FramingVerdict::Inconclusive;
        case ErrorCategory::TransportError:
            // 连接已建立后对端主动断开是明确的拒绝证据；连接阶段失败则无法
            // 区分"不支持该布局"与"对端根本没在监听"。
            return connection_established ? FramingVerdict::Rejected
                                          : FramingVerdict::Inconclusive;
        case ErrorCategory::OperationOutcomeUnknown:
            // 命令可能已生效但响应丢失，证据强度等同于超时
            return FramingVerdict::Inconclusive;
        case ErrorCategory::MalformedPacket:
        case ErrorCategory::ProtocolError:
        case ErrorCategory::InvalidState:
        case ErrorCategory::InvalidArgument:
        case ErrorCategory::RecoveryFailed:
        case ErrorCategory::UnsupportedFeature:
        case ErrorCategory::Cancelled:
        default:
            // 协议层违规 / 本地配置错误：都属于"这一候选不可用"的明确结论
            return FramingVerdict::Rejected;
    }
}

FramingVerificationResult VerifyTcpFramingCandidate(
    const TcpFramingProbeConfig& config, const TcpFramingCandidate& candidate) {
    const auto started = std::chrono::steady_clock::now();
    const auto finish = [&started](FramingVerificationResult result) {
        const auto finished = std::chrono::steady_clock::now();
        result.elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(finished -
                                                                  started)
                .count();
        return result;
    };

    if (!IsProbeable(candidate)) {
        return finish(MakeFailure(FramingVerdict::Rejected,
                                  "候选非法：Profile 或 PACKET_ALIGNMENT 未指定"));
    }
    if (config.host.empty() || config.port == 0) {
        return finish(MakeFailure(FramingVerdict::Rejected,
                                  "验证参数非法：地址为空或端口为 0"));
    }
    if (config.connect_timeout_ms == 0 || config.command_timeout_ms == 0) {
        return finish(MakeFailure(FramingVerdict::Rejected,
                                  "验证参数非法：超时必须大于 0"));
    }

    TcpTransportConfig transport_config;
    transport_config.role = TcpRole::Master;
    transport_config.remote_host = config.host;
    transport_config.remote_port = config.port;
    transport_config.connect_timeout_ms = config.connect_timeout_ms;
    transport_config.packet_alignment = candidate.alignment;
    transport_config.wire_profile = candidate.profile;

    XcpMasterOptions options;
    options.timeouts.command_timeout =
        std::chrono::milliseconds(config.command_timeout_ms);
    options.timeouts.synch_timeout =
        std::chrono::milliseconds(config.command_timeout_ms);
    // 验证阶段不做恢复重试：错配候选本就不回包，重试只会成倍拉长等待
    options.timeouts.max_retries = 0;
    // 不探测 GET_VERSION：验证只关心"这一布局能否完成基本往返"，多一次
    // 往返既拖慢整轮 auto，也会把版本探测的不确定性混进布局结论。
    options.autodetect_version = false;

    auto master = std::make_unique<XcpMaster>(
        std::make_unique<TcpTransport>(transport_config), options);

    bool connection_established = false;
    try {
        master->Connect();
        connection_established = true;
        // 再补一次状态查询：只成功一次可能是缓存或巧合，连续两次往返才说明
        // 该布局下长度字段与对齐都被对端稳定接受。
        (void)master->QueryStatus();
    } catch (const XcpException& error) {
        const FramingVerdict verdict =
            ClassifyFramingFailure(error.Category(), connection_established);
        return finish(MakeFailure(verdict, error.what()));
    } catch (const std::exception& error) {
        return finish(
            MakeFailure(FramingVerdict::Rejected,
                        std::string("验证过程中抛出非 XCP 异常：") + error.what()));
    } catch (...) {
        return finish(MakeFailure(FramingVerdict::Rejected,
                                  "验证过程中抛出未知异常"));
    }

    // 验证完成即彻底断开：正式会话随后另建连接，绝不复用本次试连。
    // 断开失败不改变结论——两次往返已证明该候选可用；连接由析构兜底关闭。
    try {
        master->Disconnect();
    } catch (const std::exception&) {
        // 故意吞掉：结论以"能否完成往返"为准，断开异常不构成布局证据
    }

    FramingVerificationResult accepted;
    accepted.verdict = FramingVerdict::Accepted;
    return finish(std::move(accepted));
}

}  // namespace calmcar::xcp