/**
 * @file protocol_types.hpp
 * @brief XCP 协议常量、强类型别名与基础枚举定义。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 3 节实现。
 * 本文件为叶子模块，不依赖项目内其他头文件（除标准库）。
 */

#ifndef CALMCAR_XCP_PROTOCOL_TYPES_HPP_
#define CALMCAR_XCP_PROTOCOL_TYPES_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace calmcar::xcp {

/// @brief XCP 字节缓冲区类型
using Bytes = std::vector<std::uint8_t>;

/// @brief 只读字节视图
using BytesView = std::span<const std::uint8_t>;

/// @brief 可写字节视图
using MutableBytesView = std::span<std::uint8_t>;

/// @brief XCP 32 位地址（Address 部分）
using Address = std::uint32_t;

/// @brief XCP 8 位地址扩展
using AddressExtension = std::uint8_t;

/// @brief 以 AG（地址粒度）为单位的元素计数
using ElementCount = std::uint32_t;

/// @brief 以字节为单位的计数
using ByteCount = std::uint32_t;

/// @brief 16 位 CTR（XCP on Ethernet Transport 计数器，按 Frame 递增）
using DatagramCtr = std::uint16_t;

/// @brief 16 位 LEN（XCP on Ethernet Transport 长度字段）
using DatagramLen = std::uint16_t;

/**
 * @brief XCP 命令码（Master -> Slave，范围 0xC0..0xFF）
 *
 * 本阶段仅使用 CONNECT/DISCONNECT/GET_STATUS/SYNCH/GET_COMM_MODE_INFO/
 * SET_MTA/UPLOAD/SHORT_UPLOAD；其余命令码作为协议常量保留，便于后续扩展。
 */
enum class CommandCode : std::uint8_t {
    Connect = 0xFF,            ///< 建立逻辑 XCP 会话
    Disconnect = 0xFE,         ///< 断开逻辑 XCP 会话
    GetStatus = 0xFD,          ///< 查询当前会话状态
    Synch = 0xFC,              ///< 同步并恢复隐含状态
    GetCommModeInfo = 0xFB,    ///< 读取扩展通信模式信息
    GetId = 0xFA,              ///< 获取 Slave 标识或 A2L 信息
    SetRequest = 0xF9,         ///< 请求保存/清除非易失数据
    GetSeed = 0xF8,            ///< 读取解锁 Seed
    Unlock = 0xF7,             ///< 发送 Key 解锁资源
    SetMta = 0xF6,             ///< 设置 Memory Transfer Address
    Upload = 0xF5,             ///< 从当前 MTA 上传内存
    ShortUpload = 0xF4,        ///< 带地址的一次性上传
    BuildChecksum = 0xF3,      ///< 计算内存校验和
    TransportLayerCmd = 0xF2,  ///< 传输层专用命令
    UserCmd = 0xF1,            ///< 用户自定义命令
    Download = 0xF0,           ///< 下载到当前 MTA
    DownloadNext = 0xEF,       ///< Block Mode 续传下载
    DownloadMax = 0xEE,        ///< 最大长度下载
    ShortDownload = 0xED,      ///< 带地址的一次性下载
    ModifyBits = 0xEC,         ///< 位修改
};

/**
 * @brief Slave -> Master 的 Packet Identifier（PID）分类
 *
 * XCP 规定 Slave 侧 PID 空间：0x00..0xFB 为 DAQ DTO，0xFC SERV，0xFD EV，
 * 0xFE ERR，0xFF RES。本阶段 DTO 仅识别、不解析内容。
 */
enum class PacketType : std::uint8_t {
    Res = 0xFF,   ///< Positive Response
    Err = 0xFE,   ///< Negative Response / Error
    Ev = 0xFD,    ///< Event
    Serv = 0xFC,  ///< Service Request
};

/**
 * @brief 从原始首字节判断 Packet 类型
 * @param first_byte 收到的 XCP Packet 首字节
 * @return PacketType 枚举值；若落在 DAQ DTO 范围（0x00..0xFB）则返回
 * std::nullopt
 * @note 返回 nullopt 表示该包是 DAQ DTO，调用方应按 DTO 路径处理。
 */
[[nodiscard]] std::optional<PacketType> ClassifyPacket(
    std::uint8_t first_byte) noexcept;

/**
 * @brief XCP 协议错误码（ERR Packet 的 Byte 1）
 *
 * 仅列举本阶段可能遇到的取值；未知错误码由调用方以原始 uint8_t 保留并上报。
 */
