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
| `class` 成员变量 | `m_` + 小写蛇形 + 末尾 `_` | `m_remote_port_`、`m_recv_callback_`、`m_session_state_` |
| `struct` 成员变量 | 裸 `snake_case`，**不加** `m_` 前缀与末尾 `_` | `remote_host`、`error_code`、`max_cto`、`data` |
| 静态成员变量 | `s_` + 小写蛇形 + 末尾 `_` | `s_instance_count_` |
| 全局变量 | `g_` + 小写蛇形 + 末尾 `_` | `g_default_timeouts_` |
| 布尔变量 | 仍按所属作用域前缀，不额外添加 `b` | class：`m_connected_`；struct：`strict_remote_port`；局部：`is_open`、`has_pending_command` |
| 智能指针/容器 | 不添加类型缩写，仅遵循所属类别前缀 | class：`m_transport_`、`m_pending_response_`；struct：`additional_info`；局部：`xcp_packets` |

> 说明：本项目的"匈牙利命名"采用**作用域前缀形式**，且**按 `struct` / `class` 分流**：
> `class` 成员统一 `m_<snake_case>_`，`struct` 成员统一裸 `snake_case`（GoogleTest 夹具成员按 §2.2.1.1 豁免）。
> 不使用 `str`、`u16`、`p` 等类型前缀，避免类型变化导致名称失真；类型信息由 C++ 类型系统表达。
>
> 本文后续展示的接口原型均应在实现时按本节规则落地：函数名用大驼峰，参数与局部变量用小写蛇形，
> 成员变量按 struct / class 分流。协议报文中定义的字段名称仅在注释、报文图和标准术语说明中
> 保留原始大写拼写（如 `MAX_CTO`、`ERROR_CODE` 只出现在注释里，字段名写 `max_cto`、`error_code`）。

#### 2.2.1.1 struct 与 class 的判定依据（批次 4 裁决）

**按 `struct` / `class` 关键字判定，不按"是否有成员函数"或"是否纯数据"判定。**

| 要点 | 说明 |
|---|---|
| 为什么可以只看关键字 | 实测产品代码（`include/` + `src/`）的 struct 中**只有 `XcpAddress40` 一个带成员函数**（`XcpAddress40::Advance()`，`protocol_types.hpp`；同文件的 `ConnectResponse`、`GetStatusResponse`、`GetCommModeInfoResponse`、`SessionParameters` 均无成员函数）；`UdpFrame`、`UdpHeader`、`UdpFrameView` 的编解码是**自由函数**（`EncodeUdpFrame` / `DecodeUdpDatagram`，`udp_header_codec.hpp`），不是成员函数。`tests/` 里的 `Fixture`（`recovery_test.cpp`）、`Harness`（`memory_access_test.cpp`）、`Rig`（`xcp_master_integration_test.cpp`）确实也带成员函数，但其成员**本就是裸 `snake_case`**，按关键字判定与按语义判定结果一致。故两种判法在本项目结论相同，而只有前者可被自动校验——后者会永久留下需要人工判断的分歧面 |
| 有意接受的后果 | `XcpAddress40` 的成员函数照常 public，其字段仍按 struct 规则用裸名。C++ 中 `struct` 与 `class` 的差别本就是默认访问级别，以关键字作判据与语言语义一致 |
| `class` 侧不变 | 所有 `class`（接口类 `IXcpTransport`、产品类 `UdpTransport`/`CommandExecutor`/`Session`/`XcpException`/`MemoryAccess`/`XcpMaster`/`CommandCodec`/`ResponseParser`，以及测试类 `MockTransport`/`UdpTestSlave`/`FakeSlave`/`MockXcpSlave`/`EventRecorder`/`ScriptedSlave`/`RecordingEvents`/`TransportObserver`）成员一律 `m_<snake>_` |
| 唯一豁免 | **GoogleTest 夹具类的成员**用 `<snake>_`（仅尾下划线），例 `slave_`、`ep_`、`ctr_`、`res_`。理由：夹具成员在每个 `TEST_F` 体内被大量直接引用，`m_` 前缀显著降低可读性，而尾下划线已足以与局部变量区分。**该豁免只覆盖夹具类本身**，夹具内外定义在 `tests/` 的 helper class（如 `FakeSlave`、`MockXcpSlave`、`EventRecorder`）**不享有豁免**，仍须 `m_<snake>_` |
| 匿名 `struct` | 函数体内的匿名 `struct`（如参数化测试的 `Case`）成员按裸 `snake_case`，与 struct 规则一致 |
| `.cpp` 内私有 `struct`（PIMPL） | 同样按裸 `snake_case`（原批次 3 的"私有 PIMPL 豁免"已被 struct 主规则吸收，不再单列）。实例：`UdpTransport::SocketImpl::{winsock, handle, bound, remote}` |

#### 2.2.1.2 裁决历史（批次 3 → 批次 4，保留以便追溯）

> ⚠️ **以下批次 3 的裁决已被批次 4 推翻，仅作历史记录，不得再作为判据引用。**

批次 3 曾把"纯值语义聚合参数对象"与"`.cpp` 内私有 PIMPL"登记为 `m_<snake>_` 的**显式豁免**，
并写明一条"已知代价"：

> ~~`CommandTimeouts` 因此成为公开头文件 `include/libxcp/` 中**唯一**不遵循 `m_<snake>_`
> 的结构体（其余 7 个均遵循），造成公开 API 内部的命名不一致。~~

该断言**已被证伪**。批次 4 确立 struct/class 分流后，`include/libxcp/` 中**所有** struct
（`UdpTransportConfig`、`ConnectResponse`、`GetStatusResponse`、`GetCommModeInfoResponse`、
`SessionParameters`、`XcpAddress40`、`UdpHeader`、`UdpFrame`、`UdpFrameView`、
`PositiveResponse`、`NegativeResponse`、`EventPacket`、`ServicePacket`、`DtoPacket`、
`CommandTimeouts`）一律使用裸名：`CommandTimeouts` 不再是例外，而是**默认合规**。
故批次 4 撤销豁免表中"纯值聚合参数对象""私有 PIMPL"两行，只保留夹具成员豁免一行。

同时撤销批次 3 的裁决口径~~"以代码现状为准，改设计文档，不做任何批量重命名"~~：
批次 4 的口径改为**"以文档新规则为准，分批改造代码"**（代码侧待办见 §2.2.1.3）。

> 方法论教训（沿用批次 3）：文档中"某项已全面符合"的断言，必须由可复跑的扫描器给出命中数，
> 且扫描器要通过已知正例自检——"脚本跑通且零输出"不等于"检查通过"。
>
> ⚠️ **工具可得性说明（重要）**：本批次使用的审计脚本位于工作副本 `scripts/`，
> 而 `.gitignore` 已包含 `scripts` 条目，故**这些脚本不入库**——新克隆的仓库中它们不存在。
> 本节及 §2.2.1.3 给出的计数结论（文档侧 struct 64 字段全裸名、`m_<snake>_` 归零、
> class 侧 66 字段全 `m_<snake>_`）是**当次本地实测结果**，他人复核需按下列要点重建工具，
> 或改为逐条人工核对（**按符号名定位，不用行号**——行号会随每次格式化漂移，
> 本项目已在批次 5 实测到 4 处锚点各偏移 1 行、2 处偏移 2~4 行）：
> `protocol_types.hpp` 的 `ConnectResponse` / `GetStatusResponse` / `GetCommModeInfoResponse` /
> `XcpAddress40` / `SessionParameters`；`response_parser.hpp` 的 `PositiveResponse` /
> `NegativeResponse` / `EventPacket` / `ServicePacket` / `DtoPacket`；
> `udp_header_codec.hpp` 的 `UdpFrame` / `UdpHeader` / `UdpFrameView`；
> `udp_transport_config.hpp` 的 `UdpTransportConfig`）。
>
> | 本地脚本（不入库） | 用途 | 关键设计要求 |
> |---|---|---|
> | `scripts/tools_audit_member_naming.py` | struct/class 成员风格分布 | 作用域栈解析；**自检 A：9 条合成用例**（覆盖访问说明符粘连、预处理行粘连、`decltype` 类型、括号内默认参数、`const`/`override` 尾限定符、位域等正反例）+ **自检 B：语料锚点**；任一失败即 exit 1，结论作废 |
> | `scripts/tools_audit_rename_churn.py` | 改名引用面 / 跨类同名 / 新名撞名 | 一律由扫描器供数据，禁止手写字段清单 |
> | `scripts/tools_audit_diff.py` | 文档 vs 代码字段集合比对 | 按 `m_` 前后缀归一后比较，区分"仅命名差异"与"字段集合差异" |
> | `scripts/verify_rename_baseline.py` | **改动前快照 vs 现状逐条映射核对** | 必须自带**灵敏度自测**（注入 class 误改裸名 / struct 漏改 / 成员消失 / 新旧名并存 / 凭空新增，五种错误都要报出）；改名类改动的**最终裁判是它，不是改后重跑检测器**——因为改名脚本与检测器同源，盲区会同时污染两者 |

#### 2.2.1.3 落地状态核对（批次 4）

函数命名沿用批次 3 结论（已统一）；成员变量自批次 4 起按 **struct / class 分流**重新核对，
核对手段为可复跑扫描器 `tools_audit_member_naming.py`（内置"合成用例 + 语料锚点"两级自检，
任一失败即退出码 1、结论作废；该脚本按 §2.2.1.2 末"工具可得性说明"**不入库**，复核时需自备），
并以 `verify_rename_baseline.py` 对**改动前快照**逐条核对映射（见下方"检测器修正"）。

