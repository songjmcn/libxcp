/**
 * @file ixcp_transport.hpp
 * @brief 传输层抽象接口与包监听器。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 5 节实现。
 * 协议核心只依赖本接口，不依赖任何 Socket API 或具体 Transport 实现。
 */

#ifndef CALMCAR_XCP_IXCP_TRANSPORT_HPP_
#define CALMCAR_XCP_IXCP_TRANSPORT_HPP_

#include <cstdint>
#include <string>

#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief Transport 包监听器接口
 *
 * Transport 内部线程收到完整 XCP Packet 后调用此接口。回调直接在 Transport
 * 工作线程执行，实现者需自行保证线程安全（不得在回调内做阻塞式长操作）。
 */
class IPacketListener {
public:
    virtual ~IPacketListener() = default;

    /**
     * @brief 收到一个完整的 XCP Packet
     * @param packet 完整 XCP Packet 字节（不含 Transport Header）
     * @note 在 Transport 工作线程调用；packet 仅在本次回调期间有效，
     *       监听器若需异步保存必须自行复制字节。
     */
    virtual void OnPacketReceived(BytesView packet) = 0;

    /**
     * @brief Transport 通道已关闭（正常关闭或错误关闭）
     * @param reason 关闭原因描述
     * @note 在 Transport 工作线程调用；返回后不再有 OnPacketReceived 回调。
     */
    virtual void OnTransportClosed(std::string_view reason) = 0;

    /**
     * @brief Transport 发生可恢复错误（如畸形 Datagram 丢弃）
     * @param message 错误描述
     * @note 在 Transport 工作线程调用；Transport 不会因此关闭。
     */
    virtual void OnTransportWarning(std::string_view message) = 0;
};

/**
 * @brief XCP Transport 抽象接口
 *
 * 协议核心通过此接口收发 XCP Packet，不感知具体传输介质。实现者负责：
 * 打开/关闭通道、发送完整 CTO Packet、内部线程接收并通过 IPacketListener
 * 回调推送收到的 Packet。
 */
class IXcpTransport {
public:
    virtual ~IXcpTransport() = default;

    /**
     * @brief 打开 Transport 通道
     * @param listener 包监听器，生命周期须长于 Transport 使用期
     * @throws XcpException(TransportError) 打开失败
     * @note 打开后 Transport 内部线程开始接收并通过 listener 回调。
     */
    virtual void Open(IPacketListener& listener) = 0;

    /**
     * @brief 关闭 Transport 通道
     * @note 关闭后不再调用 listener 回调；可安全重复调用（幂等）。
     */
    virtual void Close() = 0;

    /**
     * @brief 发送一个完整的 XCP CTO Packet
     * @param packet 完整 XCP Packet 字节（不含 Transport Header）
     * @throws XcpException(TransportError) 未打开或发送失败
     * @note 可在任意线程调用；实现需保证线程安全。
     */
    virtual void Send(BytesView packet) = 0;

    /// @brief Transport 是否已打开
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_IXCP_TRANSPORT_HPP_
