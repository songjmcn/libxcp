# XCP 1.3.0 最小子集详细设计 — 类接口与头文件

> 依据：`code-plan/XCP_1.3.0_最小协议核心实现计划.md`、`docs/XCP_1.3.0_document.md` 与 `docs/ASAM_XCP_Part3_XCP_on_Ethernet_TCP_UDP_1.1.md`。
>
> XCP on Ethernet 的当前依据为 XCP 1.1 Part 3 文档；历史 `XCP_-Part_3-_Transport_Layer_Specification_XCP_on_Ethernet_(TCP_IP_and_UDP_IP)_-1.0.pdf` 仅用于差异追溯，发生冲突时以 1.1 文档为准。
>
> 本文档只做详细设计与接口定义，不实现代码。所有头文件使用 C++20，跨平台（Windows/Linux/macOS），命名空间 `calmcar::xcp`，注释使用中文 Doxygen 格式。
>
> 已确认的设计决策：
> - **并发模型**：Transport 内部线程收包，通过 `IPacketListener` 回调推送给上层；回调直接在 Transport 工作线程执行，上层自同步。
> - **回调形式**：`IPacketListener` 纯虚抽象接口。
> - **错误风格**：公共 API 抛出 `XcpException` 异常；项目启用 C++ 异常。
> - **命名空间**：`calmcar::xcp`，内部实现细节放 `calmcar::xcp::detail`。

### XCP on Ethernet 1.1 规范升级核对结论

| 项目 | XCP 1.1 Part 3 要求 | 本文档更新结果 |
|---|---|---|
| Ethernet Header | `LEN(u16le) + CTR(u16le)`，无 Tail | 原设计一致，保留 |
| UDP 打包 | 一个 UDP Datagram 可连续容纳多个完整 XCP Frame；Frame 不得跨 Datagram 边界 | 原“一个 Datagram 只含一个 Frame”的接收假设存在冲突，已改为多 Frame 接收解析；发送方向保留单 Frame/Datagram 的项目策略 |
| CTR | Master/Slave 各自独立；每个 XCP Frame、所有方向 Packet 均递增 | 已将 CTR 处理单位明确为 Frame，而非 Datagram |
| UDP 会话端点 | 未连接时对 CONNECT 来源 IP:port 应答；连接后仅接受同一来源 IP，即使端口变更；响应仍发回原 CONNECT 来源 IP:port | 已更新 `UdpTestSlave` 接口、状态字段与测试要求 |
| 特定命令/事件 | 当前没有 Ethernet 特定命令或事件 | 原最小范围不实现 Discovery/GET_SLAVE_ID 等，保持不变 |

---

## 目录

