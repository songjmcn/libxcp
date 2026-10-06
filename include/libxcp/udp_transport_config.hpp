/**
 * @file udp_transport_config.hpp
 * @brief UDP Transport 配置参数。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 7 节实现。
 */

#ifndef CALMCAR_XCP_UDP_TRANSPORT_CONFIG_HPP_
#define CALMCAR_XCP_UDP_TRANSPORT_CONFIG_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

#include "libxcp/protocol_types.hpp"
#include "libxcp/udp_header_codec.hpp"

namespace calmcar::xcp {

/**
 * @brief UDP Transport 配置参数
 *
 * 端点全部由配置提供，不硬编码示例端口。长度上限默认取 IPv4 UDP 理论上限，
 * 调用方可按路径 MTU 设置更小值以避免 IP 分片。
 */
struct UdpTransportConfig {
    /// @brief 远端 Slave IPv4 地址（如 "192.168.1.10" 或 "127.0.0.1"）
    std::string remote_host;

    /// @brief 远端 Slave UDP 业务端口（0 为非法值，Open() 时校验）
    std::uint16_t remote_port = 0;

    /// @brief 本地绑定 IPv4 地址（默认 "0.0.0.0"，Loopback 测试用 "127.0.0.1"）
    std::string local_host = "0.0.0.0";

    /// @brief 本地绑定端口（0 表示由 OS 分配临时端口）
    std::uint16_t local_port = 0;

    /**
     * @brief 接收轮询间隔（毫秒），用于内部线程周期性检查关闭标记
     * @details 实际以 Socket 接收超时实现，不影响上层命令超时；越小关闭越及时。
     */
    std::uint32_t receive_poll_interval_ms = 100;

    /// @brief 最大允许的单个 XCP Frame 内原始 Packet 长度（字节）
    ///        默认 kUdpMaxXcpPacket (65503)；可设更小值以避免 IP 分片
    std::size_t max_frame_packet_size = kUdpMaxXcpPacket;

    /// @brief 最大允许的 UDP Datagram Payload 长度（字节）
    ///        默认 kUdpMaxDatagramSize (65507)，限制连续打包 Frame 的总长度
    std::size_t max_datagram_size = kUdpMaxDatagramSize;

    /**
     * @brief 是否严格匹配远端端口（true 时要求收到的包来自 remote_port）
     * @details 这是 Master 侧的项目安全策略（设计决策 D3），不是 XCP 1.1 Part 3
     *          对 Slave 连接绑定规则的复刻；设为 false 时仅匹配 remote_host。
     */
    bool strict_remote_port = true;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_UDP_TRANSPORT_CONFIG_HPP_
