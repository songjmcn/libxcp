# XCPlite 是否支持 CAN / CAN FD 作为传输层协议 —— 分析结论

> 分析对象：`thirdparty/XCPlite`（子模块，`github.com/vectorgrp/XCPlite.git`）
> 版本：`V2.2.2-4-g88e3cbe`（commit `88e3cbeec79318f50067eede2502bcf4d34dab5f`，2026-09-24）
> 分析方式：只读源码与文档核查，未修改 thirdparty 任何文件
> 结论时间：2026-10-22

---

## 1. 结论（先行）

**XCPlite 不支持 CAN 或 CAN FD 作为 XCP 传输层。** 它只实现 **XCP on Ethernet（TCP / UDP / 原始以太网 UDP）**，
外加一种非以太网的 **共享内存（SHM）** 传输层用于同机多进程场景。

CAN / CAN FD 在 XCPlite 中**没有任何可用的实现路径**：既没有编译开关，也没有传输层源文件，
更没有公开 API 入口。若要 CAN 支持，只能自己实现传输层，且**无法绕开其以太网协议层耦合**（见 §4.3）。

---

## 2. 证据

### 2.1 官方定位：明确"exclusively XCP on Ethernet"

`thirdparty/XCPlite/README.md:11`：

> Designed exclusively for the **XCP on Ethernet Transport Layer** (using TCP or UDP sockets with jumbo frames)

`thirdparty/XCPlite/CLAUDE.md:7` 同样表述：

> a C11/C++17 implementation of the ASAM XCP measurement and calibration protocol, for XCP-on-Ethernet (TCP/UDP)

### 2.2 文档中的 CAN 提及，全部是"对比/背景"而非"能力"

| 位置 | 原文要点 | 性质 |
|---|---|---|
| `thirdparty/XCPlite/README.md:116` | `XCPbasic - Free implementation for smaller Microcontrollers (8-bit+), optimized for CAN` | 把 CAN 场景**推荐给另一个产品** |
| `thirdparty/XCPlite/docs/XCP_INTRODUCTION.md:23` | "There are transport layers for ... CAN, CAN-FD, FLEXRAY, SPI and Ethernet" | XCP **标准**的通用背景介绍 |
| `thirdparty/XCPlite/docs/XCP_INTRODUCTION.md:39` | XCPbasic 是 CAN 传输层的免费实现 | 同上，指向别的实现 |
| `thirdparty/XCPlite/src/xcplite.c:28` | `For micro-controllers, more features and more transport layers (CAN, FlexRay) are provided` | 说明**完整版 XCPprof** 才有，Lite 版没有 |

`src/xcplite.c:28` 这句是最直接的自我声明：CAN / FlexRay 属于商业版（XCPprof）范畴。

### 2.3 传输层只存在以太网与共享内存两套

`thirdparty/XCPlite/src/` 下与传输层相关的实现文件仅有：

- `xcpethtl.c` / `xcpethtl.h` — Ethernet 传输层（报文成帧）
- `xcpethserver.c` / `xcpethserver.h` — XCP on Ethernet 服务端（TCP/UDP 连接管理）
- `sockets.c` / `sockets.h` — 平台 Socket 抽象
- `socket_raw.c` + `socket_raw_hal.h` / `socket_raw_hal_linux.c` — 原始以太网 UDP/IPv4（无 TCP/IP 协议栈的目标）
- `shm.c` / `shm.h` + `xcpshmserver.h` — 共享内存传输层（`shm` 配置）

**不存在** `xcpcantl.c`、`xcpcan*.c` 一类文件。`inc/` 公开头文件中也无任何 CAN 相关声明。

### 2.4 传输层选择开关只有以太网选项

`thirdparty/XCPlite/src/xcplib_cfg.h:76-77`（默认配置）：

```c
#define OPTION_ENABLE_TCP
#define OPTION_ENABLE_UDP
```

`thirdparty/XCPlite/src/xcptl_cfg.h:16-21` 把这些映射为传输层编译开关：