**设计文档侧（本文件内 ```cpp 原型）**

| 对象 | 数量 | 状态 |
|---|---|---|
| `struct` 成员 | 17 个 struct / 64 字段 | ✅ 全部裸 `snake_case`（批次 4：§3.9、§3.10、§3.11、§6、§12、§15.2 共 41 字段由 `m_<snake>_` 转入；§7、§10 的 21 字段在批次 4 前置改动中已完成；另补入 `raw_error_code` / `raw_event_code` 2 字段使 §10 与代码字段集合一致） |
| `class` 成员 | 10 个 class / 66 字段 | ✅ 全部 `m_<snake>_`（修正扫描器盲区后由 59 增至 66：`CommandCodec`、`ResponseParser` 各 1 条及 `XcpException`/`Session`/`XcpMaster`/`MemoryAccess` 各 1 条首成员曾被粘连吞掉） |
| 散文/表格/示例中的字段引用 | §7 配置项注释、§16.4 关闭语义、§18.2 D5、§18.3、附录 B.1 | ✅ 已同步为裸名（旧名残留会直接与新规则矛盾） |
| `IEventListener*` 构造参数 | §12、§14 | ✅ 参数本就 snake_case，合规（扫描器曾把多行参数续行误判为成员，已排除） |

**代码侧（`include/` `src/` `tests/`，批次 5 实施完毕）**

| 类别 | 范围 | 数量 | 状态 |
|---|---|---|---|
| A 冲突（原） | `UdpTransportConfig`、`PositiveResponse`、`NegativeResponse`、`EventPacket`、`ServicePacket`、`DtoPacket` | 18 字段 / 189 处引用 | ✅ 已转裸 `snake_case` |
| B 待办（原） | `ConnectResponse`、`GetStatusResponse`、`GetCommModeInfoResponse`、`SessionParameters`、`XcpAddress40`、`UdpHeader`、`UdpFrame`、`UdpFrameView`、`FaultInjection` | 39 字段 / 377 处引用 | ✅ 已转裸 `snake_case` |
| class 合规 | 产品 class：`CommandExecutor` 17、`UdpTransport` 10、`Session` 5、`XcpException` 5、`XcpMaster` 4、`MemoryAccess` 2、`CommandCodec` 1、`ResponseParser` 1 | 45 字段 | ✅ 全程保持 `m_<snake>_`（`CommandCodec`/`ResponseParser` 各 1 条曾被误改成裸名，见下方"检测器修正"，已回退） |
| class 修复 | 测试 helper class 补尾下划线：`FakeSlave` 13、`MockXcpSlave` 13、`ScriptedSlave` 6、`EventRecorder` 4×**两个同名类**、`TransportObserver` 4、`RecordingEvents` 3、`ThreadJoiner` 1×**两个同名类** | **47 条声明 / 44 个唯一 (类, 成员) 组合** | ✅ 已全部补为 `m_<snake>_`；差额 3 条来自跨文件同名类（`ThreadJoiner`、`EventRecorder` 的两个成员各出现两次） |
| 此前不可见的 class 成员 | `RawSender` 4、`RawEndpoint` 3、`UdpTestSlave` 16、`RecordingListener` 6 | 29 字段 | ✅ 本就合规，但**旧扫描器因盲区 ①③ 整条漏记**，修正后才进入统计 |
| 夹具豁免 | `UdpTestSlaveCommands` 的 `slave_`、`ep_`、`ctr_`、`res_` | 4 字段 | ✅ 按 §2.2.1.1 保持豁免，**未改** |
| 静态成员 | `WinsockSession` 的 `s_<snake>_` | 4 | ✅ 合规则未动（旧扫描器漏记 1 条，故先前记为 3） |

代码侧最终风格分布（**改前基线 230 条 = 改后 230 条**，成员总数不变，即改名过程无成员被吞掉或新增）：

```
                     改前(快照基线)   改后
class  m_snake_            85    ->    132   （补尾下划线 +47）
class  s_snake_             4    ->      4   （静态成员，合规未动）
class  snake_               4    ->      4   （夹具成员，§2.2.1.1 唯一豁免）
struct bare_snake          30    ->     90   （转裸名 +60）
struct m_snake_            60    ->      0   ← 归零
class  m_snake_noTail      47    ->      0   ← 归零
class  bare_snake           0    ->      0   （见下方"检测器修正"，曾一度为 2）
```

> ⚠️ **检测器自身的修正（本批次最重要的教训）**：上表的"改前/改后"是用**修正后**的扫描器
> 重测的。修正前的扫描器只报 207 条，**漏记 23 条成员声明**，源于四个盲区：
> ① `private:` / `public:` 后的**首条**成员声明与访问说明符粘连而被整条丢弃；
> ② `#if defined(_WIN32)` 的圆括号粘进紧随其后的声明，被误判为函数；
> ③ `decltype(::socket(...)) m_handle_{}` 这类"类型含括号"的数据成员被当成函数；
> ④ `#endif` 会产生一个名叫 `endif` 的假成员（多报）。
>
> 致命之处在于：**改名脚本与审计脚本共用同一个解析器**。因盲区 ① 从未进入"该标识符的全部持有者"
> 判定的 `class CommandCodec::m_byte_order_` 与 `class ResponseParser::m_byte_order_`，
> 被误判为"只属于待改名 struct"而连带改成裸名——声明与引用同步改掉，**编译器完全发现不了**。
> 修正办法不是只重跑改后的检测器，而是**引入改动前快照做基线**，逐条核对每个成员的前后映射
> （`verify_rename_baseline.py`，含 6 项灵敏度自测：注入"class 被改成裸名 / struct 漏改 /
> 本应保留的成员消失 / 新旧名并存 / 凭空新增"五种错误都必须被报出）。
> 该核对现已全绿：222 个唯一 (类别, 类型, 成员) 三元组，**每一条改动都落在规则允许的四种映射之内**。

文档↔代码字段一致性（`tools_audit_diff.py`）：**16 / 16 个两侧同名 struct 字段名完全一致，仅命名待改 0，字段集合真实差异 0**。`CommandResult` 仍仅存在于文档（§12 未实现）。

**验证**：Release 构建 exit 0 且无 error / 无 warning C；`ctest` **253/253 通过**（1 项按设计跳过）；`clang-format --dry-run --Werror` 全部 37 个源文件 **0 不合规**（改名改变行宽后曾致 15 个文件不合规，已用 `clang-format -i` 修复并重新构建 + 复测）；`verify_rename_baseline.py` 灵敏度自测 **6/6 通过**，222 个 (类别, 类型, 成员) 三元组**全部落在规则允许的四种映射之内**，违规成员 **0**。

> ⚠️ 本批次实际执行的**安全约束**（后续同类改动仍须遵守）：**禁止按字段名全局文本替换**。
> 实施时按"标识符的全部持有者是否都属于待改名 struct"分流：57 个标识符中判定 56 个 SAFE。
> **但该判定当时只有 55 个成立** —— `m_byte_order_` 同时是 `struct ConnectResponse`（待转裸名）与
> `class CommandCodec` / `class ResponseParser`（必须保持 `m_`）的成员，本应是第 2 个 CONFLICT；
> 因后两者的声明正好是 `private:` 后的首条而被当时共用的解析器漏记，"全部持有者"检查**空洞地通过**，
> 于是 class 侧被连带改成裸名。详见下方"检测器修正"。
> 由此得到一条更强的约束：**"全部持有者"判定的可信度等于解析器的完备度，而改名脚本与审计脚本
> 一旦同源，盲区会同时污染两者**；因此改名类改动的最终裁判必须是**改动前基线逐条映射核对**
> （`verify_rename_baseline.py`），而不是改后重跑同一个检测器。
> 当时被正确识别的 CONFLICT 是 `m_error_code_` —— 同时是 `struct NegativeResponse`（转裸名）与
> `class XcpException`（必须保持 `m_`）的成员。该标识符在 `include/`+`src/`+`tests/` 共 **13 处**出现，
> 逐点判定接收者类型后改掉 `NegativeResponse` 侧 **10 处**（分布在 9 行，其中
> `protocolMessage()` 内一行两处），保留 `XcpException` 侧 **3 处**
> （`xcp_error.hpp` 成员声明、`xcp_error.cpp` 构造初始化列表、`XcpException::GetErrorCode()`）。
> `m_data_` 虽分属 4 个 struct，但 4 者都是待改名 struct，故属 SAFE。
> 另需一道防呆：确认改名后**同一 struct 内不出现重名字段**（曾检出 `max_cto`/`max_dto`
> 与测试侧 `AgCase` 既有裸名字段趋同，因不同作用域而无害，但必须显式确认而非默认放行）。
> 统计、冲突检测与执行脚本均为本地工具（`tools_audit_rename_churn.py`、
> `rename_struct_fields.py`、`rename_class_members.py`，及收尾用的 `fix_batch5_regressions.py`；
> 三个一次性改写脚本已随改动完成删除），按 §2.2.1.2 末说明**不入库**。



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
/// @param first_byte 收到的 XCP Packet 首字节
/// @return PacketType 枚举值；若为 DAQ DTO 范围则返回 std::nullopt
/// @note 返回 nullopt 表示该包是 DAQ DTO，调用方应按 DTO 路径处理
std::optional<PacketType> ClassifyPacket(std::uint8_t first_byte);

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
std::string_view ErrorCodeName(ErrorCode code);

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
std::string_view EventCodeName(EventCode code);

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
constexpr std::uint8_t AgToBytes(AddressGranularity ag) noexcept;

/// @brief 将字节数转为 AG（仅接受 1/2/4）
std::optional<AddressGranularity> BytesToAg(std::uint8_t bytes) noexcept;

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
constexpr bool HasResource(ResourceMask mask, Resource res) noexcept;

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
std::string_view SessionStateName(SessionState state);

}  // namespace calmcar::xcp
```

### 3.9 CONNECT 响应参数结构体

```cpp
namespace calmcar::xcp {

/// @brief CONNECT 响应解析结果（COMM_MODE_BASIC 已拆解）
struct ConnectResponse {
    ResourceMask resource_mask;                    ///< RESOURCE 字段
    ByteOrder byte_order;                          ///< COMM_MODE_BASIC 中的字节序
    AddressGranularity address_granularity;        ///< COMM_MODE_BASIC 中的 AG
    bool slave_block_mode_supported;               ///< COMM_MODE_BASIC 中的 Block Mode 位
    bool optional_comm_mode_available;             ///< COMM_MODE_BASIC 中的 Optional 信息可用位
    std::uint8_t max_cto;                          ///< MAX_CTO（0x08..0xFF）
    std::uint16_t max_dto;                         ///< MAX_DTO（0x0008..0xFFFF）
    std::uint8_t protocol_layer_version;           ///< Protocol Layer 主版本
    std::uint8_t transport_layer_version;          ///< Transport Layer 主版本
};

/// @brief GET_STATUS 响应解析结果
struct GetStatusResponse {
    bool resume;                         ///< bit7
    bool daq_running;                    ///< bit6
    bool clear_daq_req;                  ///< bit3
    bool store_daq_req;                  ///< bit2
    bool store_cal_req;                  ///< bit0
    ResourceMask resource_protection;    ///< 当前资源保护状态
    std::uint8_t state_number;           ///< ECU State 编号
    std::uint16_t session_config_id;     ///< Session Configuration ID
};

/// @brief GET_COMM_MODE_INFO 响应解析结果
struct GetCommModeInfoResponse {
    std::uint8_t comm_mode_optional;       ///< Master Block Mode / Interleaved Mode 能力
    std::uint8_t max_bs;                    ///< Block Mode 最大块大小
    std::uint8_t min_st;                    ///< 最小分离时间（单位 100μs）
    std::uint8_t queue_size;                ///< Interleaved Mode 队列深度
    std::uint8_t driver_version_major;      ///< Driver Version 高 nibble
    std::uint8_t driver_version_minor;      ///< Driver Version 低 nibble
};

}  // namespace calmcar::xcp
```

### 3.10 40 位地址结构体

```cpp
namespace calmcar::xcp {

/// @brief XCP 40 位地址（32-bit Address + 8-bit Extension）
struct XcpAddress40 {
    Address address;                         ///< 32 位地址
    AddressExtension extension;              ///< 8 位地址扩展

