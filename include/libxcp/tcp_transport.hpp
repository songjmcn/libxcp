/**
 * @file tcp_transport.hpp
 * @brief 标准 XCP on TCP/IP Transport 实现（跨平台 IPv4 Socket + 字节流解帧）。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 5 节实现（B-T2）。
 *
 * @par 与 UDP 传输的本质差异（决定了本文件的全部设计）
 *   1. **有连接**：`Open()` 要么 connect 远端（Master），要么 listen/accept
 *      （Slave）。连接建立之前没有"对端"概念，因此没有来源过滤配置；
 *      连接断开后本 Transport 直接进入关闭状态，**不自动重连**。
 *   2. **字节流**：`recv()` 边界与 Frame 边界无关，必须用
 *      `TcpStreamDecoder` 跨调用积累状态（UDP 那套"整体拒绝"策略会把
 *      正常分片误判为畸形）。
 *   3. **发送可能短写**：`send()` 返回的字节数可能小于请求值，必须循环发送；
 *      对端停止读取时内核缓冲填满，必须用可写等待 + 超时给出明确上界，而不是
 *      无限期阻塞调用线程。
 *
 * @par 不自动重连（刻意的设计决策）
 *   TCP 断链后本 Transport 不会静默重建连接。理由：XCP 的会话状态
 *   （CONNECT/RESOURCE/DAQ 分配/SEED&KEY）都绑定在一条连接上，传输层擅自
 *   重连会让上层的会话视图与真实连接错位——上层以为自己在和已 CONNECT 的
 *   Slave 通信，实际却是一条全新的、未 CONNECT 的连接。因此断链只上报
 *   `OnTransportClosed()`，重连与重新握手由上层显式决策。
 */

#ifndef CALMCAR_XCP_TCP_TRANSPORT_HPP_
#define CALMCAR_XCP_TCP_TRANSPORT_HPP_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "libxcp/ixcp_transport.hpp"
#include "libxcp/tcp_stream_decoder.hpp"
#include "libxcp/tcp_transport_config.hpp"
#include "libxcp/tcp_wire_profile.hpp"

namespace calmcar::xcp {

/**
 * @brief 标准 XCP on TCP/IP Transport 实现
 *
 * @par 线程模型
 *   - **接收线程**：`Open()` 成功后启动，独占 `m_decoder_` 与接收缓冲，直到
 *     `Close()` join 完毕。收到完整 Frame 后回调 `OnPacketReceived()`。
 *   - **发送**：可在任意线程调用，由 `m_send_mutex_` 串行化；发送 CTR 在同一
 *     把锁下递增，保证 CTR 分配顺序与实际写入顺序严格一致（否则对端会看到
 *     乱序 CTR）。
 *   - **Close()**：可被任意线程调用（含从监听器回调内调用）。通过
 *     `shutdown()` 打断阻塞中的 `recv()`，再 join 接收线程。**不**在 join 前
 *     关闭句柄——句柄号会被 OS 立即回收，可能分配给本进程新建的 Socket，
 *     而接收线程此刻仍可能在使用它。
 *
 * @par 关闭语义（与 UDP 一致）
 *   `Close()` 幂等。接收线程退出前统一交付一次 `OnTransportClosed()`，
 *   此后不再有任何回调。
 *
 * @par 接收 CTR 策略（计划 §1.5）
 *   共同规则：首个合法 Frame 建立基线且**允许任意首值**；`0xFFFF -> 0`
 *   是正常回绕；CRM/DTO/EV/SERV 共用同一条接收 CTR 序列，不按包类型分开
 *   维护。每次 `Open()` 清空接收基线。
 *
 *   Profile 差异（**这是 TCP 与 UDP 的关键分歧点**，§1.5 末条明确禁止直接
 *   复用 UDP 的半域容错策略）：
 *   - **AsamV15**：严格 `current == previous + 1 mod 65536`。duplicate /
 *     jump / backward 一律发 warning 后**关闭连接**；
 *   - **AsamV14**：duplicate / backward 视为协议异常并**关闭连接**；
 *     forward gap 只记录丢帧诊断并**继续**——因为 XCPlite 的发送队列丢弃
 *     会主动推进全局 CTR，缺号属对端已知行为而非我方失步。
 *
 *   TCP 无消息边界，CTR 是发现"对端漏发 / 我方漏读"的唯一手段，故校验
 *   失败一律断链，不尝试在流内重新同步。
 */
class TcpTransport : public IXcpTransport {
public:
    /// @brief 构造 TCP Transport
    /// @param config 角色、端点、超时、Profile 与对齐配置
    /// @note 此处仅保存配置；合法性一律在 `Open()` 校验（与 `UdpTransport`
    ///       保持一致，使"构造不抛异常、失败点集中在 Open"成为可依赖的约定）
    explicit TcpTransport(TcpTransportConfig config);

