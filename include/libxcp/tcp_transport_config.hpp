/**
 * @file tcp_transport_config.hpp
 * @brief TCP Transport 配置参数。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 5 节实现（B-T2）。
 */

#ifndef CALMCAR_XCP_TCP_TRANSPORT_CONFIG_HPP_
#define CALMCAR_XCP_TCP_TRANSPORT_CONFIG_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_wire_profile.hpp"

namespace calmcar::xcp {

/// @brief TCP 连接角色：决定 Open() 是主动连接还是监听接受
enum class TcpRole : std::uint8_t {
    /// @brief Master 侧：主动 connect 到远端 Slave
    Master = 0,
    /// @brief Slave 侧：listen 并接受一个入站连接（测试/从机模拟用）
    Slave = 1,
};

/**
 * @brief TCP Transport 配置参数
 *
 * @details 与 `UdpTransportConfig` 的差异是**刻意的**，不要把两者做成同一份
 *          配置：TCP 是有连接协议，因此多出角色、连接/接受超时、KeepAlive、
 *          地址重用等概念；同时又**没有**"来源过滤"——连接一旦建立，对端就
 *          被内核唯一确定，再按地址过滤只会引入无意义的配置面。
 */
struct TcpTransportConfig {
    /// @brief 连接角色（决定 Open() 走 connect 还是 listen/accept）
    TcpRole role = TcpRole::Master;

    /// @brief 远端 Slave IPv4 地址（仅 Master 角色使用，如 "192.168.1.10"）
    std::string remote_host;

    /// @brief 远端 Slave TCP 端口（仅 Master 角色使用；0 为非法值）
    std::uint16_t remote_port = 0;

    /// @brief 本地绑定 IPv4 地址（Slave 角色必填；Master 角色可留空表示由内核选源）
    std::string local_host = "0.0.0.0";

    /// @brief 本地绑定端口（0 表示由 OS 分配临时端口）
    std::uint16_t local_port = 0;

    /**
     * @brief 建立连接的超时（毫秒）
     * @details 覆盖 DNS 之后的三次握手等待，以及 Slave 角色下等待 accept 的
     *          时长。必须大于 0。
     */
    std::uint32_t connect_timeout_ms = 3000;

    /**
     * @brief 单次 send 的**可写等待**超时（毫秒）
     * @details 对端停止读取时内核发送缓冲会填满，此时 send 会阻塞。本值给出
     *          明确上界，超时即报 TransportError（"发送超时"），而不是让调用
     *          线程无限期挂住。必须大于 0。
     */
    std::uint32_t send_timeout_ms = 3000;

    /**
     * @brief 是否启用 TCP KeepAlive
     * @details 默认关闭：XCP 自身有命令级超时，KeepAlive 主要用于探测**静默
     *          断开**（如中间设备丢失连接状态）。是否启用由调用方按链路特性
     *          决定，不由库擅自开启。
     */
    bool keep_alive = false;

    /**
     * @brief 是否设置 SO_REUSEADDR
     * @details Slave 角色监听端口时常用，避免上次连接处于 TIME_WAIT 导致
     *          "地址已在使用"。Master 角色默认关闭。
     */
    bool reuse_address = false;

    /**
     * @brief 复用 `UdpTransportConfig::packet_alignment` 的同名语义
     * @details `PacketAlignment::Unknown` 会让 `Open()` 直接失败（同 UDP）。
     */
    PacketAlignment packet_alignment = PacketAlignment::Bit8;

    /**
     * @brief TCP 上的 XCP 帧布局 Profile（B-T1 定义，见 `tcp_wire_profile.hpp`）
     * @details 默认 `TcpWireProfile::AsamV14`：与 XCPlite 默认 TCP 路径互通
     *          （见 `tcp_wire_profile.hpp` 的互操作性说明）。
     * @note `TcpWireProfile` 是 **enum class**，不存在 `v14LengthInDle()` 这类
     *       工厂函数——Profile 必须由调用方显式择一，本库明令禁止从版本号、
     *       A2L 对齐信息或首帧内容自动推断（计划 §1.2）。
     */
    TcpWireProfile wire_profile = TcpWireProfile::AsamV14;

    /// @brief 单个 XCP Frame 内 Packet 的长度上限（字节）
    ///        默认 65535，与 DLE 一个字节能表达的最大值一致
    std::size_t max_frame_packet_size = 65535;

    /**
     * @brief 单个 XCP Packet 的长度上限（字节）
     * @details TCP 是字节流，理论上不限制单包大小；但 XCP 的 CTO 语义上限为
     *          255（1 字节长度字段）或 65535（2 字节）。默认 65535 覆盖
     *          "一个 Packet 占满一个 Frame"的极限情形。
     */
    std::size_t max_packet_size = 65535;

    /**
     * @brief 单次 `recv` 的缓冲区大小（字节）
     * @details 只影响每次系统调用的拷贝量，不改变协议语义。默认 4096 与计划
     *          §5.7 一致。
     */
    std::size_t receive_chunk_size = 4096;

    /**
     * @brief 解码器的重同步上限（字节）
     * @details 收到无法解释的字节时，解码器最多向后搜索这么多字节来重新
     *          定位帧头；超过则判定为流不可恢复。默认 255（一个最大 DLE 帧
     *          头长度），避免在被污染的流上无限搜索。
     */
    std::size_t max_resync_bytes = 255;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_TCP_TRANSPORT_CONFIG_HPP_