    /// @brief 地址前进指定元素数（按 AG 换算为字节数）
    /// @param elements 前进的元素数
    /// @param address_granularity 地址粒度
    /// @return 前进后的新地址；溢出时返回 nullopt
    [[nodiscard]] std::optional<XcpAddress40> Advance(
        ElementCount elements,
        AddressGranularity address_granularity) const noexcept;
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
    ConnectResponse connect;                              ///< CONNECT 响应参数
    std::optional<GetCommModeInfoResponse> comm_mode_info;///< 仅在查询成功时存在
    std::optional<GetStatusResponse> status;              ///< 仅在查询成功时存在
    bool short_upload_available = true;                   ///< SHORT_UPLOAD 是否可用
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
std::string_view ErrorCategoryName(ErrorCategory cat);

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
    /// @param command_code 相关命令码（可选，用于诊断）
    /// @param error_code 协议错误码（仅 ProtocolError 时有效）
    /// @param retry_count 恢复重试次数（仅恢复场景有效）
    /// @param transport_error 底层 Transport 错误描述（可选）
    XcpException(ErrorCategory category,
                 std::string message,
                 std::optional<CommandCode> command_code = std::nullopt,
                 std::optional<ErrorCode> error_code = std::nullopt,
                 int retry_count = 0,
                 std::string transport_error = "");

    /// @brief 获取错误分类
    [[nodiscard]] ErrorCategory Category() const noexcept;

    /// @brief 获取相关命令码
    [[nodiscard]] std::optional<CommandCode> CommandCode() const noexcept;

    /// @brief 获取协议错误码（仅 ProtocolError）
    [[nodiscard]] std::optional<ErrorCode> ErrorCode() const noexcept;

    /// @brief 获取恢复重试次数
    [[nodiscard]] int RetryCount() const noexcept;

    /// @brief 获取底层 Transport 错误描述
    [[nodiscard]] std::string_view TransportError() const noexcept;

private:
    ErrorCategory m_category_;
    std::optional<CommandCode> m_command_code_;
    std::optional<ErrorCode> m_error_code_;
    int m_retry_count_;
    std::string m_transport_error_;
};

}  // namespace calmcar::xcp
```

### 4.3 便捷构造函数（可选，内部使用）

```cpp
namespace calmcar::xcp::detail {

/// @brief 构造 InvalidArgument 异常
[[nodiscard]] XcpException MakeInvalidArgument(std::string msg);

/// @brief 构造 InvalidState 异常
[[nodiscard]] XcpException MakeInvalidState(std::string msg);

/// @brief 构造 TransportError 异常
[[nodiscard]] XcpException MakeTransportError(std::string msg, std::string transport_detail = "");

/// @brief 构造 Timeout 异常
[[nodiscard]] XcpException MakeTimeout(std::string msg, std::optional<CommandCode> cmd, int retry);

/// @brief 构造 MalformedPacket 异常
[[nodiscard]] XcpException MakeMalformedPacket(std::string msg);

/// @brief 构造 ProtocolError 异常
[[nodiscard]] XcpException MakeProtocolError(std::string msg,
                                              CommandCode cmd,
                                              ErrorCode code);

/// @brief 构造 UnsupportedFeature 异常
[[nodiscard]] XcpException MakeUnsupportedFeature(std::string msg);

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
    virtual void OnPacketReceived(BytesView packet) = 0;

    /// @brief Transport 通道已关闭（正常关闭或错误关闭）
    /// @param reason 关闭原因描述
    /// @note 此方法在 Transport 工作线程调用；调用后不再有 OnPacketReceived
    virtual void OnTransportClosed(std::string_view reason) = 0;

    /// @brief Transport 发生可恢复错误（如畸形 Datagram 丢弃）
    /// @param message 错误描述
    /// @note 此方法在 Transport 工作线程调用；Transport 不会因此关闭
    virtual void OnTransportWarning(std::string_view message) = 0;
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
    virtual void Open(IPacketListener& listener) = 0;

    /// @brief 关闭 Transport 通道
    /// @note 关闭后不再调用 listener 回调；可安全重复调用
    virtual void Close() = 0;

    /// @brief 发送一个完整的 XCP CTO Packet
    /// @param packet 完整 XCP Packet 字节（不含 Transport Header）
    /// @throws XcpException(TransportError) 发送失败
    /// @note 此方法可在任意线程调用；实现需保证线程安全
    virtual void Send(BytesView packet) = 0;

    /// @brief Transport 是否已打开
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
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
/// @note xcp_packet 的生命周期不超过传入 DecodeUdpDatagram() 的字节视图。
struct UdpFrameView {
    UdpHeader header;
    BytesView xcp_packet;
};

/// @brief 编码一个 XCP on Ethernet Frame
/// @param xcp_packet 原始 XCP Packet（不含 Transport Header）
/// @param ctr 该 Frame 的发送计数器值
/// @return 编码后的 Frame
/// @throws XcpException(InvalidArgument) xcp_packet.size() > kUdpMaxXcpPacket (65503)
[[nodiscard]] UdpFrame EncodeUdpFrame(BytesView xcp_packet, DatagramCtr ctr);

/// @brief 解码一个 UDP Datagram 中连续打包的全部 XCP Frame
/// @param datagram 完整 UDP Payload
/// @return 全部 Frame 视图；空 Datagram、Header 不完整、LEN 为 0、LEN 越界或末尾残留字节时返回 nullopt
/// @note XCP 1.1 Part 3 允许一个 UDP Datagram 包含多个完整 Frame，
///       但任何单个 Frame 都不得跨 Datagram 边界。
[[nodiscard]] std::optional<std::vector<UdpFrameView>> DecodeUdpDatagram(BytesView datagram) noexcept;

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
    std::string remote_host;

    /// @brief 远端 Slave UDP 业务端口
    std::uint16_t remote_port = 0;

    /// @brief 本地绑定 IPv4 地址（默认 "0.0.0.0"，Loopback 测试用 "127.0.0.1"）
    std::string local_host = "0.0.0.0";

    /// @brief 本地绑定端口（0 表示由 OS 分配临时端口）
    std::uint16_t local_port = 0;

    /// @brief 接收超时（毫秒），0 表示阻塞接收
    ///        实际用于内部线程的周期性检查，不影响上层命令超时
    std::uint32_t receive_poll_interval_ms = 100;

    /// @brief 最大允许的单个 XCP Frame 内原始 Packet 长度（字节）
    ///        默认 kUdpMaxXcpPacket (65503)；可设更小值以避免 IP 分片
    std::size_t max_frame_packet_size = kUdpMaxXcpPacket;

    /// @brief 最大允许的 UDP Datagram Payload 长度（字节）
    ///        默认 kUdpMaxDatagramSize (65507)，限制连续打包 Frame 的总长度
    std::size_t max_datagram_size = kUdpMaxDatagramSize;

    /// @brief 是否严格匹配远端端口（true 时要求收到的包来自 remote_port）
    /// @details 这是 Master 侧的项目安全策略，不是 XCP 1.1 Part 3 对 Slave
    ///          连接绑定规则的复刻；设 false 时仅匹配 remote_host。
    bool strict_remote_port = true;
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
    void Open(IPacketListener& listener) override;

    /// @brief 关闭 Transport 通道
    void Close() override;

    /// @brief 发送一个完整的 XCP CTO Packet
    /// @throws XcpException(TransportError) 发送失败
    void Send(BytesView packet) override;

    /// @brief Transport 是否已打开
    [[nodiscard]] bool IsOpen() const noexcept override;

    /// @brief 获取当前发送方向 CTR 值（诊断用）
    [[nodiscard]] DatagramCtr SendCtr() const noexcept;

    /// @brief 获取最近收到的接收方向 CTR 值（诊断用）
    [[nodiscard]] std::optional<DatagramCtr> LastReceiveCtr() const noexcept;

private:
    /// @brief 接收线程主循环
    void ReceiveLoop();

    /// @brief 处理收到的 UDP Datagram
    /// @details 先校验来源和 Datagram 长度，再解码其中连续打包的全部 Frame；
    ///          每个通过 CTR 校验的 Frame 分别回调给 IPacketListener。
    /// @return true 表示已正常处理；false 表示需要退出循环
    bool HandleDatagram(const std::uint8_t* data, std::size_t size,
                         const std::string& src_ip, std::uint16_t src_port);

    /// @brief 处理 Datagram 内的单个 Frame
    /// @param frame 已完成 LEN 边界校验的 Frame 视图
    /// @return true 表示 Frame 已交付上层；false 表示因 CTR 规则丢弃
    bool HandleFrame(const UdpFrameView& frame);

    /// @brief 检查源地址是否匹配配置的远端
    bool IsRemoteMatch(const std::string& src_ip, std::uint16_t src_port) const;

    /// @brief 打开 Transport 时固定的 UDP 参数
    UdpTransportConfig m_config_;

    /// @brief 回调接收完整 XCP Packet 的监听器（非拥有）
    IPacketListener* m_listener_ = nullptr;

    /// @brief 平台 Socket 资源的私有实现声明
    struct SocketImpl;

    /// @brief 平台 UDP Socket 资源的拥有者
    std::unique_ptr<SocketImpl> m_socket_;

    /// @brief Transport 接收工作线程
    std::thread m_receive_thread_;

    /// @brief 接收循环运行标记
    std::atomic<bool> m_running_{false};