```c
#if defined(OPTION_ENABLE_UDP) || defined(OPTION_ENABLE_UDP_RAW)
#define XCPTL_ENABLE_UDP
#endif
#if defined(OPTION_ENABLE_TCP)
#define XCPTL_ENABLE_TCP
#endif
```

`xcptl_cfg.h:24-32` 的 `#error` 组合校验也只在 UDP / TCP / UDP_RAW 之间互斥，没有任何 `OPTION_ENABLE_CAN` 分支。
全仓库搜索 `OPTION_ENABLE_CAN` / `XCP_ON_CAN` 的**实现**（非 A2L 元数据）结果为零。

### 2.5 公开 API 只有以太网服务端

`thirdparty/XCPlite/inc/xcplib.h:46`：

```c
bool XcpEthServerInit(const uint8_t *address, uint16_t port, bool use_tcp, uint32_t measurement_queue_size);
```

初始化入口写死在"Ethernet Server"语义上（`inc/xcplib.h:37-53` 整节标题为 `XCP on Ethernet server`）。
没有任何 `XcpCanServerInit` 或传输层可插拔的工厂接口。

### 2.6 协议层版本上报的传输层版本是硬编码的以太网版本

`thirdparty/XCPlite/src/xcptl_cfg.h:35`：

```c
#define XCP_TRANSPORT_LAYER_VERSION 0x0104
```

该常量直接写进 CONNECT 响应（`src/xcplite.c:2077`）与 GET_VERSION 响应（`src/xcplite.c:2898-2899`）。
注意 `0x0104` 是 **XCP on Ethernet 传输层规范的版本号**，不是 CAN 的。CAN 传输层规范版本独立（历史上为 1.0/1.1），
不可能靠改一个常量切换过去。

### 2.7 `XCP_ON_CAN` 只出现在 AML 元数据文件里，且不参与生成

`thirdparty/XCPlite/XCP_104.aml:854`、`:1252-1253`、`:1283-1284` 中确有：

```
block "CAN_FD" struct { ... }
block "XCP_ON_CAN" struct { struct CAN_Parameters; ... }
```

**但这不构成 CAN 支持**，理由有三：

1. `XCP_104.aml` 是 **ASAM 官方的 AML 描述文件**（对整个 XCP 标准的数据模型描述，含所有传输层），
   仓库里以 `XCP_104.aml`、`CANape/XCP_104.aml` 等形式重复存在约 20 份，属于随附资料。
2. XCPlite 生成 A2L 时只是 **`/include "XCP_104.aml"`** 引用它（`src/a2l_writer.c:474`），
   并不生成 `XCP_ON_CAN` 结构。
3. XCPlite 实际生成的传输层 IF_DATA **只有以太网一种**：
   `src/a2l_writer.c:194-204` 的格式化串是 `"/begin XCP_ON_%s_IP\n" ... "/end XCP_ON_%s_IP\n"`，
   由 `A2lCreate_ETH_IF_DATA()`（`src/a2l_writer.c:329`）调用，`%s` 只填 `TCP` 或 `UDP`。
   调用点 `src/a2l_writer.c:523` 传的就是 `use_tcp`。**不存在 `A2lCreate_CAN_IF_DATA()`**。

### 2.8 协议层带 `XCP_ENABLE_PROTOCOL_LAYER_ETH` 编译开关

`thirdparty/XCPlite/src/xcp_cfg.h:24`：

```c
#define XCP_ENABLE_PROTOCOL_LAYER_ETH
```

该宏在 `src/xcplite.c:2726` 等处门控以太网专有协议层命令（如 `CC_TL_GET_SERVER_ID` /
`CC_TL_GET_SERVER_ID_EXTENDED`，见 `src/xcplite.c:2826-2829`）。这些命令的响应布局是**以太网专属**的
（MAC 地址、IP 地址、端口），CAN 上根本无对应语义。`src/xcp.h:942` 里那行
`CRM_GET_SERVER_ID_CAN_ID_CMD_STIM` 位于一段被注释掉的宏定义块中（`src/xcp.h:930-943` 区间内），
属于遗留草稿，未启用。