enum class ErrorCode : std::uint8_t {
    CmdSynch = 0x00,                        ///< 命令同步（SYNCH 的成功确认）
    CmdBusy = 0x10,                         ///< Slave 忙
    DaqActive = 0x11,                       ///< DAQ 正在运行
    PgmActive = 0x12,                       ///< Programming 正在进行
    CmdUnknown = 0x20,                      ///< 未知命令
    CmdSyntax = 0x21,                       ///< 命令语法错误
    OutOfRange = 0x22,                      ///< 参数超出范围
    WriteProtected = 0x23,                  ///< 写保护
    AccessDenied = 0x24,                    ///< 访问被拒绝
    AccessLocked = 0x25,                    ///< 需要 Seed&Key 解锁
    PageNotValid = 0x26,                    ///< 页无效
    ModeNotValid = 0x27,                    ///< 模式无效
    SegmentNotValid = 0x28,                 ///< 段无效
    Sequence = 0x29,                        ///< 命令序列错误
    DaqConfig = 0x2A,                       ///< DAQ 配置错误
    MemoryOverflow = 0x30,                  ///< 内存溢出
    Generic = 0x31,                         ///< 通用错误
    Verify = 0x32,                          ///< 校验失败
    ResourceTemporaryNotAccessible = 0x33,  ///< 资源暂时不可访问
    SubcmdUnknown = 0x34,                   ///< 子命令未知
};

/// @brief 将错误码转换为字符串名称（用于诊断和日志）
[[nodiscard]] std::string_view ErrorCodeName(ErrorCode code) noexcept;

/// @brief 将原始错误码字节安全转为 ErrorCode；未知值返回 std::nullopt
[[nodiscard]] std::optional<ErrorCode> ToErrorCode(std::uint8_t raw) noexcept;

/**
 * @brief XCP 事件码（EV Packet 的 Byte 1）
 */
enum class EventCode : std::uint8_t {
    ResumeMode = 0x00,         ///< 进入 RESUME 模式
    ClearDaq = 0x01,           ///< 请求清除 DAQ 配置
    StoreDaq = 0x02,           ///< 请求保存 DAQ 配置
    StoreCal = 0x03,           ///< 请求保存标定数据
    CmdPending = 0x05,         ///< 命令待处理（需重启 Timer）
    DaqOverload = 0x06,        ///< DAQ 过载
    SessionTerminated = 0x07,  ///< 会话被 Slave 终止
    TimeSync = 0x08,           ///< 时间同步
    StimTimeout = 0x09,        ///< STIM 超时
    Sleep = 0x0A,              ///< 休眠
    WakeUp = 0x0B,             ///< 唤醒
    EcuStateChange = 0x0C,     ///< ECU 状态变化
    User = 0xFE,               ///< 用户自定义事件
    Transport = 0xFF,          ///< 传输层事件
};

/// @brief 将事件码转换为字符串名称
[[nodiscard]] std::string_view EventCodeName(EventCode code) noexcept;

/// @brief 将原始事件码字节安全转为 EventCode；未知值返回 std::nullopt
[[nodiscard]] std::optional<EventCode> ToEventCode(std::uint8_t raw) noexcept;

/**
 * @brief XCP 字节序（来自 COMM_MODE_BASIC bit0）
 */
enum class ByteOrder : std::uint8_t {
    Intel = 0,     ///< 小端
    Motorola = 1,  ///< 大端
};

/**
 * @brief 地址粒度（来自 COMM_MODE_BASIC bit1-2）
 *
 * 枚举值即“一个地址单位对应的字节数”。
 */
enum class AddressGranularity : std::uint8_t {
    Byte = 1,   ///< 1 Byte/Address
    Word = 2,   ///< 2 Byte/Address
    DWord = 4,  ///< 4 Byte/Address
};

/// @brief 将 AG 转为字节数
[[nodiscard]] constexpr std::uint8_t AgToBytes(AddressGranularity ag) noexcept;

/**
 * @brief 将 COMM_MODE_BASIC 的 AG 位域编码（bit1-2）转为 AG
 * @param field_value 取自 COMM_MODE_BASIC 的 bit1-2（00/01/10 有效，11 保留）
 * @return 合法时返回对应 AG；11（保留值）返回 std::nullopt
 */
[[nodiscard]] std::optional<AddressGranularity> CommModeBasicToAg(
    std::uint8_t field_value) noexcept;

/// @brief 将 COMM_MODE_BASIC 的 AG 位域还原为 bit1-2 编码值
[[nodiscard]] std::uint8_t AgToCommModeBasicField(
    AddressGranularity ag) noexcept;

/**
 * @brief XCP 资源位定义（RESOURCE 字段）
 *
 * bit1 为保留位（CAL/PAG 共用 bit0），故枚举值不连续。
 */
enum class Resource : std::uint8_t {
    None = 0x00,    ///< 无资源
    CalPag = 0x01,  ///< bit0 标定与分页
    Daq = 0x04,     ///< bit2 数据采集
    Stim = 0x08,    ///< bit3 数据刺激
    Pgm = 0x10,     ///< bit4 刷写
};

/// @brief 资源掩码类型
using ResourceMask = std::underlying_type_t<Resource>;

/// @brief 检查掩码中是否包含指定资源
[[nodiscard]] constexpr bool HasResource(ResourceMask mask,
                                         Resource res) noexcept;

/// @brief 合并资源位
[[nodiscard]] constexpr ResourceMask operator|(Resource lhs,
                                               Resource rhs) noexcept;

/**
 * @brief Session 状态机状态
 */