    /// @brief 保护发送、Frame 编码与 CTR 递增的互斥量
    mutable std::mutex m_send_mutex_;

    /// @brief 下一个 Master→Slave XCP Frame 使用的 CTR
    std::atomic<DatagramCtr> m_send_ctr_{0};

    /// @brief 保护接收 CTR 基线和最近值的互斥量
    mutable std::mutex m_recv_mutex_;

    /// @brief 最近接收并接受的 Slave→Master Frame CTR
    std::optional<DatagramCtr> m_last_recv_ctr_;

    /// @brief 是否已由首个合法接收 Frame 建立 CTR 基线
    bool m_recv_baseline_established_ = false;
};

}  // namespace calmcar::xcp
```

### 8.1 接收 CTR 策略说明（实现细节，写入 .cpp 注释）

```
接收策略（与 XCP 1.1 Part 3 及项目策略一致）：
1. 一个 UDP Datagram 可按 LEN 顺序解出多个完整 Frame；每个 Frame 均独立执行 CTR 检查。
2. 首个合法 Frame 建立 m_last_recv_ctr_ 基线，接收并推进。
3. CTR == 期望值（m_last_recv_ctr_ + 1 mod 65536）→ 接收，推进。
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
    /// @param byte_order Session 字节序（CONNECT 后确定）
    explicit CommandCodec(ByteOrder byte_order) noexcept;

    // ---- 命令编码 ----

    /// @brief 编码 CONNECT 命令
    /// @param mode 0x00=普通, 0x01=用户自定义
    /// @return CTO: [0xFF][mode]
    [[nodiscard]] Bytes EncodeConnect(std::uint8_t mode = 0x00) const;

    /// @brief 编码 DISCONNECT 命令
    /// @return CTO: [0xFE][0x00]
    [[nodiscard]] Bytes EncodeDisconnect() const;

    /// @brief 编码 GET_STATUS 命令
    /// @return CTO: [0xFD][0x00]
    [[nodiscard]] Bytes EncodeGetStatus() const;

    /// @brief 编码 SYNCH 命令
    /// @return CTO: [0xFC][0x00]
    [[nodiscard]] Bytes EncodeSynch() const;

    /// @brief 编码 GET_COMM_MODE_INFO 命令
    /// @return CTO: [0xFB][0x00]
    [[nodiscard]] Bytes EncodeGetCommModeInfo() const;

    /// @brief 编码 SET_MTA 命令
    /// @param extension 地址扩展
    /// @param address 32 位地址
    /// @return CTO: [0xF6][reserved][extension][addr_b0..b3]
    [[nodiscard]] Bytes EncodeSetMta(AddressExtension extension, Address address) const;

    /// @brief 编码 UPLOAD 命令
    /// @param number_of_elements 要读取的元素数（以 AG 为单位）
    /// @return CTO: [0xF5][number_of_elements]
    [[nodiscard]] Bytes EncodeUpload(ElementCount number_of_elements) const;

    /// @brief 编码 SHORT_UPLOAD 命令
    /// @param number_of_elements 要读取的元素数
    /// @param extension 地址扩展
    /// @param address 32 位地址
    /// @return CTO: [0xF4][number_of_elements][reserved][extension][addr_b0..b3]
    [[nodiscard]] Bytes EncodeShortUpload(ElementCount number_of_elements,
                                           AddressExtension extension,
                                           Address address) const;

private:
    ByteOrder m_byte_order_;

    /// @brief 按字节序写入 16 位值到缓冲区
    void WriteU16(Bytes& buf, std::uint16_t val) const;

    /// @brief 按字节序写入 32 位值到缓冲区
    void WriteU32(Bytes& buf, std::uint32_t val) const;
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
    CommandCode command{};  ///< 对应的命令码（由调用方传入的期望命令）
    Bytes data;             ///< RES 后的数据（不含 0xFF 前缀）
};

/// @brief Negative Response 内容
struct NegativeResponse {
    std::uint8_t raw_error_code{0};       ///< ERR Packet Byte 1 原始值（保留 Slave 返回的未知码）
    std::optional<ErrorCode> error_code;  ///< 已识别的错误码；未知厂商码为 std::nullopt
    Bytes additional_info;                ///< 可选附加信息（Byte 2..）
};

/// @brief Event Packet 内容
struct EventPacket {
    std::uint8_t raw_event_code{0};          ///< EV Packet Byte 1 原始值（保留未知码）
    std::optional<EventCode> event_code;     ///< 已识别的事件码；未知值为 std::nullopt
    Bytes info;                              ///< 可选 Event 信息（Byte 2..）
};

/// @brief Service Request Packet 内容
struct ServicePacket {
    std::uint8_t service_code;     ///< SERV Packet 的 Byte 1
    Bytes data;                    ///< 可选 Service 数据
};

/// @brief DTO Packet（本阶段仅识别，不解析内容）
struct DtoPacket {
    std::uint8_t pid;              ///< 原始 PID（0x00..0xFB）
    Bytes data;                    ///< DTO 数据
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
    /// @param ByteOrder Session 字节序
    explicit ResponseParser(ByteOrder byte_order) noexcept;

    /// @brief 解析一个完整的 XCP Packet
    /// @param packet 完整 XCP Packet 字节
    /// @param expected_command 调用方期望的命令码（用于 PositiveResponse.command）
    /// @return 解析结果；畸形包返回 nullopt
    [[nodiscard]] std::optional<ParsedPacket> Parse(BytesView packet,
                                                      CommandCode expected_command) const;

    // ---- 专用解析方法 ----

    /// @brief 解析 CONNECT 响应
    /// @param res_data RES 后的数据（不含 0xFF 前缀）
    /// @return 解析结果；格式非法返回 nullopt
    [[nodiscard]] std::optional<ConnectResponse> ParseConnectResponse(BytesView res_data) const;

    /// @brief 解析 GET_STATUS 响应
    [[nodiscard]] std::optional<GetStatusResponse> ParseGetStatusResponse(BytesView res_data) const;

    /// @brief 解析 GET_COMM_MODE_INFO 响应
    [[nodiscard]] std::optional<GetCommModeInfoResponse> ParseGetCommModeInfoResponse(BytesView res_data) const;

private:
    ByteOrder m_byte_order_;

    /// @brief 按字节序读取 16 位值；越界返回 nullopt
    /// @note 批次 1 修正：原设计返回裸 `std::uint16_t`，但越界时无合法返回值可用，
    ///       返回 `std::optional` 才能真正保证"畸形包不越界读"。
    [[nodiscard]] std::optional<std::uint16_t> ReadU16(
        BytesView data, std::size_t offset) const;

    /// @brief 读取单字节；越界返回 nullopt（批次 1 新增）
    [[nodiscard]] static std::optional<std::uint8_t> ReadU8(
        BytesView data, std::size_t offset) noexcept;

    /// @brief 按字节序读取 32 位值
    /// @note 批次 3 说明：本最小子集需要解析的多字节字段（MAX_DTO、Session
    ///       Configuration ID）均为 WORD，`ReadU32()` 无调用点，故**未实现**。
    ///       需要时按上面 `ReadU16()` 的模式（返回 `std::optional`、越界安全）补充。
    // [[nodiscard]] std::optional<std::uint32_t> ReadU32(
    //     BytesView data, std::size_t offset) const;
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
    [[nodiscard]] SessionState State() const;

    /// @brief 是否已连接（Connected 状态）
    [[nodiscard]] bool IsConnected() const;

    /// @brief 是否有等待响应的命令（Outstanding Command）
    [[nodiscard]] bool HasPendingCommand() const;

    // ---- 状态迁移 ----

    /// @brief 进入 Connecting 状态
    /// @throws XcpException(InvalidState) 当前状态不允许连接
    void BeginConnecting();

    /// @brief CONNECT 成功，保存参数并进入 Connected
    /// @throws XcpException(InvalidArgument) 参数校验失败
    void EstablishConnection(const ConnectResponse& connect_response);

    /// @brief 进入 Disconnecting 状态
    /// @throws XcpException(InvalidState) 当前状态不允许断开
    void BeginDisconnecting();

    /// @brief DISCONNECT 成功，进入 Disconnected
    void CompleteDisconnection();

    /// @brief 进入 Recovering 状态
    /// @throws XcpException(InvalidState) 当前状态不允许恢复
    void BeginRecovery();

    /// @brief 恢复成功，回到 Connected
    void CompleteRecovery();

    /// @brief 标记 Session 为 Failed
    /// @param reason 失败原因
    void Fail(std::string_view reason);

    /**
     * @brief 最近一次 Fail() 的原因文本（批次 3 补齐到文档，代码早已实现）
     * @return 失败原因；未发生过 Fail() 时为空串
     * @details 供上层在 `SessionState::Failed` 下向用户展示诊断信息。
     *          `Reset()` / `BeginConnecting()` 会清空该原因。
     */
    [[nodiscard]] std::string FailReason() const;

    /// @brief 重置到 Disconnected（用于强制清理）
    void Reset();

    // ---- Outstanding Command 管理 ----

    /// @brief 标记命令已发送，等待响应
    /// @throws XcpException(InvalidState) 已有 Pending Command
    void MarkCommandSent(CommandCode cmd);

    /// @brief 标记命令响应已收到
    void ClearPendingCommand();

    /// @brief 获取当前 Pending 命令码
    [[nodiscard]] std::optional<CommandCode> PendingCommand() const;

    // ---- 参数访问 ----

    /// @brief 获取 Session 参数快照
    [[nodiscard]] SessionParameters Parameters() const;

    /// @brief 更新 GET_COMM_MODE_INFO 结果
    void UpdateCommModeInfo(const GetCommModeInfoResponse& info);

    /// @brief 更新 GET_STATUS 结果
    void UpdateStatus(const GetStatusResponse& status);

    /// @brief 标记 SHORT_UPLOAD 不可用（降级）
    void DisableShortUpload();

    /// @brief 获取当前 Byte Order
    [[nodiscard]] ByteOrder GetByteOrder() const;

    /// @brief 获取当前 AG
    [[nodiscard]] AddressGranularity GetAddressGranularity() const;

    /// @brief 获取 MAX_CTO
    [[nodiscard]] std::uint8_t MaxCto() const;

    /// @brief 获取 MAX_DTO
    [[nodiscard]] std::uint16_t MaxDto() const;

private:
    mutable std::mutex m_mutex_;
    SessionState m_state_ = SessionState::Disconnected;
    SessionParameters m_params_;
    std::optional<CommandCode> m_pending_command_;
    std::string m_fail_reason_;

