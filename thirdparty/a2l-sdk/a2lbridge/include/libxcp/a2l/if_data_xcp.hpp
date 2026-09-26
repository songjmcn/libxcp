// =============================================================================
// if_data_xcp.hpp —— IF_DATA XCP 提取结果（设计文档 §4.3 / §6.3-A）
//
// 领域类型独立定义；SDK IfDataXcpDto/DaqListDto → 本处结构的映射在
// src/if_data_xcp_impl.cpp 完成（R1）。
// =============================================================================

#ifndef LIBXCP_A2L_IF_DATA_XCP_HPP_
#define LIBXCP_A2L_IF_DATA_XCP_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libxcp/a2l/daq_layout.hpp"
#include "libxcp/a2l/a2l_types.hpp"

namespace calmcar::xcp::a2l {

/**
 * @brief 一种 transport 实例的连接参数
 * @details 当前 SDK 快照只填 UDP 的 host/port，其余传输层保留 Kind，
 *          字段留待后续里程碑（§4.3）。
 */
struct TransportEndpoint {
    /// @brief 传输层类别
    enum class Kind : std::uint8_t { UdpIp, TcpIp, Can, Sxi, Usb, Flx };

    Kind kind = Kind::UdpIp;            ///< IF_DATA XCP 段声明的传输层
    std::uint16_t version = 0x0100;     ///< IF_DATA XCP 段版本号
    std::string remote_host;            ///< IPv4/IPv6 字符串；CAN 时为通道名
    std::uint16_t remote_port = 0;      ///< 远端端口
    std::string local_host;             ///< A2L 描述时填入，否则由调用方决定
    std::uint16_t local_port = 0;       ///< 本地端口
    std::uint8_t packet_alignment = 1;  ///< 8/16/32 bit（首版恒 1，未建模）
    std::vector<std::uint8_t>
        sub_commands;  ///< GET_SLAVE_ID 等子命令（首版未建模）
};

/**
 * @brief Protocol Layer 级参数
 * @details 等价于现有 CommandTimeouts + SessionParameters 的 A2L 侧来源。
 *          t1..t7 是 Master 超时配置，不参与运行时比对（B-16）。
 */
struct ProtocolLayerInfo {
    std::uint16_t version =
        0x0100;  ///< PROTOCOL_LAYER VERSION（major<<8|minor）
    std::uint16_t timeout_ms[7] =
        {};                     ///< t1..t7，索引 0 = t1（A2L 未给出时为 0）
    std::uint8_t max_cto = 0;   ///< MAX_CTO
    std::uint16_t max_dto = 0;  ///< MAX_DTO
    ByteOrder byte_order = ByteOrder::MsbLast;  ///< BYTE_ORDER
    AddressGranularity address_granularity =
        AddressGranularity::Byte;                 ///< ADDRESS_GRANULARITY
    std::vector<std::uint8_t> optional_commands;  ///< A2L 声明支持的命令码
    std::string seed_and_key_function;  ///< 外部函数文件名（不含路径）
    bool has_ecu_states = false;        ///< 是否声明 ECUSTATE
};

/**
 * @brief MODULE 级 IF_DATA XCP 汇总结果
 */
struct IfDataXcpInfo {
    bool ok = false;             ///< 是否成功提取到有效 IF_DATA XCP
    bool from_xcp_plus = false;  ///< 数据来源为 XCPplus（优先级更高，§6.3-A）
    std::string last_error;      ///< 提取失败的展示文本（仅 UI 用，B-19）
    ProtocolLayerInfo protocol_layer;              ///< Protocol Layer 参数
    std::vector<TransportEndpoint> transports;     ///< 声明的传输层实例
    std::optional<DaqInfo> daq;                    ///< DAQ 能力（未声明时空）
    std::vector<EventChannelInfo> event_channels;  ///< EVENT_CHANNEL 列表
    std::vector<DaqListLayout>
        static_daq_lists;  ///< STATIC 预定义 DAQ_LIST 布局
};

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_IF_DATA_XCP_HPP_