### 2.9 唯一的非以太网传输层是共享内存，不是 CAN

`shm` 配置（`src/xcplib_shm_cfg.h`）实现的是**同机多进程共享内存**传输
（`docs/SHM.md`），用途是 SIL-Kit 多参与者仿真等场景，与 CAN 总线无关。
它同样不是可扩展的第三方传输层插槽。

### 2.10 原始以太网 HAL 也不是 CAN 的接入点

`src/socket_raw_hal.h:10-16` 提供了可替换的以太网 HAL 后端：

```
socket_raw_hal_linux.c  Linux AF_PACKET (development and test)
socket_raw_hal_xlapi.c  Vector XLAPI on Windows      (future)
socket_raw_hal_cmp.c    ASAM CMP capture modules     (future)
```

三个后端**全部是以太网帧**后端（含 Vector XLAPI 这种 CAN 硬件厂商的接口，但此处用的是它的**以太网**能力）。
`src/socket_raw_hal.h:19-23` 明确规定帧契约是"完整以太网帧（不含 FCS）：目的 MAC、源 MAC、EtherType、载荷"。
`src/socket_raw.c` 在其上实现 UDP/IPv4/ARP/ICMP —— 这层抽象**语义上不可能承载 CAN**。

> 这一点容易误判，需要特别指出：**不能**通过写一个 `socket_raw_hal_xxx.c` 来实现 CAN 传输层。
> 该 HAL 的契约是"以太网帧"，不是"CAN 帧"。

---

## 3. 唯一可复用的部分：协议层与传输层是分离的

好消息是 XCPlite 的分层是清晰的，协议层本身并不感知以太网：

```
src/xcplite.c, src/xcp.h        XCP 协议层（命令处理、DAQ 状态机）      ← 传输无关
src/xcpappl.c                   应用回调
src/xcpethtl.c/.h               以太网传输层（成帧）                   ← 传输相关
src/xcpethserver.c/.h           XCP on Ethernet 服务端                ← 传输相关
src/sockets.c/.h                Socket 抽象                           ← 传输相关
```

`docs/TECHNICAL.md:372` 也确认 socket 抽象是"for XCP on Ethernet transport layer"。
理论上协议层（`xcplite.c`）可以配一套新的 CAN 传输层复用。

---

## 4. 若要支持 CAN，需要付出什么

### 4.1 需要新写的部分

- CAN 传输层成帧：CTO/DTO 在 CAN 帧上的打包与拆解（CAN 上是 `MAX_CTO=8`，CAN FD 最大 64 字节）
- CAN 标识符管理（每条 DAQ List 一个独立 CAN ID 以保证 DTO 可识别）
- 平台 CAN 后端：Windows（Vector XL / PEAK）、Linux（SocketCAN）、macOS（基本没有原生 SocketCAN 等价物，是硬缺口）
- A2L 生成：`A2lCreate_CAN_IF_DATA()` 及 `XCP_ON_CAN` / `CAN_FD` 结构输出
- 协议层 `XCPTL_MAX_CTO_SIZE` / `XCPTL_MAX_DTO_SIZE` 需按 CAN 重定义（当前 `src/xcptl_cfg.h:40` 为 248）

### 4.2 需要修改 thirdparty 的既有文件

这是与项目 `AGENTS.md`（"针对 thirdparty 不能在没有明确指示的情况下修改任何文件"）冲突的地方。
关键耦合点：

- `src/xcpethserver.c` — 服务端初始化流程与以太网绑定
- `src/a2l_writer.c:474`、`:523` — AML include 与 IF_DATA 生成写死以太网
- `src/xcptl_cfg.h:16-35` — 传输层开关与版本号写死

### 4.3 无法"零侵入"绕过的硬约束