    /// @brief 校验 CONNECT 参数
    /// @throws XcpException(InvalidArgument) 参数非法
    void ValidateConnectParams(const ConnectResponse& resp) const;
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
/// @note 批次 3 裁决：本结构的字段名**保持代码现状**（裸 snake_case），不改名为
///       `m_<snake>_`。理由与影响见 §2.2.1.2：它是纯值语义的聚合参数对象、无成员函数，
///       字段以 `CommandTimeouts{std::chrono::milliseconds(400), ..., 2}` 位置初始化方式使用。
///       ⚠️ 代价：它是公开头文件中唯一不遵循 §2.2.1 成员规则的结构体（其余 7 个均为
///       `m_<snake>_`），该不一致为**已知并接受**。本节字段名以代码为准。
struct CommandTimeouts {
    /// @brief 普通命令超时（毫秒）
    std::chrono::milliseconds command_timeout{1000};

    /// @brief SYNCH 恢复超时（毫秒）
    std::chrono::milliseconds synch_timeout{1000};

    /// @brief 最大恢复重试次数（不含首次尝试，设计决策 D2）
    int max_retries{2};
};

/// @brief 命令执行结果
/// @note 批次 3 说明：`CommandResult` **未实现**。实现中 `Execute*()` 系列直接返回
///       已解析的具体响应类型（如 `ConnectResponse`、`GetStatusResponse`）或
///       `Bytes`，`RunCommand()` 内部返回 `ParsedPacket`。引入仅含单个字段的
///       `CommandResult` 包装并不会带来额外信息，故不落地。
struct CommandResult {
    ParsedPacket response;  ///< 最终收到的响应（RES 或 ERR）
};

/// @brief 事件观察者接口（可选，用于上层接收异步 Event）
/// @note 回调在 Transport 工作线程执行；实现者不得在其中阻塞等待命令响应，
///       否则会与单 Outstanding Command 模型自锁（设计 §16.3）。
class IEventListener {
public:
    virtual ~IEventListener() = default;

    /// @brief 收到异步 Event
    virtual void OnEvent(const EventPacket& event) = 0;

    /// @brief 收到异步 Service Request
    virtual void OnService(const ServicePacket& service) = 0;

    /// @brief 收到 DTO（本阶段仅识别）
    virtual void OnDto(const DtoPacket& dto) = 0;
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
    /// @param event_listener 事件监听器（可选，可为 nullptr）
    CommandExecutor(IXcpTransport& transport,
                    Session& session,
                    CommandTimeouts timeouts = {},
                    IEventListener* event_listener = nullptr);

    /// @brief 析构
    ~CommandExecutor() override;

    // ---- IPacketListener 实现 ----

    void OnPacketReceived(BytesView packet) override;
    void OnTransportClosed(std::string_view reason) override;
    void OnTransportWarning(std::string_view message) override;

    // ---- 命令执行 ----

    /// @brief 执行 CONNECT 命令
    /// @param mode 0x00=普通, 0x01=用户自定义
    /// @return CONNECT 响应解析结果
    /// @throws XcpException 超时、协议错误或恢复失败
    [[nodiscard]] ConnectResponse ExecuteConnect(std::uint8_t mode = 0x00);

    /// @brief 执行 DISCONNECT 命令
    /// @throws XcpException 超时或协议错误
    void ExecuteDisconnect();

    /// @brief 执行 GET_STATUS 命令
    /// @return GET_STATUS 响应解析结果
    [[nodiscard]] GetStatusResponse ExecuteGetStatus();

    /// @brief 执行 GET_COMM_MODE_INFO 命令
    /// @return 响应结果；若 Slave 返回 ERR_CMD_UNKNOWN 则返回 nullopt
    [[nodiscard]] std::optional<GetCommModeInfoResponse> ExecuteGetCommModeInfo();

    /// @brief 执行 SET_MTA 命令
    /// @param extension 地址扩展
    /// @param address 32 位地址
    void ExecuteSetMta(AddressExtension extension, Address address);

    /// @brief 执行 UPLOAD 命令
    /// @param number_of_elements 元素数
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes ExecuteUpload(ElementCount number_of_elements);

    /// @brief 执行 SHORT_UPLOAD 命令
    /// @param number_of_elements 元素数
    /// @param extension 地址扩展
    /// @param address 32 位地址
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes ExecuteShortUpload(ElementCount number_of_elements,
                                            AddressExtension extension,
                                            Address address);

    /// @brief 发送 SYNCH（用于恢复，通常不直接调用）
    /// @return true 表示收到 ERR_CMD_SYNCH（恢复成功）
    bool SendSynch();

private:
    /// @brief 执行单条命令的通用流程（编码→发送→等待→解析）
    /// @param cmd 命令码
    /// @param encoded_packet 已编码的 CTO
    /// @return 解析后的响应
    /// @throws XcpException 超时或协议错误
    /// @note 实现命名为 `RunCommand()`（批次 1 落地时定名），语义与本节所述流程一致。
    [[nodiscard]] ParsedPacket RunCommand(CommandCode cmd, const Bytes& encoded_packet);

    /// @brief 单次发送尝试：发送 + 等待最终响应（不含恢复重试）
    /// @return 响应；超时或 Transport 关闭返回 nullopt
    /// @note 批次 2 修正：原实现把该逻辑内联在 `RunCommand()` 中，导致 Pending 清理
    ///       路径不一致；现抽出为独立函数并统一清理 Pending。
    [[nodiscard]] std::optional<ParsedPacket> PerformAttempt(
        CommandCode cmd, BytesView encoded_packet);

    /// @brief 等待当前 Pending Command 的响应
    /// @param timeout 超时时长
    /// @return 收到的响应；超时返回 nullopt
    [[nodiscard]] std::optional<ParsedPacket> WaitForResponse(std::chrono::milliseconds timeout);

    /// @brief 按 ErrorCode 分派 RES/ERR（含计划 §6.4 错误策略）
    [[nodiscard]] ParsedPacket DispatchResponse(CommandCode cmd,
                                                ParsedPacket response);

    /// @brief 超时恢复流程：SYNCH → 等待 ERR_CMD_SYNCH → 恢复隐含状态
    /// @param cmd 原命令码
    /// @throws XcpException 恢复失败
    void PerformRecovery(CommandCode cmd);

    /// @brief UPLOAD 重试前重建 MTA（计划 §6.2 第 3 条）
    void RestoreUploadMta();

    /// @brief 按 Session 字节序按需创建 Codec/Parser
    void EnsureCodec(ByteOrder byte_order);

    /// @brief 校验 RES 数据长度是否等于 elements*AG
    void CheckResLength(CommandCode cmd, const PositiveResponse& res,
                        ElementCount elements);

    // 依赖
    IXcpTransport& m_transport_;
    Session& m_session_;
    CommandTimeouts m_timeouts_;
    IEventListener* m_event_listener_;

    // Codec/Parser（在 Session 字节序确定后创建）
    std::optional<CommandCodec> m_codec_;
    std::optional<ResponseParser> m_parser_;

    // 同步
    mutable std::mutex m_mutex_;
    std::condition_variable m_response_cv_;
    std::condition_variable m_synch_cv_;
    std::optional<ParsedPacket> m_pending_response_;
    bool m_response_ready_ = false;
    bool m_synch_confirmed_ = false;
    bool m_in_recovery_ = false;
    bool m_transport_closed_ = false;
    std::string m_transport_close_reason_;

    // EV_CMD_PENDING 计时
    bool m_cmd_pending_received_ = false;

    // 隐含状态恢复（批次 2 新增）
    std::optional<XcpAddress40> m_last_mta_;
};

}  // namespace calmcar::xcp
```

### 12.1 RunCommand 内部流程

```text
RunCommand(cmd, encoded_packet):
  0. 若已有 Pending Command → throw InvalidState（拒绝并发发送）
  1. PerformAttempt(cmd, encoded_packet):
       m_session_.MarkCommandSent(cmd)     // 检查无 Pending，设置 Pending
       m_transport_.Send(encoded_packet)   // 失败则清理 Pending 后抛出
       WaitForResponse(m_timeouts_.command_timeout) // 阻塞在 m_response_cv_
       m_session_.ClearPendingCommand()
       → nullopt 表示超时或 Transport 已关闭
  2. OnPacketReceived 回调（Transport 工作线程）：
       * RES/ERR      → 填充 m_pending_response_，唤醒 WaitForResponse
                        槽位已占用则丢弃并上报畸形（绝不覆盖，批次 2 修正）
       * ERR_CMD_SYNCH 且 m_in_recovery_ → 置 m_synch_confirmed_，仅此场景视为成功
       * EV(CMD_PENDING) → 重启计时标记；**事件仍照常上报**（批次 2 修正）
       * EV(其他)     → event_listener->OnEvent()，不唤醒
       * SERV        → event_listener->OnService()
       * DTO         → event_listener->OnDto()
       所有 listener 回调均在**释放 m_mutex_ 之后**执行（设计 §16.3 死锁规避）
  3. 若 nullopt（超时）且重试未耗尽:
       PerformRecovery(cmd)               // Transport 已关闭则不执行 SYNCH
         → SendSynch() + 等待 ERR_CMD_SYNCH（m_timeouts_.synch_timeout）
         → 成功则 m_session_.CompleteRecovery()，失败则 m_session_.Fail()
       若原命令为 UPLOAD → RestoreUploadMta()  // 批次 2 修正：重试前必须重发 SET_MTA
       重试原命令，最多 m_timeouts_.max_retries 次（不含首次）
  4. DispatchResponse(cmd, response):
       - RES → 校验长度后返回 PositiveResponse
       - ERR_CMD_SYNCH 仅在 Recovery 中视为成功
       - ERR_ACCESS_LOCKED → throw UnsupportedFeature
       - ERR_CMD_UNKNOWN(GET_COMM_MODE_INFO/SHORT_UPLOAD) → 返回给调用方降级
       - 其他 → throw ProtocolError（未知错误码保留原始十六进制值）
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
    /// @param element_count 元素数（以 AG 为单位）
    /// @return 读取到的原始字节
    /// @throws XcpException(InvalidArgument) 参数非法或地址溢出
    /// @throws XcpException 协议错误或超时
    [[nodiscard]] Bytes ReadElements(Address address,
                                      AddressExtension extension,
                                      ElementCount element_count);

    /// @brief 读取内存（以字节为单位）
    /// @param address 32 位地址
    /// @param extension 地址扩展
    /// @param byte_count 字节数（必须可被 AG 整除）
    /// @return 读取到的原始字节
    /// @throws XcpException(InvalidArgument) byte_count 不能被 AG 整除
    [[nodiscard]] Bytes ReadBytes(Address address,
                                   AddressExtension extension,
                                   ByteCount byte_count);

    /// @brief 读取内存（单次 SHORT_UPLOAD，最多 MAX_CTO/AG 元素）
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes ShortUpload(Address address,
                                     AddressExtension extension,
                                     ElementCount element_count);

    /// @brief 读取内存（SET_MTA + 多次 UPLOAD 分块）
    /// @return 拼接后的完整字节序列
    [[nodiscard]] Bytes UploadChunked(Address address,
                                       AddressExtension extension,
                                       ElementCount element_count);

