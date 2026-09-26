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
 * @details 当前 SDK 快照填 UDP 的 host/port/packet_alignment/sub_commands
 *          （批次12 补全后两项）；其余传输层保留 Kind，
 *          字段留待后续里程碑（§4.3）。多 XCPonUDP/IP 实例只取首个
 *          （设计 §6.3 已登记限制）。
 */
struct TransportEndpoint {
    /// @brief 传输层类别
    enum class Kind : std::uint8_t { UdpIp, TcpIp, Can, Sxi, Usb, Flx };

    Kind kind = Kind::UdpIp;         ///< IF_DATA XCP 段声明的传输层
    std::uint16_t version = 0x0100;  ///< IF_DATA XCP 段版本号
    std::string remote_host;         ///< IPv4/IPv6 字符串；CAN 时为通道名
    std::uint16_t remote_port = 0;   ///< 远端端口
    std::string local_host;          ///< A2L 描述时填入，否则由调用方决定
    std::uint16_t local_port = 0;    ///< 本地端口
    std::uint8_t packet_alignment =
        8;  ///< 位宽 8/16/32 bit（批次12：取自 A2L PACKET_ALIGNMENT；0=未知）
    std::vector<std::uint8_t>
        sub_commands;  ///< 传输层子命令码（批次12：A2L OPTIONAL_TL_SUBCMD；
                       ///< 仅含上游可识别项，空不代表未声明）
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
 * @brief 同一参数在 XCP 与 XCPplus 块取值不同的记录（§6.3-A，Info 级）
 * @details 取值以 XCPplus 为准；本结构仅供 UI 展示与诊断，不阻断功能。
 */
struct XcpPlusConflict {
    std::string parameter;      ///< 参数名（如 MAX_CTO/BYTE_ORDER）
    std::string xcp_value;      ///< plain XCP 块取值（文本）
    std::string xcpplus_value;  ///< XCPplus 块取值（文本，以它为准）
};

/**
 * @brief MODULE 级 IF_DATA XCP 汇总结果
 */
struct IfDataXcpInfo {
    bool ok = false;             ///< 是否成功提取到有效 IF_DATA XCP
    bool from_xcp_plus = false;  ///< 数据来源为 XCPplus（优先级更高，§6.3-A）
    bool ambiguous =
        false;  ///< 多 MODULE 同时声明且未指定 active_module（B-17，批次10）
    std::string module_name;  ///< 来源 MODULE 名（B-17 module scope，批次10）
    std::string last_error;   ///< 提取失败的展示文本（仅 UI 用，B-19）
    ProtocolLayerInfo protocol_layer;              ///< Protocol Layer 参数
    std::vector<TransportEndpoint> transports;     ///< 声明的传输层实例
    std::optional<DaqInfo> daq;                    ///< DAQ 能力（未声明时空）
    std::vector<EventChannelInfo> event_channels;  ///< EVENT_CHANNEL 列表
    std::vector<DaqListLayout>
        static_daq_lists;  ///< STATIC 预定义 DAQ_LIST 布局
    std::vector<XcpPlusConflict>
        plus_conflicts;  ///< XCP vs XCPplus 差异（§6.3-A，批次10）
};

}  // namespace calmcar::xcp::a2l

#endif  // LIBXCP_A2L_IF_DATA_XCP_HPP_