enum class SessionState {
    Disconnected,   ///< 未连接
    Connecting,     ///< 正在建立连接（CONNECT 已发送，等待响应）
    Connected,      ///< 已连接
    Disconnecting,  ///< 正在断开（DISCONNECT 已发送，等待响应）
    Recovering,     ///< 超时恢复中（SYNCH 已发送，等待 ERR_CMD_SYNCH）
    Failed,         ///< 不可恢复故障，需重新连接
};

/// @brief 将 Session 状态转为字符串
[[nodiscard]] std::string_view SessionStateName(SessionState state) noexcept;

/**
 * @brief CONNECT 响应解析结果（COMM_MODE_BASIC 已拆解）
 */
struct ConnectResponse {
    ResourceMask m_resource_mask_{};            ///< RESOURCE 字段
    ByteOrder m_byte_order_{ByteOrder::Intel};  ///< COMM_MODE_BASIC 中的字节序
    AddressGranularity m_address_granularity_{
        AddressGranularity::Byte};                ///< COMM_MODE_BASIC 中的 AG
    bool m_slave_block_mode_supported_{false};    ///< COMM_MODE_BASIC bit6
    bool m_optional_comm_mode_available_{false};  ///< COMM_MODE_BASIC bit7
    std::uint8_t m_max_cto_{0};   ///< MAX_CTO（有效范围 0x08..0xFF）
    std::uint16_t m_max_dto_{0};  ///< MAX_DTO（有效范围 0x0008..0xFFFF）
    std::uint8_t m_protocol_layer_version_{0};   ///< Protocol Layer 主版本
    std::uint8_t m_transport_layer_version_{0};  ///< Transport Layer 主版本
};

/**
 * @brief GET_STATUS 响应解析结果
 */
struct GetStatusResponse {
    bool m_resume_{false};         ///< Current Session Status bit7
    bool m_daq_running_{false};    ///< Current Session Status bit6
    bool m_clear_daq_req_{false};  ///< Current Session Status bit3
    bool m_store_daq_req_{false};  ///< Current Session Status bit2
    bool m_store_cal_req_{false};  ///< Current Session Status bit0
    ResourceMask
        m_resource_protection_{};     ///< Current Resource Protection Status
    std::uint8_t m_state_number_{0};  ///< STATE_NUMBER
    std::uint16_t m_session_config_id_{0};  ///< Session Configuration ID
};

/**
 * @brief GET_COMM_MODE_INFO 响应解析结果
 */
struct GetCommModeInfoResponse {
    std::uint8_t m_comm_mode_optional_{0};    ///< COMM_MODE_OPTIONAL 原始字节
    std::uint8_t m_max_bs_{0};                ///< Block Mode 最大块大小
    std::uint8_t m_min_st_{0};                ///< 最小分离时间（单位 100μs）
    std::uint8_t m_queue_size_{0};            ///< Interleaved Mode 队列深度
    std::uint8_t m_driver_version_major_{0};  ///< Driver Version 高 nibble
    std::uint8_t m_driver_version_minor_{0};  ///< Driver Version 低 nibble
};

/**
 * @brief XCP 40 位地址（32-bit Address + 8-bit Extension）
 */
struct XcpAddress40 {
    Address m_address_{};             ///< 32 位地址
    AddressExtension m_extension_{};  ///< 8 位地址扩展

    /**
     * @brief 地址前进指定元素数（按 AG 换算为字节数）
     * @param elements 前进的元素数
     * @param ag 地址粒度
     * @return 前进后的新地址；32 位地址部分溢出时返回 std::nullopt
     * @note 地址扩展不参与本次进位（跨扩展边界的处理属于后续 SEGMENT 功能）。
     */
    [[nodiscard]] std::optional<XcpAddress40> Advance(
        ElementCount elements, AddressGranularity ag) const noexcept;
};

/// @brief 相等比较
[[nodiscard]] bool operator==(const XcpAddress40& lhs,
                              const XcpAddress40& rhs) noexcept;

/**
 * @brief 完整 Session 参数快照（CONNECT + 后续查询的不可变结果）
 */
struct SessionParameters {
    ConnectResponse m_connect_;  ///< CONNECT 协商结果
    std::optional<GetCommModeInfoResponse>
        m_comm_mode_info_;                       ///< 仅在查询成功时存在
    std::optional<GetStatusResponse> m_status_;  ///< 仅在查询成功时存在
    bool m_short_upload_available_{true};        ///< SHORT_UPLOAD 是否可用
};

// ---------------------------------------------------------------------------
// constexpr 工具函数实现（声明见上方）
// ---------------------------------------------------------------------------

constexpr std::uint8_t AgToBytes(AddressGranularity ag) noexcept {
    return static_cast<std::uint8_t>(ag);
}

constexpr bool HasResource(ResourceMask mask, Resource res) noexcept {
    return (mask & static_cast<ResourceMask>(res)) != 0;
}

constexpr ResourceMask operator|(Resource lhs, Resource rhs) noexcept {
    return static_cast<ResourceMask>(lhs) | static_cast<ResourceMask>(rhs);
}

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_PROTOCOL_TYPES_HPP_