private:
    CommandExecutor& m_executor_;
    Session& m_session_;

    /// @brief 检查 SHORT_UPLOAD 是否可用且单包可容纳
    [[nodiscard]] bool CanUseShortUpload(ElementCount element_count) const;

    /// @brief 计算 UPLOAD 单块最大元素数
    [[nodiscard]] ElementCount MaxUploadElements() const;

    /// @brief 计算 SHORT_UPLOAD 最大元素数
    [[nodiscard]] ElementCount MaxShortUploadElements() const;

    /// @brief 校验读取参数
    /// @throws XcpException(InvalidArgument) 参数非法或地址溢出
    void ValidateRead(Address address, ElementCount element_count) const;
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
    /// @param event_listener 事件监听器（可选）
    explicit XcpMaster(std::unique_ptr<IXcpTransport> transport,
                        CommandTimeouts timeouts = {},
                        IEventListener* event_listener = nullptr);

    /// @brief 析构，自动断开连接
    ~XcpMaster();

    // 禁止拷贝
    XcpMaster(const XcpMaster&) = delete;
    XcpMaster& operator=(const XcpMaster&) = delete;

    // ---- 连接管理 ----

    /// @brief 建立 XCP 连接
    /// @details 执行: Transport.Open → CONNECT → [GET_COMM_MODE_INFO] → GET_STATUS
    /// @throws XcpException 连接失败
    void Connect();

    /// @brief 断开 XCP 连接
    /// @details 执行: DISCONNECT → Transport.Close。即使失败也释放本地资源。
    void Disconnect();

    /// @brief 是否已连接
    [[nodiscard]] bool IsConnected() const;

    // ---- 内存读取 ----

    /// @brief 读取内存（以字节为单位）
    /// @param address 32 位地址
    /// @param extension 地址扩展
    /// @param byte_count 字节数（必须可被 AG 整除）
    /// @return 读取到的原始字节
    [[nodiscard]] Bytes ReadMemoryBytes(Address address,
                                        AddressExtension extension,
                                        ByteCount byte_count);

    /// @brief 读取内存（以元素为单位）
    /// @param address 32 位地址
    /// @param extension 地址扩展
    /// @param element_count 元素数（以 AG 为单位）
    /// @return 读取到的原始字节
    /// @note 原设计的两个 `ReadMemory` 重载未采用：`ByteCount` 与 `ElementCount`
    ///       均为 `std::uint32_t` 别名，同名重载会使字面量调用产生歧义。
    ///       故按实现拆为 `ReadMemoryBytes` / `ReadMemory` 两个明确名称。
    [[nodiscard]] Bytes ReadMemory(Address address,
                                    AddressExtension extension,
                                    ElementCount element_count);

    // ---- 状态查询 ----

    /// @brief 获取 Session 参数快照
    [[nodiscard]] SessionParameters GetSessionParameters() const;

    /// @brief 获取当前 Session 状态
    [[nodiscard]] SessionState GetSessionState() const;

    /// @brief 手动查询 GET_STATUS 并更新 Session
    /// @return GET_STATUS 响应
    [[nodiscard]] GetStatusResponse QueryStatus();
private:
    std::unique_ptr<IXcpTransport> m_transport_;
    Session m_session_;
    std::unique_ptr<CommandExecutor> m_executor_;
    std::unique_ptr<MemoryAccess> m_memory_access_;
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

    void Open(IPacketListener& listener) override;
    void Close() override;
    void Send(BytesView packet) override;
    [[nodiscard]] bool IsOpen() const noexcept override;

    // ---- 测试控制接口 ----

    /// @brief 设置下次 Send 时的响应
    /// @param response_func 接收发送的 Packet，返回要回调的响应 Packet
    void SetResponse(std::function<Bytes(BytesView)> response_func);

    /// @brief 主动注入异步 Packet（模拟 EV/SERV/DTO）
    void InjectPacket(BytesView packet);

    /// @brief 主动注入 Transport 关闭事件
    void InjectClose(std::string_view reason);

    /// @brief 主动注入 Transport 警告
    void InjectWarning(std::string_view message);

    /// @brief 获取已发送的 Packet 列表
    [[nodiscard]] std::vector<Bytes> SentPackets() const;

private:
    IPacketListener* m_listener_ = nullptr;
    bool m_open_ = false;
    mutable std::mutex m_mutex_;
    std::function<Bytes(BytesView)> m_response_func_;
    std::vector<Bytes> m_sent_packets_;
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

/// @brief 故障注入配置（N 为响应序号，从 1 开始）
struct FaultInjection {
    /// @brief 丢弃第 N 个响应（用于验证 Timeout/SYNCH 恢复）
    std::optional<std::size_t> drop_response_n;

    /// @brief 延迟第 N 个响应（毫秒）
    std::optional<std::pair<std::size_t, std::uint32_t>> delay_response_n;

    /// @brief 对第 N 个响应使用错误 LEN（验证畸形 Datagram 丢弃）
    std::optional<std::size_t> corrupt_len_n;

    /// @brief 对第 N 个响应使用跳号 CTR（固定 +5，验证缺口诊断）
    std::optional<std::size_t> jump_ctr_n;

    /// @brief 对第 N 个响应复用上一个 CTR（验证重复丢弃）
    std::optional<std::size_t> duplicate_ctr_n;

    /**
     * @brief 对第 N 个响应的 CTR 施加指定的有符号偏移（模 65536）
     * @details 批次 3 新增。跳号（+5）与重复（-1）都无法构造"与期望值恰好相差
     *          0x8000"的歧义 Frame（决策 D5），必须由发送侧直接指定偏移量。
     */
    std::optional<std::pair<std::size_t, int>> ctr_offset_n;
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
    void Start();

    /// @brief 停止 Slave
    void Stop();

    /// @brief 获取 Slave 绑定的端口（用于 Master 连接）
    [[nodiscard]] std::uint16_t Port() const;

    // ---- 测试配置 ----

    /// @brief 设置模拟内存内容
    void SetMemory(Address address, BytesView data);

    /// @brief 设置故障注入
    void SetFaultInjection(const FaultInjection& fault);

    /// @brief 获取收到的命令计数
    [[nodiscard]] std::size_t CommandCount() const;

    /**
     * @brief 获取下一个 Slave→Master Frame 将使用的 CTR 值（批次 3 新增）
     * @details 故障注入只改写实际发出的 Header，不改变本计数器的推进规律；
     *          测试据此验证精确偏移注入后的真实 CTR。
     */
    [[nodiscard]] DatagramCtr NextSendCtr() const;

    /// @brief 是否已建立模拟 XCP Session（批次 3 新增，诊断用）
    [[nodiscard]] bool IsConnected() const;

    /// @brief 向已连接 Master 发送一个包含多个 XCP Frame 的 UDP Datagram
    /// @param xcp_packets 要按顺序打包的原始 XCP Packet 列表
    /// @throws XcpException(InvalidState) 尚未记录 CONNECT 来源端点
    /// @throws XcpException(InvalidArgument) Frame 或 Datagram 超过允许长度
    /// @details 仅用于验证 Master 接收端对 XCP 1.1 多 Frame UDP 打包的解析能力。
    void SendPackedFrames(std::span<const BytesView> xcp_packets);

    /**
     * @brief 原样发送一段已构造好的 UDP Payload 到 CONNECT 来源端点（批次 3 新增）
     * @param payload 完整 UDP Payload（自行负责 LEN/CTR Header 与畸形注入）
     * @throws XcpException(InvalidState) 尚未记录 CONNECT 来源端点
     * @details 测试专用。只有 Slave 自身的 Socket 能从"配置的远端端口"发出报文，
     *          因此注入畸形 Datagram（错误 LEN、残留字节等）必须走本入口，
     *          否则会被 Master 的来源过滤先拦掉。
     */
    void SendRawPayload(BytesView payload);

    /**
     * @brief 原样发送一段 UDP Payload 到指定端点（无需先建立 CONNECT 会话）
     * @param payload 完整 UDP Payload
     * @param dst_ip 目的 IPv4 文本
     * @param dst_port 目的 UDP 端口
     * @details 测试专用。报文仍从 Slave 自身的 Socket 发出，因此源端口等于
     *          Slave 端口，可通过 Master 的"严格匹配远端端口"过滤。用于在 Master
     *          未建立 XCP 会话时精确注入指定 CTR 或畸形结构。
     */
    void SendRawPayloadTo(BytesView payload, const std::string& dst_ip,
                          std::uint16_t dst_port);

private:
    /// @brief 接收线程主循环
    /// @details 对每个 UDP Datagram 按 LEN 解析全部完整 Frame，再依序调用 HandleCommand()。
    void ReceiveLoop();

    /// @brief 处理收到的 UDP Datagram 中的一个 XCP Frame
    /// @param xcp_packet Frame 内的原始 XCP Packet
    /// @param source_ip 发送方 IPv4 地址
    /// @param source_port 发送方 UDP 端口
    void HandleCommand(BytesView xcp_packet,
                       const std::string& source_ip,
                       std::uint16_t source_port);

    /// @brief 判断来源是否符合当前逻辑 XCP 会话
    /// @details 未连接时只允许 CONNECT 建立会话；连接后仅匹配 CONNECT 来源 IP，
    ///          不要求后续命令使用同一源端口。
    [[nodiscard]] bool IsCurrentSessionSource(const std::string& source_ip) const;

    /// @brief 发送响应到 CONNECT 时记录的来源 IP:port
    void SendResponse(BytesView xcp_packet);