    /// @brief 析构；若仍处于打开状态则先执行 `Close()`，保证接收线程被 join
    ~TcpTransport() override;

    // 禁止拷贝：持有 Socket 与线程
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    /**
     * @brief 打开通道：校验配置 → 创建 Socket → connect（Master）或
     *        listen+accept（Slave）→ 启动接收线程
     * @param listener 包监听器，生命周期须长于本 Transport 使用期
     * @throws XcpException(InvalidArgument) 配置非法（端口为 0、Peer 地址为空、
     *         尺寸越界、`wire_profile == Unknown`、`packet_alignment == Unknown`）
     * @throws XcpException(UnsupportedFeature) 对端地址是 IPv6 字面量
     * @throws XcpException(TransportError) Socket 创建/绑定/监听/连接失败或超时
     * @note 幂等：内部先调用 `Close()`，允许在同一实例上重新打开
     */
    void Open(IPacketListener& listener) override;

    /**
     * @brief 关闭通道：通知接收线程退出、打断阻塞 `recv`、join，并关闭连接
     * @note 可安全重复调用；可从监听器回调内调用
     */
    void Close() override;

    /**
     * @brief 发送一个完整 XCP CTO Packet
     * @param packet 完整 XCP Packet 字节（不含 Transport Header）
     * @throws XcpException(InvalidState) 未打开时调用
     * @throws XcpException(InvalidArgument) `packet` 为空或超过配置上限
     * @throws XcpException(TransportError) 发送失败、对端已断、或可写等待超时
     * @note 线程安全；短写由内部循环补齐，调用方无需重试
     */
    void Send(BytesView packet) override;

    /// @brief Transport 是否已打开
    [[nodiscard]] bool IsOpen() const noexcept override;

    /// @brief 当前发送计数器值，即下一个待分配的 Frame CTR
    /// @details `Open()` 后为 0；每发出一个 Frame 递增一次
    [[nodiscard]] DatagramCtr SendCtr() const noexcept;

    /// @brief 实际生效的本地端口（`local_port == 0` 时由 OS 分配）
    /// @return 尚未打开或取回失败时返回 0
    [[nodiscard]] std::uint16_t LocalPort() const noexcept;

    /// @brief 累计已投递给监听器的 Frame 数（诊断与测试用）
    /// @note 只统计**成功回调**的帧；被丢弃的畸形帧不计入
    [[nodiscard]] std::uint64_t DeliveredFrameCount() const noexcept;

private:
    /// @brief 接收线程主体：循环 `recv` → 投喂解码器 → 交付 Frame
    /// @details 退出前回调 `OnTransportClosed()`；解码器报告不可恢复畸形时
    ///          以"流失步"原因退出并**不**尝试重同步
    void ReceiveLoop();

    /// @brief 建立连接：按角色走 connect 或 listen+accept
    /// @throws XcpException(TransportError) 失败或超时
    void EstablishConnection();

    /// @brief 以非阻塞方式连接并等待结果（含超时）
    /// @param addr 目标地址（**不透明指针**，由 `EstablishConnection()` 在
    ///             实现文件内构造的平台地址结构；此处的类型擦除是刻意的）
    /// @param addr_len 地址结构字节数
    /// @throws XcpException(TransportError) 失败或超时
    /// @note 参数类型为 `const void*` 而非 `const sockaddr*`：本头文件属于
    ///       **公开接口**，不得把 `sockaddr`/`socklen_t` 这些平台 Socket 类型
    ///       泄漏进 ABI（与 `udp_transport.hpp` 的平台无关风格保持一致）。
    ///       平台差异全部收敛在 `src/tcp_transport.cpp` 与 `src/socket_util.hpp`。
    void ConnectWithTimeout(const void* addr, std::size_t addr_len);