1. [设计概览与模块依赖图](#1-设计概览与模块依赖图)
2. [命名空间与头文件组织](#2-命名空间与头文件组织)
3. [protocol_types.hpp — 协议常量与强类型](#3-protocol_typeshpp--协议常量与强类型)
4. [xcp_error.hpp — 异常与错误码](#4-xcp_errorhpp--异常与错误码)
5. [ixcp_transport.hpp — 传输层抽象接口](#5-ixcp_transporthpp--传输层抽象接口)
6. [udp_header_codec.hpp — UDP Transport Header 编解码](#6-udp_header_codechpp--udp-transport-header-编解码)
7. [udp_transport_config.hpp — UDP Transport 配置](#7-udp_transport_confighpp--udp-transport-配置)
8. [udp_transport.hpp — UDP Transport 实现](#8-udp_transporthpp--udp-transport-实现)
9. [command_codec.hpp — 命令编码器](#9-command_codechpp--命令编码器)
10. [response_parser.hpp — 响应解析器](#10-response_parserhpp--响应解析器)
11. [session.hpp — 会话状态与参数](#11-sessionhpp--会话状态与参数)
12. [command_executor.hpp — 命令执行器](#12-command_executorhpp--命令执行器)
13. [memory_access.hpp — 内存访问](#13-memory_accesshpp--内存访问)
14. [xcp_master.hpp — 顶层门面](#14-xcp_masterhpp--顶层门面)
15. [测试设施接口](#15-测试设施接口)
16. [线程安全模型](#16-线程安全模型)
17. [字节布局参考表](#17-字节布局参考表)
18. [开放问题与后续确认](#18-开放问题与后续确认)

---

## 1. 设计概览与模块依赖图

### 1.1 模块依赖关系

```text
                    xcp_master.hpp  (顶层门面，用户唯一入口)
                    /           \
              session.hpp    memory_access.hpp
                |                |
                |          command_executor.hpp
                |                |
                +-- command_codec.hpp
                +-- response_parser.hpp
                          |
                    ixcp_transport.hpp  (抽象接口)
                          |
                   udp_transport.hpp
                          |
              udp_header_codec.hpp + udp_transport_config.hpp
                          |
              protocol_types.hpp  (被所有模块依赖)
              xcp_error.hpp        (被所有模块依赖)
```

### 1.2 依赖方向规则

- `protocol_types.hpp` 和 `xcp_error.hpp` 是叶子模块，不依赖其他项目头文件。
- `ixcp_transport.hpp` 只依赖 `protocol_types.hpp` 和 `xcp_error.hpp`。
- `command_codec.hpp` 和 `response_parser.hpp` 只依赖 `protocol_types.hpp` 和 `xcp_error.hpp`。
- `session.hpp` 依赖 `protocol_types.hpp` 和 `xcp_error.hpp`，不依赖 Transport。
- `command_executor.hpp` 依赖 `ixcp_transport.hpp`、`command_codec.hpp`、`response_parser.hpp`、`session.hpp`。
- `memory_access.hpp` 依赖 `command_executor.hpp` 和 `session.hpp`。
- `xcp_master.hpp` 组合 `session.hpp`、`command_executor.hpp`、`memory_access.hpp`，并持有 `IXcpTransport`。
- 协议核心（session/codec/parser/executor/memory/master）**不依赖**任何 Socket API、`udp_*.hpp` 或平台专用头文件。

---

## 2. 命名空间与头文件组织

### 2.1 命名空间

```cpp
namespace calmcar::xcp {
    // 公共类型与接口
}

namespace calmcar::xcp::detail {
    // 内部实现细节，不对外承诺稳定性
}
```

### 2.2 头文件守卫

所有头文件必须使用传统预处理宏守卫，不使用 `#pragma once`。宏名采用全大写，并由项目前缀、模块路径和文件名构成：

```cpp
#ifndef CALMCAR_XCP_PROTOCOL_TYPES_HPP_
#define CALMCAR_XCP_PROTOCOL_TYPES_HPP_

// 头文件内容

#endif  // CALMCAR_XCP_PROTOCOL_TYPES_HPP_
```

接口原型中的宏名必须与文件一一对应；例如：

| 头文件 | 守卫宏 |
|---|---|
| `protocol_types.hpp` | `CALMCAR_XCP_PROTOCOL_TYPES_HPP_` |
| `xcp_error.hpp` | `CALMCAR_XCP_XCP_ERROR_HPP_` |
| `ixcp_transport.hpp` | `CALMCAR_XCP_IXCP_TRANSPORT_HPP_` |
| `udp_header_codec.hpp` | `CALMCAR_XCP_UDP_HEADER_CODEC_HPP_` |
| `udp_transport_config.hpp` | `CALMCAR_XCP_UDP_TRANSPORT_CONFIG_HPP_` |
| `udp_transport.hpp` | `CALMCAR_XCP_UDP_TRANSPORT_HPP_` |
| `command_codec.hpp` | `CALMCAR_XCP_COMMAND_CODEC_HPP_` |
| `response_parser.hpp` | `CALMCAR_XCP_RESPONSE_PARSER_HPP_` |
| `session.hpp` | `CALMCAR_XCP_SESSION_HPP_` |
| `command_executor.hpp` | `CALMCAR_XCP_COMMAND_EXECUTOR_HPP_` |
| `memory_access.hpp` | `CALMCAR_XCP_MEMORY_ACCESS_HPP_` |
| `xcp_master.hpp` | `CALMCAR_XCP_XCP_MASTER_HPP_` |
| `tests/mock_transport.hpp` | `CALMCAR_XCP_TEST_MOCK_TRANSPORT_HPP_` |
| `tests/udp_test_slave.hpp` | `CALMCAR_XCP_TEST_UDP_TEST_SLAVE_HPP_` |

### 2.2.1 编码命名规则

以下规则适用于后续实现中的公共接口、私有接口、函数、参数、局部变量、成员变量、全局变量和测试设施；协议中具有固定拼写的标识符（例如 `XCP`、`UDP`、`CTO`、`DTO`、`MTA`、`CTR`、`LEN`）保留全大写缩写。

| 对象 | 规则 | 示例 |
|---|---|---|
| 命名空间 | 全小写 | `calmcar::xcp`、`calmcar::xcp::detail` |
| 类、结构体、枚举、接口 | 大驼峰（PascalCase） | `UdpTransport`、`IXcpTransport`、`CommandExecutor`、`SessionState` |
| 函数、成员函数、全局函数 | 大驼峰（PascalCase） | `Open()`、`Send()`、`ReadMemory()`、`DecodeUdpDatagram()` |
| 预处理宏 | 全大写，下划线分隔 | `CALMCAR_XCP_UDP_TRANSPORT_HPP_` |
| `constexpr` 编译期常量 | `k` + 大驼峰 | `kUdpHeaderSize`、`kUdpMaxDatagramSize` |
| 参数与局部变量 | 小写蛇形（snake_case） | `remote_port`、`xcp_packet`、`retry_count` |
| 普通成员变量 | `m_` + 小写蛇形 + 末尾 `_` | `m_remote_port_`、`m_recv_callback_`、`m_session_state_` |
| 静态成员变量 | `s_` + 小写蛇形 + 末尾 `_` | `s_instance_count_` |
| 全局变量 | `g_` + 小写蛇形 + 末尾 `_` | `g_default_timeouts_` |
| 布尔变量 | 仍按所属作用域前缀，不额外添加 `b` | `m_connected_`、`is_open`、`has_pending_command` |
| 智能指针/容器 | 不添加类型缩写，仅遵循作用域前缀 | `m_transport_`、`m_pending_response_`、`xcp_packets` |

> 说明：本项目的“匈牙利命名”采用**作用域前缀形式**，即成员变量统一使用 `m_<snake_case>_`。不使用 `str`、`u16`、`p` 等类型前缀，避免类型变化导致名称失真；类型信息由 C++ 类型系统表达。
>
> 本文后续展示的接口原型均应在实现时按本节规则落地：函数名改为大驼峰，参数使用小写蛇形，成员变量使用 `m_<snake_case>_`。协议报文中定义的字段名称仅在注释、报文图和标准术语说明中保留原始大写拼写。

### 2.3 目录结构（与计划文档一致）

```text
include/libxcp/
  protocol_types.hpp
  xcp_error.hpp
  ixcp_transport.hpp
  udp_transport_config.hpp
  udp_header_codec.hpp
  udp_transport.hpp
  command_codec.hpp
  response_parser.hpp
  session.hpp
  command_executor.hpp
  memory_access.hpp
  xcp_master.hpp
src/
  ... 对应 .cpp
tests/
  mock_transport.hpp
  udp_test_slave.hpp
  udp_test_slave.cpp
  ... 各测试 .cpp
```

---

## 3. protocol_types.hpp — 协议常量与强类型

> 定义所有协议层枚举、强类型别名和常量。被所有模块依赖。

### 3.1 强类型别名

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <string_view>
#include <optional>
#include <variant>

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

/// @brief 以 AG 为单位的元素计数
using ElementCount = std::uint32_t;

/// @brief 以字节为单位的计数
using ByteCount = std::uint32_t;

/// @brief 16 位 CTR（UDP Transport 计数器）
using DatagramCtr = std::uint16_t;

/// @brief 16 位 LEN（UDP Transport 长度字段）
using DatagramLen = std::uint16_t;

}  // namespace calmcar::xcp
```

### 3.2 命令码枚举

```cpp
namespace calmcar::xcp {

/// @brief XCP 命令码（Master -> Slave，范围 0xC0..0xFF）
enum class CommandCode : std::uint8_t {
    Connect           = 0xFF,
    Disconnect        = 0xFE,
    GetStatus         = 0xFD,
    Synch             = 0xFC,
    GetCommModeInfo   = 0xFB,
    GetId             = 0xFA,
    SetRequest        = 0xF9,
    GetSeed           = 0xF8,
    Unlock            = 0xF7,
    SetMta            = 0xF6,
    Upload            = 0xF5,
    ShortUpload       = 0xF4,
    BuildChecksum     = 0xF3,
    TransportLayerCmd = 0xF2,
    UserCmd           = 0xF1,
    Download          = 0xF0,
    DownloadNext      = 0xEF,
    DownloadMax       = 0xEE,
    ShortDownload     = 0xED,
    ModifyBits        = 0xEC,
};

}  // namespace calmcar::xcp
```

### 3.3 PID 分类枚举

```cpp
namespace calmcar::xcp {

/// @brief Slave -> Master 的 Packet Identifier 分类
enum class PacketType : std::uint8_t {
    Res  = 0xFF,  ///< Positive Response
    Err  = 0xFE,  ///< Negative Response / Error
    Ev   = 0xFD,  ///< Event
    Serv = 0xFC,  ///< Service Request
    // 0x00..0xFB 为 DAQ DTO（本阶段仅识别，不解析内容）
};

/// @brief 从原始首字节判断 Packet 类型
/// @param firstByte 收到的 XCP Packet 首字节
/// @return PacketType 枚举值；若为 DAQ DTO 范围则返回 std::nullopt
/// @note 返回 nullopt 表示该包是 DAQ DTO，调用方应按 DTO 路径处理
std::optional<PacketType> classifyPacket(std::uint8_t firstByte);

}  // namespace calmcar::xcp
```

### 3.4 错误码枚举

```cpp
namespace calmcar::xcp {

/// @brief XCP 协议错误码（ERR Packet 的 Byte 1）
enum class ErrorCode : std::uint8_t {
    CmdSynch                            = 0x00,
    CmdBusy                             = 0x10,
    DaqActive                           = 0x11,
    PgmActive                           = 0x12,
    CmdUnknown                          = 0x20,
    CmdSyntax                           = 0x21,
    OutOfRange                          = 0x22,
    WriteProtected                      = 0x23,
    AccessDenied                        = 0x24,
    AccessLocked                        = 0x25,
    PageNotValid                        = 0x26,
    ModeNotValid                        = 0x27,
    SegmentNotValid                     = 0x28,
    Sequence                            = 0x29,
    DaqConfig                           = 0x2A,
    MemoryOverflow                      = 0x30,
    Generic                             = 0x31,
    Verify                              = 0x32,
    ResourceTemporaryNotAccessible      = 0x33,
    SubcmdUnknown                       = 0x34,
};

/// @brief 将错误码转换为字符串名称（用于诊断和日志）
std::string_view errorCodeName(ErrorCode code);

}  // namespace calmcar::xcp
```

### 3.5 事件码枚举

```cpp
namespace calmcar::xcp {

/// @brief XCP 事件码（EV Packet 的 Byte 1）
enum class EventCode : std::uint8_t {
    ResumeMode        = 0x00,
    ClearDaq          = 0x01,
    StoreDaq          = 0x02,
    StoreCal          = 0x03,
    CmdPending        = 0x05,
    DaqOverload       = 0x06,
    SessionTerminated = 0x07,
    TimeSync          = 0x08,
    StimTimeout       = 0x09,
    Sleep             = 0x0A,
    WakeUp            = 0x0B,
    EcuStateChange    = 0x0C,
    User              = 0xFE,
    Transport         = 0xFF,
};

/// @brief 将事件码转换为字符串名称
std::string_view eventCodeName(EventCode code);

}  // namespace calmcar::xcp
```

### 3.6 字节序与地址粒度

```cpp
namespace calmcar::xcp {

/// @brief XCP 字节序（来自 COMM_MODE_BASIC）
enum class ByteOrder : std::uint8_t {
    Intel    = 0,  ///< 小端
    Motorola = 1,  ///< 大端
};

/// @brief 地址粒度（来自 COMM_MODE_BASIC）
enum class AddressGranularity : std::uint8_t {
    Byte  = 1,  ///< 1 Byte/Address
    Word  = 2,  ///< 2 Byte/Address
    DWord = 4,  ///< 4 Byte/Address
};

/// @brief 将 AG 转为字节数
constexpr std::uint8_t agToBytes(AddressGranularity ag) noexcept;

/// @brief 将字节数转为 AG（仅接受 1/2/4）
std::optional<AddressGranularity> bytesToAg(std::uint8_t bytes) noexcept;

}  // namespace calmcar::xcp
```

### 3.7 资源掩码

```cpp
namespace calmcar::xcp {

/// @brief XCP 资源位定义（RESOURCE 字段）
enum class Resource : std::uint8_t {
    None   = 0x00,
    CalPag = 0x01,  ///< bit0
    Daq    = 0x04,  ///< bit2
    Stim   = 0x08,  ///< bit3
    Pgm    = 0x10,  ///< bit4
};

/// @brief 资源掩码类型
using ResourceMask = std::underlying_type_t<Resource>;

/// @brief 检查掩码中是否包含指定资源
constexpr bool hasResource(ResourceMask mask, Resource res) noexcept;

}  // namespace calmcar::xcp
```

### 3.8 Session 状态

```cpp
namespace calmcar::xcp {

/// @brief Session 状态机状态
enum class SessionState {
    Disconnected,   ///< 未连接
    Connecting,     ///< 正在建立连接（CONNECT 已发送，等待响应）
    Connected,      ///< 已连接
    Disconnecting,  ///< 正在断开（DISCONNECT 已发送，等待响应）
    Recovering,     ///< 超时恢复中（SYNCH 已发送，等待 ERR_CMD_SYNCH）
    Failed,         ///< 不可恢复故障，需重新连接
};

/// @brief 将 Session 状态转为字符串
std::string_view sessionStateName(SessionState state);

}  // namespace calmcar::xcp
```

### 3.9 CONNECT 响应参数结构体

```cpp
namespace calmcar::xcp {

/// @brief CONNECT 响应解析结果（COMM_MODE_BASIC 已拆解）
struct ConnectResponse {
    ResourceMask resourceMask;         ///< RESOURCE 字段
    ByteOrder byteOrder;               ///< COMM_MODE_BASIC 中的字节序
    AddressGranularity addressGranularity;  ///< COMM_MODE_BASIC 中的 AG
    bool slaveBlockModeSupported;      ///< COMM_MODE_BASIC 中的 Block Mode 位
    bool optionalCommModeAvailable;    ///< COMM_MODE_BASIC 中的 Optional 信息可用位
    std::uint8_t maxCto;               ///< MAX_CTO（0x08..0xFF）
    std::uint16_t maxDto;              ///< MAX_DTO（0x0008..0xFFFF）
    std::uint8_t protocolLayerVersion; ///< Protocol Layer 主版本
    std::uint8_t transportLayerVersion;///< Transport Layer 主版本
};

/// @brief GET_STATUS 响应解析结果
struct GetStatusResponse {
    bool resume;           ///< bit7
    bool daqRunning;       ///< bit6
    bool clearDaqReq;      ///< bit3
    bool storeDaqReq;      ///< bit2
    bool storeCalReq;      ///< bit0
    ResourceMask resourceProtection;  ///< 当前资源保护状态
    std::uint8_t stateNumber;         ///< ECU State 编号
    std::uint16_t sessionConfigId;    ///< Session Configuration ID
};

/// @brief GET_COMM_MODE_INFO 响应解析结果
struct GetCommModeInfoResponse {
    std::uint8_t commModeOptional;  ///< Master Block Mode / Interleaved Mode 能力
    std::uint8_t maxBs;             ///< Block Mode 最大块大小
    std::uint8_t minSt;             ///< 最小分离时间（单位 100μs）
    std::uint8_t queueSize;         ///< Interleaved Mode 队列深度
    std::uint8_t driverVersionMajor;///< Driver Version 高 nibble
    std::uint8_t driverVersionMinor;///< Driver Version 低 nibble
};

}  // namespace calmcar::xcp
```

### 3.10 40 位地址结构体

```cpp
namespace calmcar::xcp {

/// @brief XCP 40 位地址（32-bit Address + 8-bit Extension）
struct XcpAddress40 {
    Address address;                ///< 32 位地址
    AddressExtension extension;     ///< 8 位地址扩展

    /// @brief 地址前进指定元素数（按 AG 换算为字节数）
    /// @param elements 前进的元素数
    /// @param ag 地址粒度
    /// @return 前进后的新地址；溢出时返回 nullopt
    [[nodiscard]] std::optional<XcpAddress40> advance(ElementCount elements,
                                                       AddressGranularity ag) const noexcept;
};

/// @brief 比较运算符
bool operator==(const XcpAddress40& lhs, const XcpAddress40& rhs) noexcept;

}  // namespace calmcar::xcp
```

### 3.11 Session 快照

```cpp
namespace calmcar::xcp {

/// @brief 完整 Session 参数快照（CONNECT + 后续查询的不可变结果）
struct SessionParameters {
    ConnectResponse connect;
    std::optional<GetCommModeInfoResponse> commModeInfo;  ///< 仅在查询成功时存在
    std::optional<GetStatusResponse> status;              ///< 仅在查询成功时存在
    bool shortUploadAvailable = true;  ///< SHORT_UPLOAD 是否可用（遇 ERR_CMD_UNKNOWN 后置 false）
};

}  // namespace calmcar::xcp
```

---

## 4. xcp_error.hpp — 异常与错误码

> 定义统一异常类型和错误分类。所有公共 API 通过抛出 `XcpException` 报告错误。

### 4.1 错误分类枚举

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <cstdint>
#include <stdexcept>
#include <string>
#include <optional>
#include "protocol_types.hpp"

namespace calmcar::xcp {

/// @brief 错误分类（机器可判定）
enum class ErrorCategory {
    InvalidArgument,    ///< 本地参数、AG 换算、长度非法
    InvalidState,       ///< 未连接、正在恢复或已有待响应命令
    TransportError,     ///< 打开、发送、接收、关闭失败
    Timeout,            ///< 规定时间内没有最终响应
    MalformedPacket,    ///< PID、长度、对齐或字段非法
    ProtocolError,      ///< Slave 返回 ERR
    UnsupportedFeature, ///< 本阶段不支持的功能（如 Seed&Key）
    RecoveryFailed,     ///< SYNCH 恢复或重试耗尽
};

/// @brief 将错误分类转为字符串
std::string_view errorCategoryName(ErrorCategory cat);

}  // namespace calmcar::xcp
```

### 4.2 异常类

```cpp
namespace calmcar::xcp {

/// @brief XCP 库统一异常类型
/// @details 所有公共 API 通过抛出此异常报告错误。
///          异常对象保留机器可判定分类、协议码、重试次数和底层错误信息。
class XcpException : public std::runtime_error {
public:
    /// @brief 构造异常
    /// @param category 错误分类
    /// @param message 人类可读的错误描述
    /// @param commandCode 相关命令码（可选，用于诊断）
    /// @param errorCode 协议错误码（仅 ProtocolError 时有效）
    /// @param retryCount 恢复重试次数（仅恢复场景有效）
    /// @param transportError 底层 Transport 错误描述（可选）
    XcpException(ErrorCategory category,
                 std::string message,
                 std::optional<CommandCode> commandCode = std::nullopt,
                 std::optional<ErrorCode> errorCode = std::nullopt,
                 int retryCount = 0,
                 std::string transportError = "");

    /// @brief 获取错误分类
    [[nodiscard]] ErrorCategory category() const noexcept;

    /// @brief 获取相关命令码
    [[nodiscard]] std::optional<CommandCode> commandCode() const noexcept;

    /// @brief 获取协议错误码（仅 ProtocolError）
    [[nodiscard]] std::optional<ErrorCode> errorCode() const noexcept;

    /// @brief 获取恢复重试次数
    [[nodiscard]] int retryCount() const noexcept;

    /// @brief 获取底层 Transport 错误描述
    [[nodiscard]] std::string_view transportError() const noexcept;

private:
    ErrorCategory category_;
    std::optional<CommandCode> commandCode_;
    std::optional<ErrorCode> errorCode_;
    int retryCount_;
    std::string transportError_;
};

}  // namespace calmcar::xcp
```

### 4.3 便捷构造函数（可选，内部使用）

```cpp
namespace calmcar::xcp::detail {

/// @brief 构造 InvalidArgument 异常
[[nodiscard]] XcpException makeInvalidArgument(std::string msg);

/// @brief 构造 InvalidState 异常
[[nodiscard]] XcpException makeInvalidState(std::string msg);

/// @brief 构造 TransportError 异常
[[nodiscard]] XcpException makeTransportError(std::string msg, std::string transportDetail = "");

/// @brief 构造 Timeout 异常
[[nodiscard]] XcpException makeTimeout(std::string msg, std::optional<CommandCode> cmd, int retry);

/// @brief 构造 MalformedPacket 异常
[[nodiscard]] XcpException makeMalformedPacket(std::string msg);

/// @brief 构造 ProtocolError 异常
[[nodiscard]] XcpException makeProtocolError(std::string msg,
                                              CommandCode cmd,
                                              ErrorCode code);

/// @brief 构造 UnsupportedFeature 异常
[[nodiscard]] XcpException makeUnsupportedFeature(std::string msg);

}  // namespace calmcar::xcp::detail
```

---

## 5. ixcp_transport.hpp — 传输层抽象接口

> 定义 Transport 层抽象接口。协议核心只依赖此接口，不依赖任何具体 Transport 实现。

### 5.1 监听器接口

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <cstdint>
#include <string>
#include "protocol_types.hpp"
#include "xcp_error.hpp"

namespace calmcar::xcp {

/// @brief Transport 包监听器接口
/// @details Transport 内部线程收到完整 XCP Packet 后调用此接口。
///          回调在 Transport 工作线程上执行，实现者需自行保证线程安全。
class IPacketListener {
public:
    virtual ~IPacketListener() = default;

    /// @brief 收到一个完整的 XCP Packet
    /// @param packet 完整 XCP Packet 字节（不含 Transport Header）
    /// @note 此方法在 Transport 工作线程调用；packet 只在本次回调期间有效，
    ///       监听器若需异步保存必须复制字节。
    virtual void onPacketReceived(BytesView packet) = 0;

    /// @brief Transport 通道已关闭（正常关闭或错误关闭）
    /// @param reason 关闭原因描述
    /// @note 此方法在 Transport 工作线程调用；调用后不再有 onPacketReceived
    virtual void onTransportClosed(std::string_view reason) = 0;

    /// @brief Transport 发生可恢复错误（如畸形 Datagram 丢弃）
    /// @param message 错误描述
    /// @note 此方法在 Transport 工作线程调用；Transport 不会因此关闭
    virtual void onTransportWarning(std::string_view message) = 0;
};

}  // namespace calmcar::xcp
```

### 5.2 Transport 接口

```cpp
namespace calmcar::xcp {

/// @brief XCP Transport 抽象接口
/// @details 协议核心通过此接口收发 XCP Packet，不感知具体传输介质。
///          实现者负责：打开/关闭通道、发送完整 CTO Packet、
///          内部线程接收并通过 IPacketListener 回调推送收到的 Packet。
class IXcpTransport {
public:
    virtual ~IXcpTransport() = default;

    /// @brief 打开 Transport 通道
    /// @param listener 包监听器，生命周期须长于 Transport 使用期
    /// @throws XcpException(TransportError) 打开失败
    /// @note 打开后 Transport 内部线程开始接收并通过 listener 回调
    virtual void open(IPacketListener& listener) = 0;

    /// @brief 关闭 Transport 通道
    /// @note 关闭后不再调用 listener 回调；可安全重复调用
    virtual void close() = 0;

    /// @brief 发送一个完整的 XCP CTO Packet
    /// @param packet 完整 XCP Packet 字节（不含 Transport Header）
    /// @throws XcpException(TransportError) 发送失败
    /// @note 此方法可在任意线程调用；实现需保证线程安全
    virtual void send(BytesView packet) = 0;

    /// @brief Transport 是否已打开
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
};

}  // namespace calmcar::xcp
```

---

## 6. udp_header_codec.hpp — UDP Transport Header 编解码

> 纯函数编解码 XCP 1.1 Part 3（XCP on Ethernet） 的 4 字节 Header。无状态，易测试。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <cstdint>
#include <cstddef>
#include <span>
#include <optional>
#include <string>
#include <vector>
#include "protocol_types.hpp"
#include "xcp_error.hpp"

namespace calmcar::xcp {

/// @brief UDP Transport Header 固定长度（字节）
constexpr std::size_t kUdpHeaderSize = 4;

/// @brief IPv4 UDP Payload 最大理论长度
constexpr std::size_t kUdpMaxDatagramSize = 65507;

/// @brief 单个 XCP Frame 中原始 XCP Packet 的最大理论长度
/// @details 单 Frame 独占 UDP Datagram 时：65507 - 4 Byte Header = 65503。
constexpr std::size_t kUdpMaxXcpPacket = kUdpMaxDatagramSize - kUdpHeaderSize;

/// @brief 编码后的单个 XCP on Ethernet Frame（Header + 一个 XCP Packet）
/// @details 一个 UDP Datagram 可以包含一个或多个此类 Frame；本项目发送方向默认一个 Datagram 只放一个 Frame。
struct UdpFrame {
    Bytes data;  ///< LEN(u16le) + CTR(u16le) + XCP Packet
};

/// @brief 解码后的 UDP Header 字段
struct UdpHeader {
    DatagramLen len;  ///< 原始 XCP Packet 字节数
    DatagramCtr ctr;  ///< 该 XCP Frame 的独立计数器
};

/// @brief Datagram 内单个 XCP Frame 的只读视图
/// @note xcpPacket 的生命周期不超过传入 decodeUdpDatagram() 的字节视图。
struct UdpFrameView {
    UdpHeader header;
    BytesView xcpPacket;
};

/// @brief 编码一个 XCP on Ethernet Frame
/// @param xcpPacket 原始 XCP Packet（不含 Transport Header）
/// @param ctr 该 Frame 的发送计数器值
/// @return 编码后的 Frame
/// @throws XcpException(InvalidArgument) xcpPacket.size() > kUdpMaxXcpPacket (65503)
[[nodiscard]] UdpFrame encodeUdpFrame(BytesView xcpPacket, DatagramCtr ctr);

/// @brief 解码一个 UDP Datagram 中连续打包的全部 XCP Frame
/// @param datagram 完整 UDP Payload
/// @return 全部 Frame 视图；空 Datagram、Header 不完整、LEN 为 0、LEN 越界或末尾残留字节时返回 nullopt
/// @note XCP 1.1 Part 3 允许一个 UDP Datagram 包含多个完整 Frame，
///       但任何单个 Frame 都不得跨 Datagram 边界。
[[nodiscard]] std::optional<std::vector<UdpFrameView>> decodeUdpDatagram(BytesView datagram) noexcept;

}  // namespace calmcar::xcp
```

---

## 7. udp_transport_config.hpp — UDP Transport 配置

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <cstdint>
#include <cstddef>
#include <string>
#include <optional>
#include "protocol_types.hpp"
#include "udp_header_codec.hpp"

namespace calmcar::xcp {

/// @brief UDP Transport 配置参数
struct UdpTransportConfig {
    /// @brief 远端 Slave IPv4 地址（如 "192.168.1.10" 或 "127.0.0.1"）
    std::string remoteHost;

    /// @brief 远端 Slave UDP 业务端口
    std::uint16_t remotePort = 0;

    /// @brief 本地绑定 IPv4 地址（默认 "0.0.0.0"，Loopback 测试用 "127.0.0.1"）
    std::string localHost = "0.0.0.0";

    /// @brief 本地绑定端口（0 表示由 OS 分配临时端口）
    std::uint16_t localPort = 0;

    /// @brief 接收超时（毫秒），0 表示阻塞接收
    ///        实际用于内部线程的周期性检查，不影响上层命令超时
    std::uint32_t receivePollIntervalMs = 100;

    /// @brief 最大允许的单个 XCP Frame 内原始 Packet 长度（字节）
    ///        默认 kUdpMaxXcpPacket (65503)；可设更小值以避免 IP 分片
    std::size_t maxFramePacketSize = kUdpMaxXcpPacket;

    /// @brief 最大允许的 UDP Datagram Payload 长度（字节）
    ///        默认 kUdpMaxDatagramSize (65507)，限制连续打包 Frame 的总长度
    std::size_t maxDatagramSize = kUdpMaxDatagramSize;

    /// @brief 是否严格匹配远端端口（true 时要求收到的包来自 remotePort）
    /// @details 这是 Master 侧的项目安全策略，不是 XCP 1.1 Part 3 对 Slave
    ///          连接绑定规则的复刻；设 false 时仅匹配 remoteHost。
    bool strictRemotePort = true;
};

}  // namespace calmcar::xcp
```

---

## 8. udp_transport.hpp — UDP Transport 实现

> 实现 `IXcpTransport`，封装跨平台 IPv4 UDP Socket、4 字节 Header 和双向 CTR。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <atomic>
#include <thread>
#include <mutex>
#include <optional>
#include <memory>
#include <string>
#include <cstddef>
#include "ixcp_transport.hpp"
#include "udp_transport_config.hpp"
#include "udp_header_codec.hpp"

namespace calmcar::xcp {

/// @brief 标准 XCP 1.1 Part 3 UDP/IP Transport 实现
class UdpTransport : public IXcpTransport {
public:
    /// @brief 构造 Transport
    /// @param config 配置参数
    explicit UdpTransport(UdpTransportConfig config);

    /// @brief 析构，自动关闭
    ~UdpTransport() override;

    // 禁止拷贝
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    /// @brief 打开 Transport 通道
    /// @throws XcpException(TransportError) Socket 创建、绑定失败
    void open(IPacketListener& listener) override;

    /// @brief 关闭 Transport 通道
    void close() override;

    /// @brief 发送一个完整的 XCP CTO Packet
    /// @throws XcpException(TransportError) 发送失败
    void send(BytesView packet) override;

    /// @brief Transport 是否已打开
    [[nodiscard]] bool isOpen() const noexcept override;

    /// @brief 获取当前发送方向 CTR 值（诊断用）
    [[nodiscard]] DatagramCtr sendCtr() const noexcept;

    /// @brief 获取最近收到的接收方向 CTR 值（诊断用）
    [[nodiscard]] std::optional<DatagramCtr> lastReceiveCtr() const noexcept;

private:
    /// @brief 接收线程主循环
    void receiveLoop();

    /// @brief 处理收到的 UDP Datagram
    /// @details 先校验来源和 Datagram 长度，再解码其中连续打包的全部 Frame；
    ///          每个通过 CTR 校验的 Frame 分别回调给 IPacketListener。
    /// @return true 表示已正常处理；false 表示需要退出循环
    bool handleDatagram(const std::uint8_t* data, std::size_t size,
                         const std::string& srcIp, std::uint16_t srcPort);

    /// @brief 处理 Datagram 内的单个 Frame
    /// @param frame 已完成 LEN 边界校验的 Frame 视图
    /// @return true 表示 Frame 已交付上层；false 表示因 CTR 规则丢弃
    bool handleFrame(const UdpFrameView& frame);

    /// @brief 检查源地址是否匹配配置的远端
    bool isRemoteMatch(const std::string& srcIp, std::uint16_t srcPort) const;

    /// @brief 打开 Transport 时固定的 UDP 参数
    UdpTransportConfig config_;

    /// @brief 回调接收完整 XCP Packet 的监听器（非拥有）
    IPacketListener* listener_ = nullptr;

    /// @brief 平台 Socket 资源的私有实现声明
    struct SocketImpl;

    /// @brief 平台 UDP Socket 资源的拥有者
    std::unique_ptr<SocketImpl> socket_;

    /// @brief Transport 接收工作线程
    std::thread receiveThread_;

    /// @brief 接收循环运行标记
    std::atomic<bool> running_{false};

    /// @brief 保护发送、Frame 编码与 CTR 递增的互斥量
    mutable std::mutex sendMutex_;

    /// @brief 下一个 Master→Slave XCP Frame 使用的 CTR
    std::atomic<DatagramCtr> sendCtr_{0};

    /// @brief 保护接收 CTR 基线和最近值的互斥量
    mutable std::mutex recvMutex_;

    /// @brief 最近接收并接受的 Slave→Master Frame CTR
    std::optional<DatagramCtr> lastRecvCtr_;

    /// @brief 是否已由首个合法接收 Frame 建立 CTR 基线
    bool recvBaselineEstablished_ = false;
};

}  // namespace calmcar::xcp
```

### 8.1 接收 CTR 策略说明（实现细节，写入 .cpp 注释）

```
接收策略（与 XCP 1.1 Part 3 及项目策略一致）：
1. 一个 UDP Datagram 可按 LEN 顺序解出多个完整 Frame；每个 Frame 均独立执行 CTR 检查。
2. 首个合法 Frame 建立 lastRecvCtr_ 基线，接收并推进。
3. CTR == 期望值（lastRecvCtr_ + 1 mod 65536）→ 接收，推进。
4. 前向跳号（模 65536 差值 1..32767）→ 接收当前 Frame，报告缺口，推进。
5. 重复或后向乱序（差值 1..32767 反方向）→ 丢弃该 Frame，报告诊断。
6. 恰好相差 0x8000 → 歧义，丢弃该 Frame，报告诊断。
7. Datagram 中任一 Frame 的 Header/LEN 越界或存在尾部残留字节时，按项目原子性策略丢弃整个 Datagram，避免交付同一 Datagram 中的部分内容。
```

---

## 9. command_codec.hpp — 命令编码器

> 将高层请求参数编码为 XCP CTO Byte Sequence。无状态，纯函数风格。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <cstdint>
#include "protocol_types.hpp"

namespace calmcar::xcp {

/// @brief XCP 命令编码器
/// @details 将命令参数编码为 CTO Byte Sequence。
///          编码使用当前 Session 的 Byte Order（CONNECT 协商后确定）。
///          所有 reserved 字节填 0。发送前需检查不超过 MAX_CTO。
class CommandCodec {
public:
    /// @brief 构造编码器
    /// @param byteOrder Session 字节序（CONNECT 后确定）
    explicit CommandCodec(ByteOrder byteOrder) noexcept;

    // ---- 命令编码 ----

    /// @brief 编码 CONNECT 命令
    /// @param mode 0x00=普通, 0x01=用户自定义
    /// @return CTO: [0xFF][mode]
    [[nodiscard]] Bytes encodeConnect(std::uint8_t mode = 0x00) const;

    /// @brief 编码 DISCONNECT 命令
    /// @return CTO: [0xFE][0x00]
    [[nodiscard]] Bytes encodeDisconnect() const;

    /// @brief 编码 GET_STATUS 命令
    /// @return CTO: [0xFD][0x00]
    [[nodiscard]] Bytes encodeGetStatus() const;

    /// @brief 编码 SYNCH 命令
    /// @return CTO: [0xFC][0x00]
    [[nodiscard]] Bytes encodeSynch() const;

    /// @brief 编码 GET_COMM_MODE_INFO 命令
    /// @return CTO: [0xFB][0x00]
    [[nodiscard]] Bytes encodeGetCommModeInfo() const;

    /// @brief 编码 SET_MTA 命令
    /// @param extension 地址扩展
    /// @param address 32 位地址
    /// @return CTO: [0xF6][reserved][extension][addr_b0..b3]
    [[nodiscard]] Bytes encodeSetMta(AddressExtension extension, Address address) const;

    /// @brief 编码 UPLOAD 命令
    /// @param numberOfElements 要读取的元素数（以 AG 为单位）
    /// @return CTO: [0xF5][numberOfElements]
    [[nodiscard]] Bytes encodeUpload(ElementCount numberOfElements) const;

    /// @brief 编码 SHORT_UPLOAD 命令
    /// @param numberOfElements 要读取的元素数
    /// @param extension 地址扩展
    /// @param address 32 位地址
    /// @return CTO: [0xF4][numberOfElements][reserved][extension][addr_b0..b3]
    [[nodiscard]] Bytes encodeShortUpload(ElementCount numberOfElements,
                                           AddressExtension extension,
                                           Address address) const;

private:
    ByteOrder byteOrder_;

    /// @brief 按字节序写入 16 位值到缓冲区
    void writeU16(Bytes& buf, std::uint16_t val) const;

    /// @brief 按字节序写入 32 位值到缓冲区
    void writeU32(Bytes& buf, std::uint32_t val) const;
};

}  // namespace calmcar::xcp
```

### 9.1 字节布局参考

| 命令 | CTO 布局 | 字节数 |
|---|---|---|
| CONNECT | `[FF][mode]` | 2 |
| DISCONNECT | `[FE][00]` | 2 |
| GET_STATUS | `[FD][00]` | 2 |
| SYNCH | `[FC][00]` | 2 |
| GET_COMM_MODE_INFO | `[FB][00]` | 2 |
| SET_MTA | `[F6][00][ext][addr3][addr2][addr1][addr0]` | 8 |
| UPLOAD | `[F5][n]` | 2 |
| SHORT_UPLOAD | `[F4][n][00][ext][addr3][addr2][addr1][addr0]` | 8 |

> 注：addr 字节序按 Session Byte Order；`[00]` 为 reserved。

---

## 10. response_parser.hpp — 响应解析器

> 解析收到的 XCP Packet，分类为 RES/ERR/EV/SERV/DTO 并提取字段。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <cstdint>
#include <optional>
#include <variant>
#include "protocol_types.hpp"

namespace calmcar::xcp {

/// @brief Positive Response 内容（按命令区分）
struct PositiveResponse {
    CommandCode command;  ///< 对应的命令码（由调用方传入或从上下文推断）
    Bytes data;           ///< RES 后的数据（不含 0xFF 前缀）
};

/// @brief Negative Response 内容
struct NegativeResponse {
    ErrorCode errorCode;              ///< ERR Packet 的 Byte 1
    Bytes additionalInfo;             ///< 可选附加信息（Byte 2..）
};

/// @brief Event Packet 内容
struct EventPacket {
    EventCode eventCode;              ///< EV Packet 的 Byte 1
    Bytes info;                       ///< 可选 Event 信息（Byte 2..）
};

/// @brief Service Request Packet 内容
struct ServicePacket {
    std::uint8_t serviceCode;         ///< SERV Packet 的 Byte 1
    Bytes data;                       ///< 可选 Service 数据
};

/// @brief DTO Packet（本阶段仅识别，不解析内容）
struct DtoPacket {
    std::uint8_t pid;                 ///< 原始 PID（0x00..0xFB）
    Bytes data;                       ///< DTO 数据
};

/// @brief 解析后的 Packet 联合类型
using ParsedPacket = std::variant<
    PositiveResponse,
    NegativeResponse,
    EventPacket,
    ServicePacket,
    DtoPacket
>;

/// @brief XCP 响应解析器
/// @details 解析收到的 XCP Packet，按首字节分类并提取字段。
///          使用 Session Byte Order 解析多字节字段。
///          对截断包、非法长度返回 nullopt 或抛出异常。
class ResponseParser {
public:
    /// @brief 构造解析器
    /// @param byteOrder Session 字节序
    explicit ResponseParser(ByteOrder byteOrder) noexcept;

    /// @brief 解析一个完整的 XCP Packet
    /// @param packet 完整 XCP Packet 字节
    /// @param expectedCommand 调用方期望的命令码（用于 PositiveResponse.command）
    /// @return 解析结果；畸形包返回 nullopt
    [[nodiscard]] std::optional<ParsedPacket> parse(BytesView packet,
                                                      CommandCode expectedCommand) const;

    // ---- 专用解析方法 ----

    /// @brief 解析 CONNECT 响应
    /// @param resData RES 后的数据（不含 0xFF 前缀）
    /// @return 解析结果；格式非法返回 nullopt
    [[nodiscard]] std::optional<ConnectResponse> parseConnectResponse(BytesView resData) const;

    /// @brief 解析 GET_STATUS 响应
    [[nodiscard]] std::optional<GetStatusResponse> parseGetStatusResponse(BytesView resData) const;

    /// @brief 解析 GET_COMM_MODE_INFO 响应
    [[nodiscard]] std::optional<GetCommModeInfoResponse> parseGetCommModeInfoResponse(BytesView resData) const;

private:
    ByteOrder byteOrder_;

    /// @brief 按字节序读取 16 位值
    [[nodiscard]] std::uint16_t readU16(BytesView data, std::size_t offset) const;

    /// @brief 按字节序读取 32 位值
    [[nodiscard]] std::uint32_t readU32(BytesView data, std::size_t offset) const;
};

}  // namespace calmcar::xcp
```

### 10.1 CONNECT 响应布局

```text
Byte:  0    1         2                3       4    5    6    7
       FF   RESOURCE  COMM_MODE_BASIC  MAX_CTO  DTO_lo DTO_hi ProtoVer TransportVer
```

- `COMM_MODE_BASIC` 解码：
  - bit0: BYTE_ORDER (0=Intel, 1=Motorola)
  - bit1-2: AG (00=BYTE, 01=WORD, 10=DWORD)
  - bit6: Slave Block Mode Supported
  - bit7: Optional Comm Mode Available

> **注意**：上述 bit 位定义是当前依据译文的推断。实际编码前必须对照 ASAM 官方规范的 CONNECT 命令字节表逐 bit 确认。若译文描述与官方表不符，以官方表为准并更新本设计。此条已列入第 18 节"开放问题"。

---

## 11. session.hpp — 会话状态与参数

> 管理 Session 状态机和协商参数。线程安全（被 CommandExecutor 和上层共用）。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <mutex>
#include <optional>
#include "protocol_types.hpp"
#include "xcp_error.hpp"

namespace calmcar::xcp {

/// @brief XCP 会话管理器
/// @details 维护 Session 状态机、协商参数和单 Outstanding Command 约束。
///          线程安全：所有公共方法内部加锁。
class Session {
public:
    Session() = default;
    ~Session() = default;

    // 禁止拷贝
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- 状态查询 ----

    /// @brief 获取当前 Session 状态
    [[nodiscard]] SessionState state() const;

    /// @brief 是否已连接（Connected 状态）
    [[nodiscard]] bool isConnected() const;

    /// @brief 是否有等待响应的命令（Outstanding Command）
    [[nodiscard]] bool hasPendingCommand() const;

    // ---- 状态迁移 ----

    /// @brief 进入 Connecting 状态
    /// @throws XcpException(InvalidState) 当前状态不允许连接
    void beginConnecting();

    /// @brief CONNECT 成功，保存参数并进入 Connected
    /// @throws XcpException(InvalidArgument) 参数校验失败
    void establishConnection(const ConnectResponse& connectResponse);

    /// @brief 进入 Disconnecting 状态
    /// @throws XcpException(InvalidState) 当前状态不允许断开
    void beginDisconnecting();

    /// @brief DISCONNECT 成功，进入 Disconnected
    void completeDisconnection();

    /// @brief 进入 Recovering 状态
    /// @throws XcpException(InvalidState) 当前状态不允许恢复
    void beginRecovery();

    /// @brief 恢复成功，回到 Connected
    void completeRecovery();

    /// @brief 标记 Session 为 Failed
    /// @param reason 失败原因
    void fail(std::string_view reason);

    /// @brief 重置到 Disconnected（用于强制清理）
    void reset();

    // ---- Outstanding Command 管理 ----

    /// @brief 标记命令已发送，等待响应
    /// @throws XcpException(InvalidState) 已有 Pending Command
    void markCommandSent(CommandCode cmd);

    /// @brief 标记命令响应已收到
    void clearPendingCommand();

    /// @brief 获取当前 Pending 命令码
    [[nodiscard]] std::optional<CommandCode> pendingCommand() const;

    // ---- 参数访问 ----

    /// @brief 获取 Session 参数快照
    [[nodiscard]] SessionParameters parameters() const;

    /// @brief 更新 GET_COMM_MODE_INFO 结果
    void updateCommModeInfo(const GetCommModeInfoResponse& info);

    /// @brief 更新 GET_STATUS 结果
    void updateStatus(const GetStatusResponse& status);

    /// @brief 标记 SHORT_UPLOAD 不可用（降级）
    void disableShortUpload();

    /// @brief 获取当前 Byte Order
    [[nodiscard]] ByteOrder byteOrder() const;

    /// @brief 获取当前 AG
    [[nodiscard]] AddressGranularity addressGranularity() const;

    /// @brief 获取 MAX_CTO
    [[nodiscard]] std::uint8_t maxCto() const;

    /// @brief 获取 MAX_DTO
    [[nodiscard]] std::uint16_t maxDto() const;

private:
    mutable std::mutex mutex_;
    SessionState state_ = SessionState::Disconnected;
    SessionParameters params_;
    std::optional<CommandCode> pendingCommand_;
    std::string failReason_;

    /// @brief 校验 CONNECT 参数
    /// @throws XcpException(InvalidArgument) 参数非法
    void validateConnectParams(const ConnectResponse& resp) const;
};

}  // namespace calmcar::xcp
```

---

## 12. command_executor.hpp — 命令执行器

> 核心调度器：编码命令→发送→等待响应→解析→错误恢复。实现 `IPacketListener`。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>
#include "ixcp_transport.hpp"
#include "command_codec.hpp"
#include "response_parser.hpp"
#include "session.hpp"
#include "xcp_error.hpp"

namespace calmcar::xcp {

/// @brief 命令超时配置
struct CommandTimeouts {
    /// @brief 普通命令超时（毫秒）
    std::chrono::milliseconds commandTimeout{1000};

    /// @brief SYNCH 恢复超时（毫秒）
    std::chrono::milliseconds synchTimeout{1000};

    /// @brief 最大恢复重试次数（不含首次尝试）
    int maxRetries = 2;
};

/// @brief 命令执行结果
struct CommandResult {
    ParsedPacket response;  ///< 最终收到的响应（RES 或 ERR）
};

/// @brief 事件观察者接口（可选，用于上层接收异步 Event）
class IEventListener {
public:
    virtual ~IEventListener() = default;

    /// @brief 收到异步 Event
    virtual void onEvent(const EventPacket& event) = 0;

    /// @brief 收到异步 Service Request
    virtual void onService(const ServicePacket& service) = 0;

    /// @brief 收到 DTO（本阶段仅识别）
    virtual void onDto(const DtoPacket& dto) = 0;
};

/// @brief 命令执行器
/// @details 实现 IPacketListener，负责命令的发送-等待-解析-恢复。
///          线程安全：内部用 mutex + condition_variable 同步。
///          Standard Communication Model：同一时刻最多一个 Pending Command。
class CommandExecutor : public IPacketListener {
public:
    /// @brief 构造执行器
    /// @param transport Transport 实例
    /// @param session Session 实例
    /// @param timeouts 超时配置
    /// @param eventListener 事件监听器（可选，可为 nullptr）
    CommandExecutor(IXcpTransport& transport,
                    Session& session,
                    CommandTimeouts timeouts = {},
                    IEventListener* eventListener = nullptr);

    /// @brief 析构
    ~CommandExecutor() override;

    // ---- IPacketListener 实现 ----

    void onPacketReceived(BytesView packet) override;
    void onTransportClosed(std::string_view reason) override;
    void onTransportWarning(std::string_view message) override;

    // ---- 命令执行 ----

    /// @brief 执行 CONNECT 命令
    /// @param mode 0x00=普通, 0x01=用户自定义
    /// @return CONNECT 响应解析结果
    /// @throws XcpException 超时、协议错误或恢复失败
    [[nodiscard]] ConnectResponse executeConnect(std::uint8_t mode = 0x00);

    /// @brief 执行 DISCONNECT 命令
    /// @throws XcpException 超时或协议错误
    void executeDisconnect();

    /// @brief 执行 GET_STATUS 命令
    /// @return GET_STATUS 响应解析结果
    [[nodiscard]] GetStatusResponse executeGetStatus();

    /// @brief 执行 GET_COMM_MODE_INFO 命令
    /// @return 响应结果；若 Slave 返回 ERR_CMD_UNKNOWN 则返回 nullopt
    [[nodiscard]] std::optional<GetCommModeInfoResponse> executeGetCommModeInfo();

    /// @brief 执行 SET_MTA 命令
    /// @param extension 地址扩展
    /// @param address 32 位地址
    void executeSetMta(AddressExtension extension, Address address);

    /// @brief 执行 UPLOAD 命令
    /// @param numberOfElements 元素数
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes executeUpload(ElementCount numberOfElements);

    /// @brief 执行 SHORT_UPLOAD 命令
    /// @param numberOfElements 元素数
    /// @param extension 地址扩展
    /// @param address 32 位地址
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes executeShortUpload(ElementCount numberOfElements,
                                            AddressExtension extension,
                                            Address address);

    /// @brief 发送 SYNCH（用于恢复，通常不直接调用）
    /// @return true 表示收到 ERR_CMD_SYNCH（恢复成功）
    bool sendSynch();

private:
    /// @brief 执行单条命令的通用流程（编码→发送→等待→解析）
    /// @param cmd 命令码
    /// @param encodedPacket 已编码的 CTO
    /// @return 解析后的响应
    /// @throws XcpException 超时或协议错误
    [[nodiscard]] ParsedPacket executeCommand(CommandCode cmd, BytesView encodedPacket);

    /// @brief 等待当前 Pending Command 的响应
    /// @param timeout 超时时长
    /// @return 收到的响应；超时返回 nullopt
    [[nodiscard]] std::optional<ParsedPacket> waitForResponse(std::chrono::milliseconds timeout);

    /// @brief 超时恢复流程：SYNCH → 等待 ERR_CMD_SYNCH → 恢复隐含状态
    /// @param cmd 原命令码
    /// @throws XcpException 恢复失败
    void performRecovery(CommandCode cmd);

    // 依赖
    IXcpTransport& transport_;
    Session& session_;
    CommandTimeouts timeouts_;
    IEventListener* eventListener_;

    // Codec/Parser（在 Session 字节序确定后创建）
    std::optional<CommandCodec> codec_;
    std::optional<ResponseParser> parser_;

    // 同步
    mutable std::mutex mutex_;
    std::condition_variable responseCv_;
    std::optional<ParsedPacket> pendingResponse_;
    bool transportClosed_ = false;
    std::string transportCloseReason_;

    // EV_CMD_PENDING 计时
    bool cmdPendingReceived_ = false;
};

}  // namespace calmcar::xcp
```

### 12.1 executeCommand 内部流程

```text
executeCommand(cmd, encodedPacket):
  1. session_.markCommandSent(cmd)      // 检查无 Pending，设置 Pending
  2. transport_.send(encodedPacket)
  3. wait for responseCv_ with timeout
     - onPacketReceived 回调中：
       * RES/ERR → 唤醒 waitForResponse
       * EV(CMD_PENDING) → 重启 Timer，不唤醒
       * EV(其他) → eventListener->onEvent()，不唤醒
       * SERV → eventListener->onService()
       * DTO → eventListener->onDto()
  4. 若超时:
       performRecovery(cmd)
       重试原命令（最多 maxRetries 次）
  5. 若收到 RES → 返回 PositiveResponse
  6. 若收到 ERR:
       - ERR_CMD_SYNCH 仅在 Recovery 中视为成功
       - ERR_ACCESS_LOCKED → throw UnsupportedFeature
       - ERR_CMD_UNKNOWN(GET_COMM_MODE_INFO/SHORT_UPLOAD) → 返回给调用方降级
       - 其他 → throw ProtocolError
```

---

## 13. memory_access.hpp — 内存访问

> 高层内存读取 API，封装 SHORT_UPLOAD 和 SET_MTA+UPLOAD 降级逻辑。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include "command_executor.hpp"
#include "session.hpp"

namespace calmcar::xcp {

/// @brief 内存访问接口
/// @details 封装轮询读取逻辑：优先 SHORT_UPLOAD，必要时降级到 SET_MTA+UPLOAD。
///          自动处理 AG 换算、分块和地址溢出检查。
class MemoryAccess {
public:
    /// @brief 构造
    /// @param executor 命令执行器
    /// @param session Session（用于获取 AG/MAX_CTO）
    MemoryAccess(CommandExecutor& executor, Session& session);

    /// @brief 读取内存（以元素为单位）
    /// @param address 32 位地址
    /// @param extension 地址扩展
    /// @param elementCount 元素数（以 AG 为单位）
    /// @return 读取到的原始字节
    /// @throws XcpException(InvalidArgument) 参数非法或地址溢出
    /// @throws XcpException 协议错误或超时
    [[nodiscard]] Bytes readElements(Address address,
                                      AddressExtension extension,
                                      ElementCount elementCount);

    /// @brief 读取内存（以字节为单位）
    /// @param address 32 位地址
    /// @param extension 地址扩展
    /// @param byteCount 字节数（必须可被 AG 整除）
    /// @return 读取到的原始字节
    /// @throws XcpException(InvalidArgument) byteCount 不能被 AG 整除
    [[nodiscard]] Bytes readBytes(Address address,
                                   AddressExtension extension,
                                   ByteCount byteCount);

    /// @brief 读取内存（单次 SHORT_UPLOAD，最多 MAX_CTO/AG 元素）
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes shortUpload(Address address,
                                     AddressExtension extension,
                                     ElementCount elementCount);

    /// @brief 读取内存（SET_MTA + 多次 UPLOAD 分块）
    /// @return 拼接后的完整字节序列
    [[nodiscard]] Bytes uploadChunked(Address address,
                                       AddressExtension extension,
                                       ElementCount elementCount);

private:
    CommandExecutor& executor_;
    Session& session_;

    /// @brief 检查 SHORT_UPLOAD 是否可用且单包可容纳
    [[nodiscard]] bool canUseShortUpload(ElementCount elementCount) const;

    /// @brief 计算 UPLOAD 单块最大元素数
    [[nodiscard]] ElementCount maxUploadElements() const;

    /// @brief 计算 SHORT_UPLOAD 最大元素数
    [[nodiscard]] ElementCount maxShortUploadElements() const;

    /// @brief 校验读取参数
    /// @throws XcpException(InvalidArgument) 参数非法或地址溢出
    void validateRead(Address address, ElementCount elementCount) const;
};

}  // namespace calmcar::xcp
```

---

## 14. xcp_master.hpp — 顶层门面

> 用户唯一入口。组合 Session、CommandExecutor、MemoryAccess，持有 Transport。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <memory>
#include <functional>
#include "session.hpp"
#include "command_executor.hpp"
#include "memory_access.hpp"
#include "ixcp_transport.hpp"
#include "xcp_error.hpp"

namespace calmcar::xcp {

/// @brief XCP Master 顶层门面
/// @details 用户通过此类使用 XCP 库。内部组合 Session、CommandExecutor、MemoryAccess。
///          构造时传入 Transport 实例（如 UdpTransport）。
class XcpMaster {
public:
    /// @brief 构造
    /// @param transport Transport 实例（XcpMaster 持有所有权）
    /// @param timeouts 命令超时配置
    /// @param eventListener 事件监听器（可选）
    explicit XcpMaster(std::unique_ptr<IXcpTransport> transport,
                        CommandTimeouts timeouts = {},
                        IEventListener* eventListener = nullptr);

    /// @brief 析构，自动断开连接
    ~XcpMaster();

    // 禁止拷贝
    XcpMaster(const XcpMaster&) = delete;
    XcpMaster& operator=(const XcpMaster&) = delete;

    // ---- 连接管理 ----

    /// @brief 建立 XCP 连接
    /// @details 执行: Transport.open → CONNECT → [GET_COMM_MODE_INFO] → GET_STATUS
    /// @throws XcpException 连接失败
    void connect();

    /// @brief 断开 XCP 连接
    /// @details 执行: DISCONNECT → Transport.close。即使失败也释放本地资源。
    void disconnect();

    /// @brief 是否已连接
    [[nodiscard]] bool isConnected() const;

    // ---- 内存读取 ----

    /// @brief 读取内存（以字节为单位）
    /// @param address 32 位地址
    /// @param extension 地址扩展
    /// @param byteCount 字节数（必须可被 AG 整除）
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes readMemory(Address address,
                                    AddressExtension extension,
                                    ByteCount byteCount);

    /// @brief 读取内存（以元素为单位）
    /// @param address 32 位地址
    /// @param extension 地址扩展
    /// @param elementCount 元素数（以 AG 为单位）
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes readMemory(Address address,
                                    AddressExtension extension,
                                    ElementCount elementCount);

    // ---- 状态查询 ----

    /// @brief 获取 Session 参数快照
    [[nodiscard]] SessionParameters sessionParameters() const;

    /// @brief 获取当前 Session 状态
    [[nodiscard]] SessionState sessionState() const;

    /// @brief 手动查询 GET_STATUS 并更新 Session
    /// @return GET_STATUS 响应
    [[nodiscard]] GetStatusResponse queryStatus();

private:
    std::unique_ptr<IXcpTransport> transport_;
    Session session_;
    std::unique_ptr<CommandExecutor> executor_;
    std::unique_ptr<MemoryAccess> memoryAccess_;
};

}  // namespace calmcar::xcp
```

---

## 15. 测试设施接口

### 15.1 MockTransport（tests/mock_transport.hpp）

> 实现 `IXcpTransport`，不使用真实 Socket。脚本化响应队列，用于确定性单元测试。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <deque>
#include <mutex>
#include <condition_variable>
#include <functional>
#include "ixcp_transport.hpp"

namespace calmcar::xcp::test {

/// @brief 脚本化 Mock Transport
/// @details 不使用真实网络栈。测试用例预先设置期望的发送内容和对应的响应。
class MockTransport : public IXcpTransport {
public:
    MockTransport() = default;
    ~MockTransport() override;

    void open(IPacketListener& listener) override;
    void close() override;
    void send(BytesView packet) override;
    [[nodiscard]] bool isOpen() const noexcept override;

    // ---- 测试控制接口 ----

    /// @brief 设置下次 send 时的响应
    /// @param responseFunc 接收发送的 Packet，返回要回调的响应 Packet
    void setResponse(std::function<Bytes(BytesView)> responseFunc);

    /// @brief 主动注入异步 Packet（模拟 EV/SERV/DTO）
    void injectPacket(BytesView packet);

    /// @brief 主动注入 Transport 关闭事件
    void injectClose(std::string_view reason);

    /// @brief 主动注入 Transport 警告
    void injectWarning(std::string_view message);

    /// @brief 获取已发送的 Packet 列表
    [[nodiscard]] std::vector<Bytes> sentPackets() const;

private:
    IPacketListener* listener_ = nullptr;
    bool open_ = false;
    mutable std::mutex mutex_;
    std::function<Bytes(BytesView)> responseFunc_;
    std::vector<Bytes> sentPackets_;
};

}  // namespace calmcar::xcp::test
```

### 15.2 UdpTestSlave（tests/udp_test_slave.hpp）

> Loopback 测试 Slave，使用真实 UDP Socket，模拟最小 XCP 命令响应，支持故障注入。

```cpp
// 本代码段省略第 2.2 节规定的文件专属宏守卫，仅展示守卫内部内容。

#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <utility>
#include <span>
#include <memory>
#include <string>
#include "protocol_types.hpp"

namespace calmcar::xcp::test {

/// @brief 故障注入配置
struct FaultInjection {
    /// @brief 丢弃第 N 个响应（0 表示不丢弃）
    std::optional<std::size_t> dropResponseN;

    /// @brief 延迟第 N 个响应（毫秒）
    std::optional<std::pair<std::size_t, std::uint32_t>> delayResponseN;

    /// @brief 对第 N 个响应使用错误 LEN
    std::optional<std::size_t> corruptLenN;

    /// @brief 对第 N 个响应使用跳号 CTR
    std::optional<std::size_t> jumpCtrN;

    /// @brief 对第 N 个响应使用重复 CTR
    std::optional<std::size_t> duplicateCtrN;
};

/// @brief UDP 测试 Slave（Loopback）
/// @details 绑定 127.0.0.1 临时端口，模拟最小 XCP Session 和内存读取。
///          支持 CONNECT/DISCONNECT/GET_STATUS/GET_COMM_MODE_INFO/SET_MTA/UPLOAD/SHORT_UPLOAD。
///          按 XCP 1.1 Part 3：未连接时对 CONNECT 的来源 IP:port 应答；连接后仅接受
///          CONNECT 来源 IP 的命令（源端口可变化），所有响应仍发往原 CONNECT 来源 IP:port。
class UdpTestSlave {
public:
    /// @brief 构造并绑定 Loopback 临时端口
    /// @details 使用测试默认的 CONNECT 响应参数；需要可配置 Session 参数时，
    ///          在后续测试设施扩展中单独引入配置结构，避免本最小接口提前膨胀。
    UdpTestSlave();

    /// @brief 析构，自动停止
    ~UdpTestSlave();

    // ---- 控制 ----

    /// @brief 启动 Slave 接收线程
    void start();

    /// @brief 停止 Slave
    void stop();

    /// @brief 获取 Slave 绑定的端口（用于 Master 连接）
    [[nodiscard]] std::uint16_t port() const;

    // ---- 测试配置 ----

    /// @brief 设置模拟内存内容
    void setMemory(Address address, BytesView data);

    /// @brief 设置故障注入
    void setFaultInjection(const FaultInjection& fault);

    /// @brief 获取收到的命令计数
    [[nodiscard]] std::size_t commandCount() const;

    /// @brief 向已连接 Master 发送一个包含多个 XCP Frame 的 UDP Datagram
    /// @param xcpPackets 要按顺序打包的原始 XCP Packet 列表
    /// @throws XcpException(InvalidState) 尚未记录 CONNECT 来源端点
    /// @throws XcpException(InvalidArgument) Frame 或 Datagram 超过允许长度
    /// @details 仅用于验证 Master 接收端对 XCP 1.1 多 Frame UDP 打包的解析能力。
    void sendPackedFrames(std::span<const BytesView> xcpPackets);

private:
    /// @brief 接收线程主循环
    /// @details 对每个 UDP Datagram 按 LEN 解析全部完整 Frame，再依序调用 handleCommand()。
    void receiveLoop();

    /// @brief 处理收到的 UDP Datagram 中的一个 XCP Frame
    /// @param xcpPacket Frame 内的原始 XCP Packet
    /// @param sourceIp 发送方 IPv4 地址
    /// @param sourcePort 发送方 UDP 端口
    void handleCommand(BytesView xcpPacket,
                       const std::string& sourceIp,
                       std::uint16_t sourcePort);

    /// @brief 判断来源是否符合当前逻辑 XCP 会话
    /// @details 未连接时只允许 CONNECT 建立会话；连接后仅匹配 CONNECT 来源 IP，
    ///          不要求后续命令使用同一源端口。
    [[nodiscard]] bool isCurrentSessionSource(const std::string& sourceIp) const;

    /// @brief 发送响应到 CONNECT 时记录的来源 IP:port
    void sendResponse(BytesView xcpPacket);

    /// @brief 测试 Socket 私有实现声明
    struct SocketImpl;

    /// @brief Loopback UDP Socket 资源的拥有者
    std::unique_ptr<SocketImpl> socket_;

    /// @brief Socket 实际绑定的临时端口
    std::uint16_t port_ = 0;

    /// @brief 接收循环运行标记
    std::atomic<bool> running_{false};

    /// @brief 测试 Slave 接收线程
    std::thread receiveThread_;

    /// @brief 保护模拟内存的互斥量
    mutable std::mutex memoryMutex_;

    /// @brief 按起始地址存储的模拟 ECU 内存块
    std::map<Address, Bytes> memory_;

    /// @brief 保护逻辑 Session、MTA、CTR 和故障配置的互斥量
    std::mutex stateMutex_;

    /// @brief 是否已建立模拟 XCP Session
    bool connected_ = false;

    /// @brief CONNECT 报文来源 IPv4 地址
    std::optional<std::string> connectSourceIp_;

    /// @brief CONNECT 报文来源 UDP 端口，也是后续响应的固定目的端口
    std::optional<std::uint16_t> connectSourcePort_;

    /// @brief 当前模拟 MTA 的 32 位地址部分
    Address mta_ = 0;

    /// @brief 当前模拟 MTA 的地址扩展部分
    AddressExtension mtaExtension_ = 0;

    /// @brief 下一个 Slave→Master XCP Frame 使用的 CTR
    DatagramCtr sendCtr_ = 0;

    /// @brief 已处理的 XCP 命令总数
    std::size_t commandCount_ = 0;

    /// @brief 当前故障注入配置
    FaultInjection fault_;
};

}  // namespace calmcar::xcp::test
```

### 15.3 XCP 1.1 UDP 多 Frame 与端点绑定测试要求

1. `UdpHeaderCodec`：一个 Datagram 中连续编码两个 Frame 时，必须按 `Header_1 + Packet_1 + Header_2 + Packet_2` 顺序完整解析；任一 LEN 越界、截断 Header 或尾部残留字节，`decodeUdpDatagram()` 必须整体失败。
2. `UdpTransport`：收到多 Frame Datagram 时，必须按 Frame 顺序分别执行 CTR 检查并回调 `IPacketListener::onPacketReceived()`；发送方向仍验证“一次 `send()` 发送一个 Frame/Datagram”的项目策略。
3. `UdpTestSlave`：未连接时仅对 CONNECT 的来源 IP:port 回复；连接后从同一 IP 的不同源端口收到命令时应处理该命令，但响应目的地必须保持原 CONNECT 来源 IP:port；其他 IP 的命令必须忽略。
4. Loopback 测试必须覆盖：一个 Datagram 中的 `EV + RES`、两个连续 `RES/ERR` Frame、第二个 Frame LEN 损坏时整个 Datagram 不交付、以及 CTR 在同一 Datagram 内连续递增。
5. 测试不得把多 Frame 打包的 CTR 当作 Datagram 级别的单一计数器；每个 Frame 的 Header 都独立消耗一个 CTR 值。

---

## 16. 线程安全模型

### 16.1 线程分布

```text
用户线程                Transport 工作线程
    |                        |
    v                        |
XcpMaster::connect()         |
    |                        |
    v                        |
CommandExecutor::execute*()  |
    |                        |
    +-- transport_.send() -->|
    |                        |
    +-- waitForResponse()    |
    |   (阻塞在 cv)          |
    |                        v
    |                Transport 收到 Datagram
    |                        |
    |                IPacketListener::onPacketReceived()
    |                        |
    |                CommandExecutor::onPacketReceived()
    |                  (在工作线程执行)
    |                        |
    |                  加锁 mutex_
    |                  设置 pendingResponse_
    |                  notify cv
    |                        |
    v                        |
waitForResponse() 唤醒        |
    |                        |
    v                        |
解析响应，返回用户            |
```

### 16.2 同步规则

| 共享状态 | 保护机制 | 访问者 |
|---|---|---|
| `Session::state_` 等全部字段 | `Session::mutex_` | 用户线程 + Transport 线程 |
| `CommandExecutor::pendingResponse_` | `CommandExecutor::mutex_` + `responseCv_` | 用户线程写(发送) + Transport 线程写(回调) |
| `CommandExecutor::transportClosed_` | 同上 | 同上 |
| `UdpTransport::sendCtr_` | `atomic` 或 `sendMutex_` | 用户线程 |
| `UdpTransport::lastRecvCtr_` | `recvMutex_` | Transport 线程 |

### 16.3 死锁规避

- `CommandExecutor::onPacketReceived()` 在工作线程执行，**不得**在持锁状态下回调 `IEventListener`。
- 正确做法：先在锁内取出响应/事件，释放锁，再回调 `IEventListener`。
- `Session` 的锁是独立的，`CommandExecutor` 调用 `Session` 方法时不会持有自己的 `mutex_`。

### 16.4 关闭语义

- `UdpTransport::close()` 设置 `running_ = false`，关闭 Socket，`join()` 接收线程。
- 接收线程退出前调用 `listener_->onTransportClosed()`。
- `close()` 可在用户线程调用，与接收线程并发安全。
- `close()` 可重复调用（幂等）。

---

## 17. 字节布局参考表

### 17.1 命令 CTO 布局

| 命令 | Code | CTO 布局 | 长度 |
|---|---|---|---|
| CONNECT | 0xFF | `[FF][mode]` | 2 |
| DISCONNECT | 0xFE | `[FE][00]` | 2 |
| GET_STATUS | 0xFD | `[FD][00]` | 2 |
| SYNCH | 0xFC | `[FC][00]` | 2 |
| GET_COMM_MODE_INFO | 0xFB | `[FB][00]` | 2 |
| SET_MTA | 0xF6 | `[F6][00][ext][addr3][addr2][addr1][addr0]` | 8 |
| UPLOAD | 0xF5 | `[F5][n]` | 2 |
| SHORT_UPLOAD | 0xF4 | `[F4][n][00][ext][addr3][addr2][addr1][addr0]` | 8 |

> addr 字节序按 Session Byte Order（Intel 小端或 Motorola 大端）。

### 17.2 CONNECT 响应布局

```text
Byte 0: FF (RES)
Byte 1: RESOURCE
Byte 2: COMM_MODE_BASIC
Byte 3: MAX_CTO
Byte 4-5: MAX_DTO (按 Session Byte Order)
Byte 6: Protocol Layer Version
Byte 7: Transport Layer Version
```

### 17.3 GET_STATUS 响应布局

```text
Byte 0: FF (RES)
Byte 1: Current Session Status
Byte 2: Current Resource Protection Status
Byte 3: STATE_NUMBER
Byte 4-5: Session Configuration ID (按 Session Byte Order)
```

### 17.4 GET_COMM_MODE_INFO 响应布局

```text
Byte 0: FF (RES)
Byte 1: reserved
Byte 2: COMM_MODE_OPTIONAL
Byte 3: reserved
Byte 4: MAX_BS
Byte 5: MIN_ST
Byte 6: QUEUE_SIZE
Byte 7: XCP Driver Version (高 nibble=major, 低 nibble=minor)
```

### 17.5 UDP Datagram 与 XCP Frame 布局

一个 XCP on Ethernet Frame 的布局为：

```text
Frame Byte 0-1: LEN (u16le, 固定小端)
Frame Byte 2-3: CTR (u16le, 固定小端)
Frame Byte 4..:  XCP Packet (LEN 字节)
```

XCP 1.1 Part 3 允许一个 UDP Datagram 依次容纳一个或多个完整 Frame：

```text
UDP Datagram = Frame_1 || Frame_2 || ... || Frame_N
```

每个 Frame 都有独立的 LEN 和 CTR；任一 Frame 不得跨 UDP Datagram 边界。本项目发送方向为简单且确定的策略：一次 `IXcpTransport::send()` 生成一个 Frame 并独占一个 UDP Datagram；接收方向必须能解析连续打包的多个 Frame。

### 17.6 COMM_MODE_BASIC 位定义（待官方规范确认）

> **重要**：以下位定义是当前依据译文的推断。实际编码前必须对照 ASAM 官方规范确认。

| Bit | 含义 | 推断值 |
|---|---|---|
| 0 | BYTE_ORDER | 0=Intel, 1=Motorola |
| 1-2 | ADDRESS_GRANULARITY | 00=BYTE, 01=WORD, 10=DWORD |
| 6 | Slave Block Mode Supported | 0/1 |
| 7 | Optional Comm Mode Available | 0/1 |

示例 `0xC0` = `1100_0000`：
- bit0=0 → Intel
- bit1-2=00 → AG=BYTE
- bit6=1 → Slave Block Mode supported
- bit7=1 → Optional available

> **此推断与官方规范的吻合度待第 18 节开放问题解决后确认。**

---

## 18. 开放问题与后续确认

### 18.1 必须在编码前确认的问题

| 编号 | 问题 | 影响 | 当前状态 |
|---|---|---|---|
| Q1 | COMM_MODE_BASIC 的精确 bit 定义 | `parseConnectResponse` 实现 | 依据译文推断，需对照官方规范确认 |
| Q2 | CONNECT 响应中 MAX_DTO 是 1 字节还是 2 字节 | `ConnectResponse` 字段类型 | 示例 `FF 15 C0 08 08 00 10 10` 中 MAX_DTO=0x0008，看起来是 2 字节；但 MAX_CTO 后直接跟 MAX_DTO 两字节，再跟版本。需确认 |
| Q3 | SET_MTA 中 address 字段的确切字节序 | `encodeSetMta` 实现 | 译文说"按 Session Byte Order"，但 SET_MTA 在 CONNECT 之前不能用，所以 CONNECT 后字节序已确定。确认 SET_MTA 的 address 确实用 Session Byte Order |
| Q4 | SYNCH 命令是否需要特殊编码 | `encodeSynch` | 译文说 SYNCH 始终以 ERR_CMD_SYNCH 应答，命令本身格式 `[FC][00]`，需确认 |
| Q5 | CMake 构建系统和测试框架选择 | 工程基线 | 当前仓库无 CMakeLists.txt，需确认使用 GoogleTest 还是其他 |
| Q6 | `.clang-format` 文件是否存在 | 代码格式 | AGENTS.md 提到参考 .clang-format，需确认仓库中是否有此文件 |

### 18.2 设计中标记为"项目策略"的决策

| 编号 | 决策 | 依据 |
|---|---|---|
| D1 | CTR 初值为 0，open() 时复位 | 计划文档 4.7 |
| D2 | 原命令最多 2 次恢复重试 | 计划文档 6.2 |
| D3 | Master 侧默认严格匹配远端 IP+端口（可配置为仅匹配 IP） | 本项目安全策略；不等同于标准对 Slave 的连接绑定规则 |
| D4 | UDP 不重排、不重传 Frame | 计划文档 4.7 |
| D5 | 恰好相差 0x8000 的 CTR 视为歧义丢弃 | 计划文档 4.7 |
| D6 | 默认单 Frame Packet 上限为 65503，Datagram 上限为 65507 | IPv4 UDP 理论上限与 XCP Header 长度 |
| D7 | 发送方向每个 Datagram 只包含一个 Frame；接收方向支持多 Frame 打包 | XCP 1.1 Part 3 允许的子集发送策略与完整接收兼容性 |
| D8 | UdpTestSlave 连接后仅校验 CONNECT 来源 IP，响应始终发往原 CONNECT 来源 IP:port | XCP 1.1 Part 3 UDP/IP Connection Behavior |

### 18.3 可选增强（本阶段不实现，但接口预留）

- `IEventListener` 接口已定义，本阶段 DTO 只上报不解析。
- `UdpTransportConfig::strictRemotePort` 预留 NAT 兼容。
- `Session::fail()` 预留 Failed 状态。

---

## 附录 A：头文件依赖关系完整图

```text
protocol_types.hpp
  └─ (无依赖)

xcp_error.hpp
  └─ protocol_types.hpp

ixcp_transport.hpp
  ├─ protocol_types.hpp
  └─ xcp_error.hpp

udp_header_codec.hpp
  ├─ protocol_types.hpp
  └─ xcp_error.hpp

udp_transport_config.hpp
  ├─ protocol_types.hpp
  └─ udp_header_codec.hpp  (for kUdpMaxXcpPacket)

udp_transport.hpp
  ├─ ixcp_transport.hpp
  ├─ udp_transport_config.hpp
  └─ udp_header_codec.hpp

command_codec.hpp
  └─ protocol_types.hpp

response_parser.hpp
  └─ protocol_types.hpp

session.hpp
  ├─ protocol_types.hpp
  └─ xcp_error.hpp

command_executor.hpp
  ├─ ixcp_transport.hpp
  ├─ command_codec.hpp
  ├─ response_parser.hpp
  ├─ session.hpp
  └─ xcp_error.hpp

memory_access.hpp
  ├─ command_executor.hpp
  └─ session.hpp

xcp_master.hpp
  ├─ session.hpp
  ├─ command_executor.hpp
  ├─ memory_access.hpp
  ├─ ixcp_transport.hpp
  └─ xcp_error.hpp
```

## 附录 B：典型调用序列

### B.1 连接 + 读取 + 断开

```cpp
using namespace calmcar::xcp;

// 1. 创建 Transport
UdpTransportConfig cfg;
cfg.remoteHost = "127.0.0.1";
cfg.remotePort = slavePort;  // 从 UdpTestSlave 获取
cfg.localHost = "127.0.0.1";
auto transport = std::make_unique<UdpTransport>(std::move(cfg));

// 2. 创建 Master
XcpMaster master(std::move(transport));

// 3. 连接
master.connect();  // CONNECT → GET_COMM_MODE_INFO → GET_STATUS

// 4. 读取内存
auto data = master.readMemory(0x70012340, 0x00, 4);  // 读 4 字节

// 5. 断开
master.disconnect();
```

### B.2 Mock Transport 测试

```cpp
using namespace calmcar::xcp;
using namespace calmcar::xcp::test;

MockTransport transport;
transport.setResponse([](BytesView sent) -> Bytes {
    // 检查 sent 是 CONNECT 命令
    // 返回 CONNECT 响应
    return {0xFF, 0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10};
});

XcpMaster master(std::make_unique<MockTransport>(std::move(transport)));
master.connect();
// 断言 sessionParameters() 正确
```

---

> 本文档到此结束。所有接口定义均为设计阶段产物，未经编译验证。开始编码前需解决第 18 节中的开放问题。