    /// @brief 把单个 XCP Packet 编码为一个 Frame 并发送到指定端点（含故障注入）
    void SendResponseTo(BytesView xcp_packet, const std::string& ip,
                        std::uint16_t port);

    /// @brief 生成 RES 前缀（0xFF + 数据）
    static Bytes MakeRes(std::initializer_list<std::uint8_t> body);

    /// @brief 生成 ERR 报文
    static Bytes MakeErr(ErrorCode code);

    /// @brief 按当前 MTA 读取指定元素数的模拟内存；越界返回 nullopt
    [[nodiscard]] std::optional<Bytes> ReadAtMta(ElementCount elements);

    /// @brief 把 MTA 前进指定元素数；溢出返回 false
    bool AdvanceMta(ElementCount elements);

    /// @brief 测试 Socket 私有实现声明
    struct SocketImpl;

    /// @brief Loopback UDP Socket 资源的拥有者
    std::unique_ptr<SocketImpl> m_socket_;

    /// @brief Socket 实际绑定的临时端口
    std::uint16_t m_port_ = 0;

    /// @brief 接收循环运行标记
    std::atomic<bool> m_running_{false};

    /// @brief 测试 Slave 接收线程
    std::thread m_receive_thread_;

    /// @brief 保护模拟内存的互斥量
    mutable std::mutex m_memory_mutex_;

    /// @brief 按起始地址存储的模拟 ECU 内存块
    std::map<Address, Bytes> m_memory_;

    /// @brief 保护逻辑 Session、MTA、CTR 和故障配置的互斥量
    std::mutex m_state_mutex_;

    /// @brief 是否已建立模拟 XCP Session
    bool m_connected_ = false;

    /// @brief CONNECT 报文来源 IPv4 地址
    std::optional<std::string> m_connect_source_ip_;

    /// @brief CONNECT 报文来源 UDP 端口，也是后续响应的固定目的端口
    std::optional<std::uint16_t> m_connect_source_port_;

    /// @brief 当前模拟 MTA 的 32 位地址部分
    Address m_mta_ = 0;

    /// @brief 当前模拟 MTA 的地址扩展部分
    AddressExtension m_mta_extension_ = 0;

    /// @brief 下一个 Slave→Master XCP Frame 使用的 CTR
    DatagramCtr m_send_ctr_ = 0;

    /// @brief 已处理的 XCP 命令总数
    std::size_t m_command_count_ = 0;

    /// @brief 当前故障注入配置
    FaultInjection m_fault_;

    /// @brief 已生成的响应计数（故障注入按序号定位；SetFaultInjection() 会重置）
    std::size_t m_response_count_ = 0;
};

}  // namespace calmcar::xcp::test
```

### 15.3 XCP 1.1 UDP 多 Frame 与端点绑定测试要求

1. `UdpHeaderCodec`：一个 Datagram 中连续编码两个 Frame 时，必须按 `Header_1 + Packet_1 + Header_2 + Packet_2` 顺序完整解析；任一 LEN 越界、截断 Header 或尾部残留字节，`DecodeUdpDatagram()` 必须整体失败。
2. `UdpTransport`：收到多 Frame Datagram 时，必须按 Frame 顺序分别执行 CTR 检查并回调 `IPacketListener::OnPacketReceived()`；发送方向仍验证“一次 `Send()` 发送一个 Frame/Datagram”的项目策略。
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
XcpMaster::Connect()         |
    |                        |
    v                        |
CommandExecutor::Execute*()  |
    |                        |
    +-- m_transport_.Send() -->|
    |                        |
    +-- WaitForResponse()    |
    |   (阻塞在 cv)          |
    |                        v
    |                Transport 收到 Datagram
    |                        |
    |                IPacketListener::OnPacketReceived()
    |                        |
    |                CommandExecutor::OnPacketReceived()
    |                  (在工作线程执行)
    |                        |
    |                  加锁 m_mutex_
    |                  设置 m_pending_response_
    |                  notify cv
    |                        |
    v                        |
WaitForResponse() 唤醒        |
    |                        |
    v                        |
解析响应，返回用户            |
```

### 16.2 同步规则

| 共享状态 | 保护机制 | 访问者 |
|---|---|---|
| `Session::m_state_` 等全部字段 | `Session::m_mutex_` | 用户线程 + Transport 线程 |
| `CommandExecutor::m_pending_response_` | `CommandExecutor::m_mutex_` + `m_response_cv_` | 用户线程写(发送) + Transport 线程写(回调) |
| `CommandExecutor::m_transport_closed_` | 同上 | 同上 |
| `UdpTransport::m_send_ctr_` | `atomic` 或 `m_send_mutex_` | 用户线程 |
| `UdpTransport::m_last_recv_ctr_` | `m_recv_mutex_` | Transport 线程 |

### 16.3 死锁规避

- `CommandExecutor::OnPacketReceived()` 在工作线程执行，**不得**在持锁状态下回调 `IEventListener`。
- 正确做法：先在锁内取出响应/事件，释放锁，再回调 `IEventListener`。
- `Session` 的锁是独立的，`CommandExecutor` 调用 `Session` 方法时不会持有自己的 `m_mutex_`。

### 16.4 关闭语义

`UdpTransport::Close()` 的正确步骤顺序（批次 3 修正，见下方"为何不能先关 Socket"）：

1. 设置 `m_running_ = false`；
2. `join()` 接收线程（接收线程退出前调用 `m_listener_->OnTransportClosed()`）；
3. 关闭 Socket，置句柄为非法值；
4. 释放 Socket 资源、清空 `m_listener_`。

约束：

- `Close()` 可在用户线程调用，与接收线程并发安全。
- `Close()` 可重复调用（幂等）；未打开时调用为空操作。
- 唤醒阻塞中的 `recvfrom` 依赖 `Open()` 设置的 `SO_RCVTIMEO` 轮询
  （`UdpTransportConfig::receive_poll_interval_ms`），因此关闭延迟上限为一个轮询周期。
- 必须先 `join()` 再关 Socket。

**为何不能先关 Socket（批次 3 修正的真实缺陷）**：

早期实现在 `join()` 之前就 `closeSocket()`，理由是"关闭 Socket 可立即唤醒阻塞的 `recvfrom`"。
这在功能上确实能唤醒，但引入了一个跨会话/跨用例串扰的窗口：

1. Socket 一旦关闭，其**句柄号立即被 OS 回收**，并可能分配给进程中其他新建的 Socket；
2. 此时接收线程可能仍处于"已从内核取出报文、正在处理并回调监听器"的中间状态；
3. 迟到的操作就会命中**被复用的句柄**，把数据投递到无关的端点上。

该缺陷在 UDP 测试中表现为偶发失败（`UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection`
在满负载全量运行时间歇性失败，单独运行或轻负载复跑 60 次均通过），属于典型的
"仅在 Socket 高频率创建/销毁时暴露"的竞态。修正为先 `join()` 再关 Socket 后，
全量套件连续复跑 15 次、单用例复跑 40 次均稳定通过。

> 代价说明：由于不再依赖"关 Socket 唤醒"，`Close()` 的返回延迟由 `SO_RCVTIMEO`
> 轮询周期决定。`CloseWhileBlockedReturnsPromptly` 用例特意把轮询周期放大到 1000 ms
> 并断言 `Close()` 在 1000 ms 内返回，锁定该延迟上限。若后续要提高关闭实时性，
> 应改用"自管道/事件对象唤醒"而非回退到"先关 Socket"。

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

每个 Frame 都有独立的 LEN 和 CTR；任一 Frame 不得跨 UDP Datagram 边界。本项目发送方向为简单且确定的策略：一次 `IXcpTransport::Send()` 生成一个 Frame 并独占一个 UDP Datagram；接收方向必须能解析连续打包的多个 Frame。

### 17.6 COMM_MODE_BASIC 位定义（已校核）

> 批次 1 已完成校核、批次 3 复核：以下位定义与本地规范文档及第三方实现一致，**不再是待确认项**。

| Bit | 含义 | 取值 |
|---|---|---|
| 0 | BYTE_ORDER | 0=Intel, 1=Motorola |
| 1-2 | ADDRESS_GRANULARITY | 00=BYTE, 01=WORD, 10=DWORD, **11=保留（非法）** |
| 3-5 | 未使用 | 实现按 0 处理，不解释语义 |
| 6 | SLAVE_BLOCK_MODE_SUPPORTED | 0/1 |
| 7 | OPTIONAL_COMM_MODE_AVAILABLE | 0/1（决定是否调用 GET_COMM_MODE_INFO） |

示例 `0xC0` = `1100_0000`：
- bit0=0 → Intel
- bit1-2=00 → AG=BYTE
- bit6=1 → Slave Block Mode supported
- bit7=1 → Optional available

**校核证据**：