    /// @brief 监听并接受一个入站连接（含超时）
    /// @throws XcpException(TransportError) 失败或超时
    void ListenAndAccept();

    /// @brief 把 `packet` 编码为一个 Frame 并**完整**写出（处理短写）
    /// @param packet 原始 XCP Packet
    /// @param ctr 本帧使用的 CTR
    /// @throws XcpException(TransportError) 写失败或可写等待超时
    void WriteFrame(BytesView packet, DatagramCtr ctr);

    /// @brief 把一段字节完整写出，循环处理短写、EINTR 与可写等待超时
    /// @param data 待写字节
    /// @param size 字节数
    /// @throws XcpException(TransportError) 写失败或超时
    void WriteAll(const std::uint8_t* data, std::size_t size);

    /// @brief 处理一次解码结果：投递 Frame、上报畸形
    /// @param result 解码结果
    /// @return false 表示应终止接收循环（不可恢复畸形）
    bool DispatchDecodeResult(const TcpDecodeResult& result);

    /**
     * @brief 按当前 Wire Profile 校验一个 Frame 的接收 CTR
     * @param ctr 本 Frame 的 CTR
     * @param out_reason 失败时写入诊断文本（供 `OnTransportClosed` 说明断链原因）
     * @return true 表示接受该 Frame；false 表示 CTR 违约，必须断链
     * @details 策略见类文档"接收 CTR 策略"（计划 §1.5）：
     *          - 基线未建立：接受并记基线（首值任意）；
     *          - `AsamV15`：仅接受 `previous + 1 mod 65536`，任何偏离即失败；
     *          - `AsamV14`：接受前向缺号（距离 1..0x7FFF）并写回基线；
     *            后向/重复（距离 0x8000..0xFFFF）视为协议异常即失败。
     * @note 仅在接收线程内调用，读写 `m_last_recv_ctr_` 无需加锁。
     */
    bool CheckReceiveCtr(DatagramCtr ctr, std::string* out_reason);

    struct SocketImpl;

    /// @brief 端点与行为配置（`Open()` 时校验；`Open()` 可能回填 `local_port`）
    TcpTransportConfig m_config_;
    /// @brief 收包监听器（非拥有）；`Open()` 写入、`Close()` 置空
    IPacketListener* m_listener_{nullptr};
    /// @brief 平台 Socket 实现（pimpl，隔离 Windows/POSIX 差异）
    std::unique_ptr<SocketImpl> m_socket_;
    /// @brief 流式解码器（仅在接收线程内使用，生命周期与打开期一致）
    std::unique_ptr<TcpStreamDecoder> m_decoder_;
    /// @brief 接收线程；仅在打开期间 joinable
    std::thread m_receive_thread_;
    /// @brief 接收线程运行标记；`Close()` 置 false 并通过 shutdown 立即打断
    std::atomic<bool> m_running_{false};
    /// @brief 保护发送路径与发送 CTR（保证 CTR 顺序 == 写入顺序）
    mutable std::mutex m_send_mutex_;
    /// @brief 下一个待分配的 Frame CTR
    std::atomic<DatagramCtr> m_send_ctr_{0};
    /// @brief 实际绑定的本地端口（`local_port == 0` 时由 OS 分配后回填）
    std::atomic<std::uint16_t> m_bound_local_port_{0};
    /// @brief 累计已投递 Frame 数
    std::atomic<std::uint64_t> m_delivered_frames_{0};

    /**
     * @brief 最近一次已接受的接收 CTR（基线）
     * @details 首个合法 Frame 建立基线且**允许任意首值**（计划 §1.5：
     *          "接收首个合法 Frame 建立基线，允许首值任意"；AsamV14 peer 的
     *          CTR 在 TCP 断连/重连后可能继续增长，故不要求为 0）。
     *          仅在接收线程内读写，无需额外互斥。
     */
    std::optional<DatagramCtr> m_last_recv_ctr_;

    /**
     * @brief 接收 CTR 基线是否已建立
     * @details 每次 `Open()` 复位为 `false`（计划 §1.5 共同规则：
     *          "本库每次成功 Open 后发送 CTR 从 0 开始"；接收侧同步清空
     *          基线，避免用旧连接的最后 CTR 去校验新连接的首帧）。
     */
    bool m_recv_baseline_established_{false};
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_TCP_TRANSPORT_HPP_