想在不改 XCPlite 的前提下接 CAN，只能走"仿造以太网"的歪路（例如用 CAN 之上跑个假的 UDP/IPv4 层），
这在工程上不可接受：会引入无意义的协议开销、破坏 A2L 描述与工具的对应关系、且 CAN 帧长（8/64 字节）
根本装不下以太网 MTU 假设下的报文。**结论是第三方插槽不存在。**

---

## 5. 对 libxcp 项目的建议

### 5.1 现状确认

`docs/modules/传输层.md:9-10` 与 `README.md:18` 已把 CAN/CAN FD 列为 v1.0 之后的下一步，
并注明"预留 CAN 系实现位"。本次分析**证实该判断是对的**，且需要修正一个预期：

> XCPlite **不能**作为 libxcp 的 CAN 传输层实现来源。它是测试对手端（Slave），
> 而不是传输层库——`docs/modules/第三方依赖.md:32` 对它的定位（"测试对手端（L3 集成与示例）"）是准确的。

### 5.2 对 libxcp 自身架构的影响

libxcp 的 `include/libxcp/ixcp_transport.hpp` 定义的 `IXcpTransport` 抽象接口是**正确且充足**的：
它只收发完整 XCP Packet，不感知介质，天然能承载 `SocketCanTransport`（`AGENTS.md` 推荐架构里已列出）。
CAN 支持应在 **libxcp 自己的 src/ 下新增传输层实现**，与 XCPlite 无关。

### 5.3 需要决策的开放问题（建议下一步讨论）

1. **CAN 还是 CAN FD 优先？** 两者 `MAX_CTO` 差异巨大（8 vs 64），会影响 `IXcpTransport` 是否需要
   暴露"最大包长"给协议核心（当前接口无此概念，协议核心可能默认按 UDP 大包处理）。
2. **平台优先级？** Windows（Vector XL/PEAK）与 Linux（SocketCAN）都可行，macOS 缺少原生 CAN 栈，
   需要明确是否放弃或走厂商 SDK。
3. **是否只做 Master（客户端）？** libxcp 是 Master 库，不需要实现 Slave 侧 CAN 传输层，范围可收窄。
4. **`AGENTS.md` 推荐架构中的 `VectorCanTransport` / `SocketCanTransport` / `PeakCanTransport`
   是三个独立类还是共享一个 CAN 传输层核心 + 三个链路后端？** 建议后者（与 libxcp 现有 UDP 分层一致）。

---

## 6. 核查清单（可复现）

| 检查项 | 命令 | 结果 |
|---|---|---|
| CAN FD 实现代码 | `grep -ri canfd thirdparty/XCPlite --include=*.c --include=*.h` | 0 命中 |
| CAN 传输层源文件 | `ls thirdparty/XCPlite/src/` | 无 `*can*` 文件 |
| 传输层编译开关 | `grep -n "OPTION_ENABLE" src/xcplib_cfg.h` | 仅 TCP / UDP / UDP_RAW |
| 公开 API | `grep -n "Xcp.*Init" thirdparty/XCPlite/inc/xcplib.h` | 仅 `XcpEthServerInit` |
| 生成的 IF_DATA | `grep -n "XCP_ON_" src/a2l_writer.c` | 仅 `XCP_ON_%s_IP`（%s = TCP/UDP） |
| `XCP_ON_CAN` 出现位置 | `grep -rn "XCP_ON_CAN" thirdparty/XCPlite` | 仅在 `XCP_104.aml`（ASAM 资料文件） |
| AML 是否被生成使用 | `grep -n "XCP_104.aml" src/a2l_writer.c` | 仅 `/include` 引用，非生成 |

---

## 7. 一句话总结

XCPlite 的传输层**只有以太网（TCP/UDP/原始以太网）与共享内存**；CAN / CAN FD 完全未实现，
`XCP_ON_CAN` / `CAN_FD` 仅作为 ASAM 官方 AML 元数据随附存在。libxcp 若要做 CAN，
必须在自己的 `src/` 下基于 `IXcpTransport` 新增实现，XCPlite 帮不上忙。