1. `docs/XCP_1.3.0_document.md` §12.4 建立 Session 示例（第 3065 行）`← FF 15 C0 08 08 00 10 10`，其自解释为 "Intel Byte Order / AG = BYTE / Slave Block Mode supported"，与 `0xC0` 逐位吻合；同节明确 `MAX_CTO = 8`、`MAX_DTO = 8`。
2. `docs/XCP_1.3.0_document.md` §7.5.1.1（第 1751-1757 行）给出 `BYTE_ORDER=0/1` 与 `AG=BYTE/WORD/DWORD` 的语义。
3. 第三方实现交叉验证：[robotjatek/XCP](https://github.com/robotjatek/XCP/blob/master/XCPLib/ConnectPositivePacket.h) 中 `BYTE_ORDER=0x1`、`ADDRESS_GRANULARITY_0=0x2`、`ADDRESS_GRANULARITY_1=0x4`、`SLAVE_BLOCK_MODE=0x40`、`OPTIONAL=0x80`，即 bit0 / bit1-2 / bit6 / bit7。
4. 代码落点：`src/response_parser.cpp` 的 `kAddressGranularityShift = 1`、`kOptionalMask = 0x80`，以及 `CommModeBasicToAg()` / `AgToCommModeBasicField()` 往返；`bit1-2 == 11` 由 `CommModeBasicToAg()` 返回 `nullopt` 拒绝，并有 `response_parser_test` / `protocol_types_test` 用例锁定。


---

## 18. 开放问题与后续确认

### 18.1 必须在编码前确认的问题

| 编号 | 问题 | 影响 | 当前状态（批次 3 收口） |
|---|---|---|---|
| Q1 | COMM_MODE_BASIC 的精确 bit 定义 | `ParseConnectResponse` 实现 | ✅ **已确认，与设计推断一致**：bit0=BYTE_ORDER、bit1-2=AG（11 保留）、bit6=SLAVE_BLOCK_MODE、bit7=OPTIONAL。证据见 §17.6；代码由 `protocol_types_test` / `response_parser_test` 逐位锁定 |
| Q2 | CONNECT 响应中 MAX_DTO 是 1 字节还是 2 字节 | `ConnectResponse` 字段类型 | ✅ **确认为 2 字节 WORD**，位于 RES Byte 4-5，按 Session Byte Order。依据：`docs/ASAM_XCP_Part3_..._1.1.md` §1.4「MAX_DTO: Parameter WORD, 0x0008-0xFFFF」+ §12.4 示例 `FF 15 C0 08 08 00 10 10`（Byte 4-5 = `08 00` → 8）。第三方实现注释亦为 `MAX_DTO = 3, //2 bytes long!` |
| Q3 | SET_MTA 中 address 字段的确切字节序 | `EncodeSetMta` 实现 | ✅ **确认按 Session Byte Order**。SET_MTA 只能在 CONNECT 之后使用，届时字节序已协商完成；`command_codec_test` 有 Intel/Motorola 双黄金报文锁定，`memory_access_test` 有 Motorola 端到端用例 |
| Q4 | SYNCH 命令是否需要特殊编码 | `EncodeSynch` | ✅ **确认无需特殊编码**：命令即 `[FC][00]` 2 字节；其"始终以 ERR_CMD_SYNCH 应答"是恢复语义，已由 `CommandExecutor::PerformRecovery()` 实现并只在恢复窗口内接受该错误码（计划 §6.2 第 1 条），`recovery_test` 覆盖正反两例 |
| Q5 | CMake 构建系统和测试框架选择 | 工程基线 | ✅ **已定**：CMake ≥ 3.24 + GoogleTest v1.15.2（FetchContent，支持 `.deps-cache/` 离线源）；库目标 `libxcp` / 别名 `libxcp::libxcp` |
| Q6 | `.clang-format` 文件是否存在 | 代码格式 | ✅ **存在**（用户提供，Google 基础风格）；本批次全部改动经 `clang-format --dry-run --Werror -style=file` 校验通过 |

> 至此设计阶段的 6 个开放问题全部关闭。仍存在的**外部验证缺口**不属于本节范围：尚未与第三方真实 ECU / CANape 做互操作抓包对照（见计划文档 §11.4 与验收标准 15）。

### 18.2 设计中标记为"项目策略"的决策

| 编号 | 决策 | 依据 | 实现落点 | 验证用例 |
|---|---|---|---|---|
| D1 | CTR 初值为 0，`Open()` 时复位 | 计划文档 §4.7 | `UdpTransport::Open()` | `ReopenResetsSendCtrAndReceiveBaseline` |
| D2 | 原命令最多 2 次恢复重试 | 计划文档 §6.2 | `CommandTimeouts::max_retries = 2`（经 `CommandExecutor::m_timeouts_` 读取） | `RetryExhaustionReportsRecoveryFailed`（断言 Upload×3、Synch×2）、`ZeroRetriesFailsImmediately` |
| D3 | Master 侧默认严格匹配远端 IP+端口（可配置为仅匹配 IP） | 本项目安全策略；不等同于标准对 Slave 的连接绑定规则 | `UdpTransport::IsRemoteMatch()` | `ForeignPortRejectedWhenStrict`、`IgnoresForeignSourceIp` |
| D4 | UDP 不重排、不重传 Frame | 计划文档 §4.7 | 接收路径无重排/重传逻辑 | `BackwardCtrFrameDropped`、`DuplicateCtrIsDropped` |
| D5 | 恰好相差 0x8000 的 CTR 视为歧义丢弃 | 计划文档 §4.7 | `UdpTransport::HandleFrame()` | ✅ **批次 3 已补齐精确构造用例**：`AmbiguousCtrOffsetIsDropped`（差值恰 0x8000 → 丢弃 + 诊断 + 基线不推进）、`JustBelowAmbiguousBoundaryIsAccepted`（0x7FFF → 前向跳号接收）、`JustAboveAmbiguousBoundaryIsDropped`（0x8001 → 后向丢弃）、端到端 `AmbiguousCtrFrameDroppedAndSessionContinues`。夹具语义由 `CtrOffsetProducesExactHeaderCounter` 锁定 |
| D6 | 默认单 Frame Packet 上限为 65503，Datagram 上限为 65507 | IPv4 UDP 理论上限与 XCP Header 长度 | `kUdpMaxXcpPacket` / `kUdpMaxDatagramSize` | `LimitsMatchIpv4Udp`、`RejectsOversizedPacket`、`MaxAllowedPacketAccepted`、`DatagramOverMaxSizeDiscarded` |
| D7 | 发送方向每个 Datagram 只包含一个 Frame；接收方向支持多 Frame 打包 | XCP 1.1 Part 3 允许的子集发送策略与完整接收兼容性 | `EncodeUdpFrame()` + `HandleDatagram()` | `EachSendProducesOneFrameWithIncrementingCtr`、`MultipleFramesInOneDatagramDeliveredInOrder` |
| D8 | UdpTestSlave 连接后仅校验 CONNECT 来源 IP，响应始终发往原 CONNECT 来源 IP:port | XCP 1.1 Part 3 UDP/IP Connection Behavior | `UdpTestSlave::IsCurrentSessionSource()` | `SameIpDifferentPortStillServed`（并断言响应不发往变更后的端口） |

> D5 说明（批次 3 更新）：批次 2 曾记录"缺少精确构造 0x8000 差值的用例"。本批次通过给
> `FaultInjection` 增加 `ctr_offset_n`（按模 65536 施加有符号偏移）关闭该缺口：
> 跳号注入固定 +5、重复注入固定 -1，两者都无法命中 0x8000，因此必须由发送侧直接指定偏移量。
> 现在 0x7FFF / 0x8000 / 0x8001 三个相邻取值均有正向用例，边界归属被逐值锁定。

### 18.3 可选增强（本阶段不实现，但接口预留）

| 预留项 | 批次 3 状态 |
|---|---|
| `IEventListener` 接口已定义，本阶段 DTO 只上报不解析 | ✅ 接口已落地并改名对齐 §2.2.1（`OnEvent`/`OnService`/`OnDto`）；`CommandExecutor` 在锁外回调，DTO 仅识别上报 |
| `UdpTransportConfig::strict_remote_port` 预留 NAT 兼容 | ✅ 已实现并有 `ForeignPortRejectedWhenStrict` 双向用例（严格拒绝 / 放宽接受） |
| `Session::Fail()` 预留 Failed 状态 | ✅ 已实现并被真实使用：SYNCH 恢复失败、CONNECT 失败/响应非法/参数校验失败均转入 `Failed`；`RecoveryFailureMarksSessionFailed`、`session_test` 的状态机用例锁定"Failed 必须先 Reset 才能重连"。另提供 `FailReason()` 读取失败原因（该访问器为本批次补齐到文档，代码早已存在） |

> 本节三项在本批次后均已不再是"仅预留"，而是有实现、有用例的既成行为；保留本节以便追溯原始范围划分。


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
cfg.remote_host = "127.0.0.1";
cfg.remote_port = slave_port;  // 从 UdpTestSlave 获取
cfg.local_host = "127.0.0.1";
auto transport = std::make_unique<UdpTransport>(std::move(cfg));

// 2. 创建 Master
XcpMaster master(std::move(transport));

// 3. 连接
master.Connect();  // CONNECT → GET_COMM_MODE_INFO → GET_STATUS

// 4. 读取内存（AG=BYTE，故 4 元素 == 4 字节）
auto data = master.ReadMemory(0x70012340, 0x00, 4);

// 5. 断开
master.Disconnect();
```

> 注意单位：`ReadMemory()` 的第三个参数是**元素数（按 AG 计数）**；需要按字节读取时
> 使用 `ReadMemoryBytes()`。二者刻意不同名，因为 `ByteCount` 与 `ElementCount`
> 同为 `std::uint32_t` 别名，同名重载会让字面量调用产生歧义（见 §14）。

### B.2 Mock Transport 测试

```cpp
using namespace calmcar::xcp;
using namespace calmcar::xcp::test;

MockTransport transport;
transport.SetResponse([](BytesView sent) -> Bytes {
    // 检查 sent 是 CONNECT 命令
    // 返回 CONNECT 响应
    return {0xFF, 0x15, 0xC0, 0x08, 0x08, 0x00, 0x10, 0x10};
});

XcpMaster master(std::make_unique<MockTransport>(std::move(transport)));
master.Connect();
// 断言 master.GetSessionParameters() 正确
```

---

> 本文档的接口定义已在批次 1~3 中全部落地并通过编译与测试验证（Release 配置，253 项用例）。
> 第 18.1 节的 6 个开放问题已全部关闭；命名规则的落地核对结果见 **§2.2.1.3**。
> 仍未闭环的是**外部互操作验证**：所有端到端测试均为自有 Master ↔ 自有 UdpTestSlave，
> 未与第三方 ECU / CANape 抓包对照（计划文档 §11.4、验收标准 15）。
>
> **口径变更（批次 4 提出，批次 5 完成闭环）**：早先"与本文档存在差异处一律以代码为准"的口径
> **已收窄**为二分——
> - **命名风格**（§2.2.1 的 struct / class 分流）：以**本文档为准**，代码分批跟进。
>   该跟进**已于批次 5 完成**：原登记的 A 冲突（18 字段 / 189 处）、B 待办（39 字段 / 377 处）、
>   class 尾下划线缺失（**47 条声明**，旧扫描器只看见 44 条）三项**全部归零**，代码与本文档的
>   struct/class 成员命名现已完全一致（实测数据见 §2.2.1.3 末）；
>   批次 5 收尾复查中发现并回退了 2 处**由共用盲区的改名脚本造成的 class 成员误改**
>   （`CommandCodec`/`ResponseParser` 的 `m_byte_order_` 曾被改成裸名），
>   该两文件现已与批次 5 之前逐字节一致；
> - **协议语义与实现细节**（字段布局、超时、状态机行为等）：仍以**代码为准**，
>   差异就地标注批次号。
>
> 任何偏差都必须显式登记，禁止静默报告为已合规（批次 3 教训，见 §2.2.1.2 末）。






