# XCP V1.5 Quick Start：最小标定与测量手册（TCP / UDP 双承载）

> **文档性质**：本手册是对 ASAM XCP 规范 V1.5 两份文档的快速入门讲解（**规范总结，而非任何实现的总结**）：
>
> - 协议层：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`（下称 **PL**，ASAM AE MCD-1 XCP BS Protocol Layer V1.5.0）
> - 以太网传输层：`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.md`（下称 **TL**，ASAM AE MCD-1 XCP AS Ethernet Transport Layer V1.5.0）
>
> 正文每个协议结论均标注规范出处（如 *PL §7.5.2.1*、*TL §4.3.2*），可按节号回查原文。
> **注意**：`docs/` 下 PL 的 Markdown 转换版存在图文错位（README §7 已声明），凡字节表均以转换版表格交叉核对、以规范语义为准；图片内容未采信。
>
> **约定**：多字节字段一律注明字节序；`BYTE`=1 字节、`WORD`=2 字节、`DWORD`=4 字节；位置编号从 0 开始；HEX 值写作 `0xNN`。AG = Address Granularity（地址粒度）。

---

## 目录

- [第 0 章 导读](#第-0-章-导读)
- [第 1 章 XCP 是什么：分层模型](#第-1-章-xcp-是什么分层模型)
- [第 2 章 协议骨架](#第-2-章-协议骨架)
- [第 3 章 标定最小闭环](#第-3-章-标定最小闭环)
- [第 4 章 测量最小闭环](#第-4-章-测量最小闭环)
- [第 5 章 以太网传输层：同一套 XCP，跑在 TCP 和 UDP 上](#第-5-章-以太网传输层同一套-xcp跑在-tcp-和-udp-上)
- [第 6 章 A2L 接口与 V1.5 要点](#第-6-章-a2l-接口与-v15-要点)
- [附录 A 术语表](#附录-a-术语表)
- [附录 B 命令码速查表](#附录-b-命令码速查表)

---

## 第 0 章 导读

### 0.1 读者与目标

面向**第一次接触 XCP** 的嵌入式/整车软件工程师。读完本手册你应当能够：

1. 说清一个 XCP 帧在 TCP 与 UDP 上的线上格式（wire format）；
2. 用命令序列完成一次最基本的**标定**（读参数、写参数、按位修改、切页）；
3. 用命令序列完成一次最基本的**测量**（动态配置 DAQ、启动、解码收到的数据流）；
4. 知道规范中哪些是必选、哪些是可选，以及未实现时的表现。

### 0.2 范围裁剪

本手册只讲"最小闭环"，以下内容**明确不在范围内**（需要时直接查规范章节）：

| 不在范围 | 规范位置 |
|---|---|
| 刷写（FLASH PROGRAMMING，PGM 资源） | PL §4.5、§7.5.5 |
| 时间同步/时间相关性（Time Correlation） | PL §4.6 |
| BYPASS、数据轮询（Polling）、STIM 刺激细节 | PL §4.2、§4.3 |
| 网关转发、软触发、CHECKSUM 细节 | PL §5.1、§7.5.1.14 |
| 块传输/交错通信模型（TL 下为 optional） | PL §5.2.2、§5.2.3；TL §4.2 |
| 软件调试（SW-DBG-over-XCP） | PL §4.8 |

### 0.3 阅读路线

- 只关心"线上长什么样"：第 2 章 + 第 5 章；
- 要做标定工具：第 2 章 → 第 3 章；
- 要做测量功能：第 2 章 → 第 4 章 → 第 5.7 节（带宽规划）；
- 对接 A2L：第 6 章。

---

## 第 1 章 XCP 是什么：分层模型

### 1.1 定位

XCP（Universal Measurement and Calibration Protocol）是 ASAM MCD-1 系列标准定义的**测量与标定协议**：Master（标定/测量工具）通过某种传输层与 Slave（ECU 上的 XCP 从机）通信，对 ECU 内存中的参数做在线读写（标定），或以受控节拍批量取回变量数据（测量/DAQ）。规范把协议拆成两层：

```
┌─────────────────────────────┐
│  Protocol Layer (PL)        │  ← 命令语义：CONNECT/UPLOAD/DOWNLOAD/DAQ…
│  ASAM MCD-1 XCP BS PL V1.5  │     报文结构：识别字段/计数器/时间戳/数据字段
├─────────────────────────────┤
│  Transport Layer (TL)       │  ← 承载：CAN / FlexRay / LIN / Ethernet …
│  本文关注 Ethernet (TCP/UDP)│     帧格式：Header + Packet + Tail
├─────────────────────────────┤
│  物理网络（以太网）          │
└─────────────────────────────┘
```

- **PL 与 TL 解耦**：同一套 XCP 报文（packet）内容不变，TL 只负责把它封装进具体网络的帧并定义连接行为（PL §5.5：XCP Message = Header + Packet + Tail，其中 Header/Tail 由 TL 定义）。
- 以太网 TL 规范（TL）对 **TCP/IP 与 UDP/IP 两种承载统一成文**（TL 标题即 "XCP on Ethernet (TCP/IP and UDP/IP)"），这正是本手册"双协议"的依据：同一协议内容，两种承载行为，见第 5 章。

### 1.2 拓扑要点（一笔带过）

PL §5.1 拓扑要点：基本形态是 **single-master/single-slave**——Slave 驱动同时只能处理一个连接；CONNECT 建立的是"连续的、逻辑的、无歧义的 point-to-point"通道。规范**不允许 single-master/multi-slave 广播**（例外只有 GET_SLAVE_ID 与 GET_DAQ_CLOCK_MULTICAST 多播）；允许同一网络存在多条互不干扰的 master/slave 通道；允许 **XCP 网关**设备转发并适配两侧 TL Header/Tail。本手册全部按"Master 直连单 Slave"展开。

### 1.3 版本协商：先问版本再干活

- 版本号是 **16 位值：高字节 Major、低字节 Minor**（PL §3.1.1）。Major 递增对应需要 Slave 驱动软件功能性变化的修改，Minor 递增对应不影响实现的修改——所以 **CONNECT 正响应只回高字节**（字节 6=PL Major、字节 7=TL Major，PL §7.5.1.1 Table 38 原文 "MSB only"）；
- 完整版本用 **GET_VERSION**（Level-1 命令 `0xC0 0x00`，**optional**）获取：响应字节 2/3/4/5 = PL Major / PL Minor / TL Major / TL Minor（PL §7.5.1.2 Table 44）。Slave 不支持时回 `ERR_CMD_UNKNOWN(0x20)`，Master 退回到只用 Major + 功能探测（见 2.6）；
- PL 与 TL 版本可独立组合（各自在自己的规范里写明对对方的版本前置要求），允许组合以 PL §3.1.2 的 Compatibility Matrix 为准——V1.5 PL ↔ V1.5 以太网 TL 属同代组合。

---

## 第 2 章 协议骨架

### 2.1 标准通信模型：一问一答

以太网只使用**标准通信模型**（Standard Communication Model，PL §5.2.1；TL §4.2 明确块传输模型与交错模型均为 optional）。其铁律有两条：

1. 每个 request 都收到对应的 response（RES）**或** error packet（ERR）——个别命令的例外写在各自命令描述里（PL §5.2.1/§7.1.1）；
2. **未收到上一请求的响应之前，Master 不得发送下一个 request**（PL §5.2.1 原文 "the master device may not send a new request until the response to the previous request has been received"）。

超时后用 SYNCH 重新同步（见 2.10）。

### 2.2 XCP 状态机（PL §5.3）

```
            CONNECT(0xFF)                DISCONNECT(0xFE)/超时/错误
 DISCONNECTED ───────────► CONNECTED ────────────────────────► DISCONNECTED
   只处理 CONNECT      ◄────────────  重新 CONNECT 亦回 ack
```

- **DISCONNECTED**：Slave 只响应 CONNECT，其余包一概不理；
- **CONNECT** 建立连续的、逻辑的、无歧义的 point-to-point 连接；已连接时再收到 CONNECT，Slave 也必须回 ack（PL §5.3 原文；未规定是否重置会话——换源地址前应先 DISCONNECT，尤其 UDP 的源地址锁定语义见 5.1）；
- 错误严重级 S0–S3（见 2.10）：S0–S2 不改变 Slave 状态；S3（Fatal）使 Slave 回到 DISCONNECTED；
- **RESUME 模式**（PL §4.1.7.2）：Slave 把 DAQ 配置存入非易失存储，上电或重连后自动恢复数据流；在 Ethernet 上 Master 必须**持续重试建立连接**才能接住恢复的流量（TL §4.1）。

### 2.3 报文分层：帧、Message、Packet

以 Ethernet 为例，从外到内三层（PL §5.5 + TL §4.3）：

```
UDP 数据报 / TCP 字节流片段（TL 帧）
└─ XCP Message = [ Header(LEN,CTR) ][ XCP Packet ][ Tail(0–3 FILL) ]
                 └ XCP Packet = [ 识别字段 PID ][ 计数器字段(可选) ][ 时间戳字段(可选) ][ 数据字段 ]
```

- **TL Header/Tail** 由传输层定义（第 5 章细讲）；
- **PL Packet** 由协议层定义（本章 2.4–2.6）。
- 注意术语：PL 文档中 "XCP Packet 起始于识别字段"；TL 文档中 "packet" 指的就是这条 PL Packet，"frame" 指整个 UDP 数据报/TCP 帧——两层文档用词各自独立，阅读时按所在文档区分。

### 2.4 包类型与识别字段（PL §7.1.1–7.1.2.1）

XCP 包分两类通道，识别字段第一字节恒为 **PID**（Packet IDentifier）：

**CTO（Control TO）通道**——命令与应答，按 PID 区分 5 种：

| CTO 类型 | PID（字节 0） | 后续字节 | 含义 |
|---|---|---|---|
| CMD | 命令码 `0xC1–0xFF`（Level-0）；`0xC0`+子码（Level-1） | 命令参数 | Master → Slave 命令 |
| RES | `0xFF` | 字节 1..=响应数据 | Slave 对 CMD 的正响应 |
| ERR | `0xFE` | 字节 1=错误码，字节 2..=可选附加信息 | Slave 对 CMD 的负响应 |
| EV | `0xFD` | 字节 1=事件码（见 2.8） | Slave 主动事件（无对应命令，不保证送达） |
| SERV | `0xFC` | 字节 1=服务码（0x00=SERV_RESET、0x01=SERV_TEXT，PL §7.3） | Slave 请求 Master 提供服务 |

（PL §7.1.3.1–5，Tables 15–19。命令码 `0xFE/0xFD/0xFC/0xFF` 与响应 PID 同值不冲突——CMD 走请求方向、后四者走响应/异步方向。）

**DTO（Data TO）通道**——同步数据（DAQ/STIM），PID 编码数据归属。PL §7.1.2.1 共列 5 种识别字段型（Figures 49–53）：CTO 通道恒用 "CTO packet code" 型；DTO 用后 4 型之一，**由 Slave 经 GET_DAQ_PROCESSOR_INFO 响应 DAQ_KEY_BYTE 的位 7:6 宣告（Table 143），Master 发 STIM 必须使用同一型**：

| 型 | 内容 | 说明 |
|---|---|---|
| "CTO packet code" | 仅 PID | CTO 专用 |
| "absolute ODT number" | 仅 PID = 绝对 ODT 号 | 每个 DAQ list 的 ODT 相对号 + 该 list 的 FIRST_PID 映射成全局唯一绝对号：`绝对号 = FIRST_PID(list) + 相对号`（FIRST_PID 由 START_STOP_DAQ_LIST 响应给出，PL §7.1.2.1、§7.5.4.4） |
| "relative ODT number + absolute DAQ list number (BYTE)" | PID(相对 ODT 号) + DAQ_LIST_NUMBER(1 字节) | list 数 ≤255 |
| "relative ODT number + absolute DAQ list number (WORD)" | PID + DAQ_LIST_NUMBER(WORD) | list 数 >255 场景 |
| "…(WORD, aligned)" | 同上 + 1 个 FILL 字节凑对齐 | 追求性能时的对齐变体 |

> Table 143 另注：`DAQ_PROPERTIES.PID_OFF_SUPPORTED=1` **且** 识别字段型为 "absolute ODT number" 时，才允许用 SET_DAQ_LIST_MODE 的 PID_OFF 关掉 DTO 识别字段（见 4.5）；其他型不允许关。

补充规则：

- ODT 相对编号**每个 DAQ list 从 0 重新开始**，因此相对号在 Slave 全局不唯一（PL Table 14）——这是"绝对 ODT 号"型存在的原因；
- `PID_OFF_SUPPORTED`（DAQ_PROPERTIES 标志）允许某 list 的 DTO **不带识别字段**，前提是采用"绝对 ODT number"型且由传输层补足唯一识别（如 CAN 上每 list 独立 CAN-ID；PL §7.1.2.1 "Empty Identification Field"）。

### 2.5 时间戳字段（PL §7.1.2.3）

- DTO 可带时间戳 TS（紧跟计数器字段或识别字段之后）；支持性看 GET_DAQ_PROCESSOR_INFO 的 `TIMESTAMP_SUPPORTED`；
- **只在每个 DAQ 周期的第一个 ODT 的 DTO 包携带**，同轮后续 ODT 包不带（PL §7.1.2.3 与 Figure 58）；STIM 方向同样允许 Master 给每轮首包带 TS，但**字段内容规范不定义**（PL §7.1.2.3）；
- Slave 的同步数据传输时钟是**自由运行计数器，从不复位**；TS 以 BYTE/WORD/DWORD 传输，位宽与刻度由 GET_DAQ_RESOLUTION_INFO 的 `TIMESTAMP_MODE` / `TIMESTAMP_TICKS` 告知；
- `TIMESTAMP_FIXED` 置位时 Slave 恒定发 TS，Master 关不掉；否则 Master 用 SET_DAQ_LIST_MODE 的 `TIMESTAMP` 标志开启。

### 2.6 命令分层（PL §7.4）

- **Level-0**：单字节命令码 `0xC1–0xFF`；**Level-1**：双字节 `0xC0 + 子命令码`（层级无语义差别，只是编码空间不够）。
- 必选性表述规范原文规则：
  - 未实现的 optional 命令 → 回 `ERR_CMD_UNKNOWN(0x20)` 且无任何副作用（PL §7.4）；不支持某资源时，该资源的"必选"命令也按 optional 对待同样回 ERR_CMD_UNKNOWN；
  - **实现了 GET_SEED 即要求 UNLOCK 流程**（受保护资源访问先解锁）；
  - **实现了 SET_CAL_PAGE 就必须同时实现 GET_CAL_PAGE**（PL §7.4 原文 "If SET_CAL_PAGE is implemented, GET_CAL_PAGE is required"）。
- 完整码表见附录 B；下文只展开最小闭环用到的命令。

### 2.7 CONNECT：一切的入口（PL §7.5.1.1）

**请求**（`0xFF`，mandatory）：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xFF` |
| 1 | BYTE | Mode：`0`=Normal，`1`=user-defined |

**正响应**（Table 38）：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xFF` |
| 1 | BYTE | RESOURCE 资源位图 |
| 2 | BYTE | COMM_MODE_BASIC |
| 3 | BYTE | MAX_CTO |
| 4 | WORD | MAX_DTO |
| 6 | BYTE | PL 版本（仅 Major） |
| 7 | BYTE | TL 版本（仅 Major） |

**RESOURCE 位图**（Table 39/40）：bit4=DBG、bit3=PGM、bit2=STIM、bit1=DAQ、bit0=CAL/PAG。某位=1 表示资源可用，且该资源的必选命令必须得到支持。注意：资源位=1 不代表此刻可访问——Slave 内 XCP Handler 暂时拿不到 ECU 资源时直接回 `ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE(0x33)`（PL §7.1.1），稍后重试。

**COMM_MODE_BASIC**（Table 41/42）：bit0=`BYTE_ORDER`（0=Intel 小端，1=Motorola 大端，**多字节参数按此序传输**）；bit2:1=`ADDRESS_GRANULARITY`（00=BYTE/AG1、01=WORD/AG2、10=DWORD/AG4）；bit7=`OPTIONAL`（是否支持扩展可选命令）；bit6=`SLAVE_BLOCK_MODE`。

**MAX_CTO / MAX_DTO 约束**：

- 二者都必须是 AG 的整数倍（PL §7.5.1.1：`MAX_CTO mod AG == 0`）；
- 参数长度按 AG 计、协议数据流按字节计；
- Ethernet 上规范范围（TL §4.4）：`MAX_CTO ∈ [0x08, 0xFF]`（BYTE），`MAX_DTO ∈ [0x0008, 0xFFFF]`（WORD）。

### 2.8 GET_STATUS / GET_VERSION / 事件与服务（PL §7.5.1.4、§7.5.1.2、§7.2–7.3）

**GET_STATUS**（`0xFD`，mandatory）：回 Slave 当前全部会话状态。正响应：字节 0=`0xFF`、字节 1=session 状态位图、字节 2=资源保护位图（Table 48）。

- 响应字节 3=STATE_NUMBER、字节 4=Session configuration id（WORD）；字节 1 session 位图（Table 49）：bit7=`RESUME`、bit6=`DAQ_RUNNING`、bit5=`x`、bit4=`DAQ_CFG_LOST`、bit3=`CLEAR_DAQ_REQ`、bit2=`STORE_DAQ_REQ`、bit1=`CAL_PAG_CFG_LOST`、bit0=`STORE_CAL_REQ`；
- `*_CFG_LOST` 位=1 表示 Slave 侧配置已变（如 ECU 状态变化），此前的改动作废：Master 须先用 `SET_REQUEST` 的对应 `CLEAR_*` 标志清位并重新初始化，**否则直接用受影响功能 → Slave 断开**（PL §7.5.1.4 注）；
- 资源保护位图：位序同 CONNECT 的 RESOURCE；某位=1 表示该资源组当前**受保护，未解锁**（访问其受保护命令前须 GET_SEED+UNLOCK，否则回 `ERR_ACCESS_LOCKED`）。标准命令组永不受保护。

**GET_VERSION**（`0xC0 0x00`，optional）：见 1.3。

**事件码**（PL §7.2 Table 22，EV 包字节 1）：

| 码 | 事件 | 含义 | 严重级 |
|---|---|---|---|
| `0x00` | EV_RESUME_MODE | Slave 以 RESUME 模式启动 | S0 |
| `0x01` | EV_CLEAR_DAQ | 非易失存储的 DAQ 配置已被清除 | S0 |
| `0x02` | EV_STORE_DAQ | DAQ 配置已存入非易失存储 | S0 |
| `0x03` | EV_STORE_CAL | 标定数据已存入非易失存储 | S0 |
| `0x05` | EV_CMD_PENDING | Slave 请求重启超时计时（长耗时命令） | S1 |
| `0x06` | EV_DAQ_OVERLOAD | DAQ 处理器过载（**采集溢出**，数据有丢失） | S1 |
| `0x07` | EV_SESSION_TERMINATED | Slave 主动终止会话 | S3 |
| `0x08` | EV_TIME_SYNC | 外部触发的时间戳分发（5.5 节 DAQ clock 通路） | S0 |
| `0x09` | EV_STIM_TIMEOUT | STIM 超时指示 | S0 |
| `0x0A/0x0B` | EV_SLEEP / EV_WAKE_UP | Slave 进入/离开 SLEEP 模式 | S1 |
| `0x0C` | EV_ECU_STATE_CHANGE | ECU 状态变化 | S0 |
| `0xFC/0xFD/0xFE/0xFF` | EV_DBG / EV_POD / EV_USER / EV_TRANSPORT | 调试/POD/用户自定义/传输层专用 | 见各自标准 |

事件实现均为 optional；**EV/SERV/DTO 不保证送达**（PL §7.1.1/§7.2）——工具侧要做容错。

### 2.9 DISCONNECT（`0xFE`，mandatory）

Master 主动发 → Slave 回 ack 后回 DISCONNECTED；Slave 侧断开条件见 TL §4.1（TCP socket 关闭即触发 XCP disconnect）。

### 2.10 错误包与错误处理（PL §7.6.3 Table 228）

负响应格式：字节 0=`0xFE`，字节 1=错误码。最小闭环常碰面的错误码：

| 码 | 名称 | 含义（规范原文要义） | 级 |
|---|---|---|---|
| 0x00 | ERR_CMD_SYNCH | 仅 SYNCH 的"正同步"应答（专用，见 2.11） | S0 |
| 0x10 | ERR_CMD_BUSY | 命令未执行（从机忙），稍后可重试 | S2 |
| 0x11 | ERR_DAQ_ACTIVE | DAQ 运行中拒绝该命令 | S2 |
| 0x20 | ERR_CMD_UNKNOWN | 未知命令/未实现 optional | S2 |
| 0x21 | ERR_CMD_SYNTAX | 命令语法错 | S2 |
| 0x22 | ERR_OUT_OF_RANGE | 语法对但参数越界 | S2 |
| 0x23 | ERR_WRITE_PROTECTED | 目标地址写保护 | S2 |
| 0x24 | ERR_ACCESS_DENIED | 目标地址不可访问 | S2 |
| 0x25 | ERR_ACCESS_LOCKED | 资源受保护，需 Seed&Key | S2 |
| 0x26 | ERR_PAGE_NOT_VALID | 页不存在 | S2 |
| 0x27 | ERR_MODE_NOT_VALID | 模式不存在 | S2 |
| 0x28 | ERR_SEGMENT_NOT_VALID | 段无效 | S2 |
| 0x29 | ERR_SEQUENCE | 命令序列错（动态 DAQ 配错顺序必遇） | S2 |
| 0x2A | ERR_DAQ_CONFIG | DAQ 配置无效 | S2 |
| 0x30 | ERR_MEMORY_OVERFLOW | Slave 资源不足（如 ALLOC 超容量、DOWNLOAD 超 MAX_CTO） | S2 |
| 0x33 | ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE | Slave 内 Handler 暂时访问不到资源（PL §7.1.1） | S2 |
| 0x34 | ERR_SUBCMD_UNKNOWN | 子命令未知 | S2 |

严重级（PL §7.6.1.4 Table 227）：**S0=Information、S1=Warning/Request、S2=Resolvable error、S3=Fatal error**；按 §5.3 只有 S3 使 Slave 转 DISCONNECTED，S0–S2 不改状态。收到 S2 错误后 Master 按 PL §7.6.3 的 Table 229–233 对应命令处理表（"Pre-Action/Action"列）执行；最小闭环基本碰不到 S3（ERR_VERIFY 属刷写场景）。

**Master 兜底策略**（PL §7.6.3）：表中未定义的 ERR 组合 → 查 Table 228 的严重度自行决定；多命令序列中途出错 → 由 Master 决定回退方式。

### 2.11 SYNCH：超时后的重新同步（PL §7.5.1.5）

超时（响应迟迟不来）后，处理流程在 PL §7.6.2：Master 对每条命令起计时器，上限 t1–t7 由 **A2L** 给出（§7.6.2 原文）。超时后执行 "Pre-Action + Action"，**允许共试 2 轮**；通常 Pre-Action=SYNCH、Action=重发原命令——SYNCH(`0xFC`) 的妙处是 Slave **恒**回 `ERR(0xFE)+ERR_CMD_SYNCH(0x00)`（PL Table 53/54），Master 由此确认 Slave 已回到就绪、可安全重发；若期间收到迟到的旧响应，识别为历史残留丢弃即可。注意特例：带内部指针状态的命令（SET_MTA/SET_DAQ_PTR 系的后续 UPLOAD/DOWNLOAD/WRITE_DAQ），Pre-Action 须**先重设指针再重发**（§7.6.2.1 原文）。

至此，命令层的"骨架"就绪。下面两章把骨架用起来。

---

## 第 3 章 标定最小闭环

### 3.1 概念先行：Sector / Segment / Page（PL §4.4.1）

标定操作的三个布局视角，务必分开理解：

| 概念 | 视角 | 用途 | 谁定义 |
|---|---|---|---|
| **Sector** | 物理布局 | 刷写（FLASH 分块擦写） | A2L SECTORS（本手册不涉及） |
| **Segment** | 逻辑布局 | "某一段标定数据在 ECU 存储里的位置与能力" | A2L SEGMENTS |
| **Page** | 访问布局 | **同一地址、不同属性**（不同值集、可写/只读），实现"改一页参数、另一页保底" | A2L PAGES + Slave 分页能力 |

- 地址空间是 **40 位**：32 位 `Address` + 8 位 `Address extension`（后文所有命令里的 ext 字节都是这个高 8 位，PL §4.4.1）。
- 每个 Segment 有**两个独立的活动页**：ECU 活动页（应用正在用的）与 XCP 活动页（XCP 驱动读写的）。工具改 XCP 页不影响应用，切 ECU 页才生效——这就是"双缓冲"标定的协议基础（PL §4.4.3）。
- 分页能力以资源位 `CAL/PAG` 宣告（CONNECT RESOURCE bit0）。

### 3.2 MTA：内存传送地址指针（PL §7.5.1.11 SET_MTA，`0xF6`，optional）

Upload/Download 系命令都不带地址，统一操作一个 Slave 内部指针——**MTA（Memory Transfer Address）**：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xF6` |
| 1–2 | BYTE | Reserved |
| 3 | BYTE | Address extension |
| 4 | DWORD | Address |

- 语义：把 MTA 指向给定地址；**后续访问类命令每次成功后，MTA 自动前移访问过的字节数**；
- 依赖 MTA 的命令：BUILD_CHECKSUM、UPLOAD、DOWNLOAD（含 _NEXT/_MAX）、MODIFY_BITS（不移动 MTA，见 3.5）、PROGRAM_*（本手册不涉及）；
- 设置 MTA 本身不校验可访问性——错误要到真正读写时才报（ERR_ACCESS_DENIED / ERR_WRITE_PROTECTED）。

### 3.3 读参数：UPLOAD 与 SHORT_UPLOAD（PL §7.5.1.12/13）

**UPLOAD**（`0xF5`，optional）——从 MTA 起读 n 个 ELEMENT（1 ELEMENT = AG 字节）：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xF5` |
| 1 | BYTE | n：标准模式 `[1..MAX_CTO/AG-1]`；块模式 `[1..255]` |

正响应（Table 71）：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xFF` |
| 1–AG-1 | BYTE | 仅 AG>1 时的对齐填充（"Used for alignment only"） |
| AG | ELEMENT | 第 1 个数据元素 |
| … | … | … |
| n·AG | ELEMENT | 第 n 个数据元素 |

> ⚠️ 解析要点：Table 71 规定第 1 个 ELEMENT 位于位置 **AG**（第 i 个 ELEMENT 位于 i·AG）——AG=1 时位置 1 即数据；AG=2 时位置 1 是"仅用于对齐"的填充、位置 2 起数据；AG=4 时位置 1–3 为填充、位置 4 起数据。响应内数据按 BYTE_ORDER 声明的字节序排列。

- MTA 自动前移 n·AG；
- **块模式**（Slave `SLAVE_BLOCK_MODE`=1 时）：单条 UPLOAD 可跨多个响应包，包数 = `((n·AG)-1)/(MAX_CTO-AG)+1`（PL §7.5.1.12 公式），后续包由 Slave 连续发出、Master 无需再发命令——**以太网下块传输模型是 optional 且默认不用**（TL §4.2），常规实现按标准模式即可。

**SHORT_UPLOAD**（`0xF4`，optional）——免 SET_MTA 版，带绝对地址：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xF4` |
| 1 | BYTE | n `[1..MAX_CTO/AG-1]` |
| 2 | BYTE | Reserved |
| 3 | BYTE | Address extension |
| 4 | DWORD | Address |

响应结构与 UPLOAD 相同；完成后 **MTA 指向读回数据块之后**（PL Table 72 注）。块模式下禁止使用 SHORT_*（PL §7.5.1.13）。工具"按地址读一个浮点"的首选就是它。

### 3.4 写参数：DOWNLOAD 与 SHORT_DOWNLOAD（PL §7.5.2.1/4）

**DOWNLOAD**（`0xF0`，CAL/PAG 资源的必选命令）——把 n 个 ELEMENT 写入 MTA 并前移：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xF0` |
| 1 | BYTE | n：标准模式 `[1..(MAX_CTO-2)/AG]`；块模式 `[1..min(MAX_BS·(MAX_CTO-2)/AG, 255)]` |
| 2–AG-1（AG=1）或 AG-1 起（AG>1） | BYTE | 对齐填充（仅 AG>2 时有，Table 82） |
| AG=1: 2 / AG>1: AG | ELEMENT | 第 1 个数据元素 |
| … | … | 共 n 个 ELEMENT |

- **要么全写要么全不写**：Slave 内存不足以完成本次 n 时回 `ERR_MEMORY_OVERFLOW`，**不写任何数据**（PL §7.5.2.1）；
- 块模式：DOWNLOAD 后跟 `DOWNLOAD_NEXT(0xEF)` 续传、仅最后一包回 ack；节拍参数 MAX_BS/MIN_ST 来自 GET_COMM_MODE_INFO——以太网下同样按 optional 处理，最小闭环用标准模式；
- 例（AG=1、BYTE_ORDER=Intel）：把地址 `0x80001234` 处的 float 改成 1.0（`0x3F800000`）：

```
CMD : F0 04 00 00 80 3F          ; DOWNLOAD n=4, 数据 = 00 00 80 3F（Intel 序）
RES : FF
（前置：SET_MTA F6 00 00 00 34 12 00 80）
```

**SHORT_DOWNLOAD**（`0xED`，optional）——免 SET_MTA、带绝对地址：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xED` |
| 1 | BYTE | n `[0..(MAX_CTO-8)/AG]` |
| 2 | BYTE | Reserved |
| 3 | BYTE | Address extension |
| 4 | DWORD | Address |
| 8 起 | ELEMENT | n 个数据元素 |

正响应：常规命令响应 `[FF]`（PL §7.5.2.4 未定义额外响应内容）；写入后 **MTA 指向下载数据块之后的第一个数据元素**（Table 86 注原文）。n 上限里的 `-8` 是命令头占位；n 可为 0（此时按 MTA 规则数据块长度为 0）。注意 MAX_CTO=8 时本命令传不了任何数据字节（PL 注，CAN 场景）。

### 3.5 按位改一个开关：MODIFY_BITS（PL §7.5.2.5，`0xEC`，optional）

对 MTA 所指 **32 位单元**做原子"与-异或"修改（适合 bit 型标定变量，避免读-改-写竞态）：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xEC` |
| 1 | BYTE | S = 位移量 Shift |
| 2 | WORD | MA = AND 掩码 |
| 4 | WORD | MX = XOR 掩码 |

公式（PL §7.5.2.5 原文）：`A := ((A) & ~(((dword)(~MA)) << S)) ^ (MX << S)`，执行后 **MTA 不变**。规范算例：要清 bit30、置 bit16 → `S=16, MA=0xBFFE, MX=0x0001`。

### 3.6 页切换（PL §7.5.3，全部 optional；实现了就必须成对）

**SET_CAL_PAGE**（`0xEB`）：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xEB` |
| 1 | BYTE | Mode（位掩码） |
| 2 | BYTE | 逻辑 Segment 号 |
| 3 | BYTE | 逻辑 Page 号 |

Mode 位掩码（Table 89/90）：**bit0=ECU**（应用改用该页）、**bit1=XCP**（XCP 驱动改用该页，两标志可同时/分开置位）、**bit7=ALL**（忽略段号，对所有段生效）。失败错误码：模式不行→`ERR_MODE_NOT_VALID`；页不存在→`ERR_PAGE_NOT_VALID`；段无效→`ERR_SEGMENT_NOT_VALID`。

**GET_CAL_PAGE**（`0xEA`）：请求 `[EA, AccessMode(0x01=ECU/0x02=XCP), Segment]`；正响应 `[FF, resv, resv, Page]`（字节 3=当前页）。

段/页能力枚举（写死页号之前先问 Slave）：`GET_PAG_PROCESSOR_INFO(0xE9)`→`GET_SEGMENT_INFO(0xE8)`→`GET_PAGE_INFO(0xE7)` 三级信息命令，均为 optional（PL §7.5.3.3–3.5）；段冻结用 `SET_SEGMENT_MODE(0xE6)`/`GET_SEGMENT_MODE(0xE5)`；页间拷贝 `COPY_CAL_PAGE(0xE4)`（把 XCP 页写入 ECU 页或反向，"Master-Slave action" 类换页动作的基础，PL §7.5.3.8、§4.4.7）。

### 3.7 解锁：Seed & Key（PL §7.5.1.9/10，optional；GET_SEED 一实现即必走）

规则（PL §7.4、§7.5.1.9）：

- Slave 支持则受保护资源先 GET_SEED 拿种子、再 UNLOCK 送钥匙；**一条命令只能申请一个资源**，多资源要跑多轮 (GET_SEED+UNLOCK)；同时申请 0 个或多个 → `ERR_OUT_OF_RANGE`；
- GET_SEED 响应 Length=0 → 资源本来就没保护，**无需 UNLOCK**；
- Key 算法在 A2L 外挂的**外部函数文件**里：A2L `PROTOCOL_LAYER` 段的 `SEED_AND_KEY_EXTERNAL_FUNCTION` 参数给出文件名（ASCII，含扩展名、**不含路径**）；一个文件可含 1–5 个算法（每受保护资源一个），导出两个函数 `XCP_GetAvailablePrivileges`（回可用资源掩码）与 `XCP_ComputeKeyFromSeed`（Seed→Key），参数表见 PL §9.1/§9.2；
- **字节序纪律**（PL §9.1 注）：外部函数收到/返回的 BYTE 数组就是 XCP 报文里的原始传输顺序——即使工具跑在 Intel 平台而 ECU 是 Motorola 格式，Master 也**不得**替算法做字节序换算；
- 解锁失败重新 CONNECT 后，前一轮的 Seed/Key 状态全部作废，须从 GET_SEED 重来（结合上表序列约束理解）。

| 命令 | 码 | 请求字节 | 正响应 |
|---|---|---|---|
| GET_SEED | `0xF8` | `[F8, Mode(0=取首段种子/1=续取余段), Resource(仅 Mode=0 有效，Table 65)]` | `[FF, Length, Seed…至 MAX_CTO-1]`；Length=0 → 资源未受保护、无需 UNLOCK；种子总长 > MAX_CTO-2 时以 Mode=1 续取（无前置 Mode=0 直接 Mode=1 → `ERR_SEQUENCE`） |
| UNLOCK | `0xF7` | `[F7, Length, Key…至 MAX_CTO-1]`；**首包 Length=密钥总长**，总长 > MAX_CTO-2 时后续包 Length=剩余长度（Table 67，**无 Mode 字节**） | `[FF, 当前资源保护位图]`（字节 1 = Current Resource Protection Mask，语义同 GET_STATUS 字节 2，Table 68） |

序列约束（PL §7.5.1.10 原文）：

- 只有先跑过 GET_SEED 序列才能发 UNLOCK；违反序列 → `ERR_SEQUENCE`；
- **密钥校验失败 → 回 `ERR_ACCESS_LOCKED`，且 Slave 直接进入 disconnected 状态**——解锁失败后必须重新 CONNECT，而不是重试 UNLOCK；
- 受保护命令未解锁就访问 → `ERR_ACCESS_LOCKED`；
- 动手前可先看 GET_STATUS 响应字节 2，判断哪些资源当前受保护（2.8），省一轮种子往返。

### 3.8 端到端标定流水（把上面串起来）

场景：读地址 `0x8000_1234` 的 4 字节参数，改成 1.0，确认，切页生效。假设 Slave：AG=1、Intel 序、MAX_CTO=16、CAL/PAG 受保护、分页支持。

```
Master                                   Slave (XCP over Ethernet)
  │ CONNECT FF 00                        │
  │ ◄──────────── FF [RESOURCE:CAL/PAG] [AG=1,Intel] [MAX_CTO=16] [MAX_DTO…] [1][1]   ← 字节6/7=PL/TL 仅 Major
  │ GET_STATUS FD                        │      ← 查会话 + 资源保护位图(此例 CAL/PAG=受保护)
  │ GET_SEED F8 00 01 (resource=CAL/PAG) │      ← Mode=0, 申请 CAL/PAG
  │ ◄── FF Len Seed…                     │
  │ UNLOCK F7 KeyLen Key…                │      ← Key 由外部函数文件算出；KeyLen=密钥总长
  │ ◄──────────────── FF 00              │      ← 字节1=资源保护位图：0x00=已无保护
  │ SHORT_UPLOAD F4 04 00 ext addr      │      ← 免 MTA 直接读
  │ ◄── FF 00 00 80 3F                   │      ← 旧值 1.0f 的 4 字节
  │ SHORT_DOWNLOAD ED 04 00 ext addr 00 00 80 3F │
  │ ◄── FF                               │      ← 标准响应（§7.5.2.4 无额外响应内容）
  │ SHORT_UPLOAD F4 04 00 ext addr      │      ← 回读校验（工程惯例，非规范要求）
  │ ◄── FF 00 00 80 3F                   │
  │ SET_CAL_PAGE EB 01 00 01            │      ← Mode=ECU(bit0)：段0 应用切到页1
  │ ◄──────────────── FF                 │
  │ GET_CAL_PAGE EA 01 00               │
  │ ◄── FF 00 00 01                      │      ← 确认 ECU 活动页=1
  │ DISCONNECT FE                        │
```

> 等价 MTA 路线（老工具风格）：`SET_MTA → UPLOAD/DOWNLOAD 循环`；SHORT_* 路线（新工具首选）：免指针状态、可并发不同地址。两条都在规范内，选哪条不影响协议合法性。

### 3.9 标定常见坑

1. **AG 与对齐**：SHORT_DOWNLOAD 数据固定在字节 8 起（头长 8 字节决定），UPLOAD 响应数据起位=AG——AG=2/4 时填错位会全盘错；
2. **字节序只声明一次**：CONNECT COMM_MODE_BASIC.bit0 之后全程有效，不要在命令中途假设可改；
3. **块模式与 SHORT_* 互斥**（PL §7.5.1.13/§7.5.2.4）；
4. **资源瞬不可用**：RESOURCE 位=1 也不保证此刻可访问——Slave 内 XCP Handler 暂时拿不到 ECU 资源时直接回 `ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE`（PL §7.1.1/§7.4），稍后重试；
5. **切页失败三连**：区分 0x26/0x27/0x28 三个错误码定位是页/模式/段的问题；
6. **MTA/UPLOAD/SHORT_UPLOAD/SHORT_DOWNLOAD/MODIFY_BITS 在 V1.5 全是 optional**（PL §7.5.1.11–13、§7.5.2.4/5 节头 Category），DOWNLOAD 才是 Calibration 组唯一 mandatory——没有哪条路线保证可用，健壮工具先探测（`ERR_CMD_UNKNOWN` → 换路线），见 2.6。

---

## 第 4 章 测量最小闭环

### 4.1 概念栈：ODT Entry → ODT → DAQ List → 事件通道（PL §4.1.1–4.1.5）

测量的协议机制是 **DAQ（Data Acquisition）**，反方向叫 **STIM（STIMulation）**，两者共用同一套配置对象，方向由 list 的 `DIRECTION` 标志决定（PL §7.5.4.3 Table 118）。自下而上：

```
ODT Entry  ── 引用一个数据元素：{Address(32bit), Address ext(8bit), Size[AG], BIT_OFFSET}
   │ 若干条聚合
   ▼
ODT（Object Descriptor Table）      ── 一次采样的"元素清单"；DTO 包与 ODT 一一对应
   │ 若干条组成
   ▼
DAQ List                            ── ODT 的集合；配置、启停的基本单位
   │ 绑定
   ▼
事件通道（Event Channel）            ── 发送节拍源：每来一个事件，该通道挂的 DAQ list 各发一轮 DTO
```

- **每个 DAQ list 的 ODT 相对编号从 0 重新开始**（PL Table 14），全局唯一要靠 FIRST_PID 映射（2.4 节）；
- 事件通道由 Slave 定义（A2L EVENT_CHANNEL + GET_DAQ_EVENT_INFO 枚举），通道属性含 `MAX_DAQ_LIST`（`0xFF`=不限、`0x00`=暂不可用）与优先级（`0xFF` 最高）；PRESCALER 只对 DAQ 方向有效（PL §4.1.5）；
- 资源宣告：CONNECT RESOURCE **bit1=DAQ**、bit2=STIM；该资源的 **basic 必选命令**（SET_DAQ_PTR/WRITE_DAQ/SET_DAQ_LIST_MODE/START_STOP_DAQ_LIST/START_STOP_SYNCH）与 **static 必选命令**（CLEAR_DAQ_LIST）必须支持（PL §7.4）。动态配置的 FREE_DAQ/ALLOC_DAQ/ALLOC_ODT/ALLOC_ODT_ENTRY 四条，PL §7.4 码表标 "mandatory for DAQ/STIM"、各命令节头（§7.5.4.18 起）却标 "dynamic, optional"——规范内部两种口径，工程上按"**宣告支持动态模式则必选**"处理即可（静态 Slave 用不到它们）。

### 4.2 静态 vs 动态 DAQ 配置（PL §4.1.6）

Slave 的 DAQ 配置模式由 **GET_DAQ_PROCESSOR_INFO 响应中 DAQ_PROPERTIES 的 `DAQ_CONFIG_TYPE` 标志**宣告：

| | 静态（Static） | 动态（Dynamic） |
|---|---|---|
| list 从哪来 | Slave 预定义（PREDEFINED），只允许改内容 | Master 用 ALLOC 系命令现场创建 |
| 数量上限（IF_DATA 参数） | MAX_DAQ / MAX_ODT / MAX_ODT_ENTRIES | MIN_DAQ+DAQ_COUNT、MAX_DAQ_TOTAL、MAX_ODT_DAQ_TOTAL、MAX_ODT_ENTRIES_TOTAL…（PL Table 1） |
| 可配置 list 号段 | — | `[MIN_DAQ, MIN_DAQ+DAQ_COUNT-1]` |

- "可配置"list 里 WRITE_DAQ/ALLOC_* 只对号段 `[MIN_DAQ, MAX_DAQ-1]` 生效（PL §7.5.4.2/§7.5.4.20）；预定义 list 直接 SET_DAQ_PTR/WRITE_DAQ 改条目；
- **动态配置不能增量追加**：要改任何结构（增删 list/ODT/条目）必须 FREE_DAQ 全部清掉后整体重配（PL §4.1.6）。

### 4.3 动态配置四步曲（PL §7.5.4.18–21，均为动态模式的必选序列）

**① FREE_DAQ**（`0xD6`）：清掉所有 DAQ list 及动态分配的 ODT/条目。请求就一个字节：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xD6` |

> 动态配置序列**永远以 FREE_DAQ 开头**（PL §7.5.4.18 原文）。

**② ALLOC_DAQ**（`0xD5`）：申请 list 数量。

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xD5` |
| 1 | BYTE | Reserved |
| 2 | WORD | DAQ_COUNT（要分配的 list 数） |

**③ ALLOC_ODT**（`0xD4`）：**逐个 DAQ list** 申请 ODT 数。

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xD4` |
| 1 | BYTE | Reserved |
| 2 | WORD | DAQ_LIST_NUMBER `[MIN_DAQ, …, MIN_DAQ+DAQ_COUNT-1]` |
| 4 | BYTE | ODT_COUNT |

**④ ALLOC_ODT_ENTRY**（`0xD3`）：**逐个 ODT** 申请条目数。

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xD3` |
| 1 | BYTE | Reserved |
| 2 | WORD | DAQ_LIST_NUMBER（同上号段） |
| 4 | BYTE | ODT_NUMBER（该 list 内相对号） |
| 5 | BYTE | ODT_ENTRIES_COUNT |

序列硬规则（三条命令说明文字里原文相同）：

- 跳步（如 FREE_DAQ 后直接 ALLOC_ODT、ALLOC_ODT 后直接 ALLOC_DAQ…）一律 `ERR_SEQUENCE(0x29)`；
- Slave 内存不够 → `ERR_MEMORY_OVERFLOW(0x30)`，**整轮配置作废**（PL §4.1.6）；
- 合法顺序矩阵见 PL §7.5.4 各命令节（FREE→ALLOC_DAQ→ALLOC_ODT(×list)→ALLOC_ODT_ENTRY(×ODT)，同层可重复）。

### 4.4 把变量填进 ODT：SET_DAQ_PTR + WRITE_DAQ（PL §7.5.4.1/2，必选）

**SET_DAQ_PTR**（`0xE2`）初始化"DAQ list 指针"：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xE2` |
| 1 | BYTE | Reserved |
| 2 | WORD | DAQ_LIST_NUMBER |
| 4 | BYTE | ODT_NUMBER（list 内相对号） |
| 5 | BYTE | ODT_ENTRY_NUMBER（ODT 内相对号） |

**WRITE_DAQ**（`0xE1`）把指针处的 ODT Entry 写成目标变量：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xE1` |
| 1 | BYTE | BIT_OFFSET：`0x00–0x1F`=位元素（位号，元素值 0/≠0 映射位状态，且 Size 必须恰等于 GRANULARITY_ODT_ENTRY_SIZE_x）；**`0xFF`=普通数据元素，本字节忽略** |
| 2 | BYTE | Size of DAQ element `[AG]`，`0 ≤ size ≤ MAX_ODT_ENTRY_SIZE_x` |
| 3 | BYTE | Address extension |
| 4 | DWORD | Address |

- 写成功后指针**在同一 ODT 内自增**；写到 ODT 最后一个条目后指针值**未定义**——跨 ODT/跨 list 必须重新 SET_DAQ_PTR（PL §7.5.4.2 原文）；
- 条目的地址/尺寸受粒度约束（PL §4.1.2 公式）：
  ```
  GRANULARITY_ODT_ENTRY_SIZE_x ∈ {1,2,4,8} 且 ≥ AG
  Address[AG] mod (GRANULARITY[BYTE]/AG) = 0
  Size[AG]    mod (GRANULARITY[BYTE]/AG) = 0
  Size[AG]    ≤ MAX_ODT_ENTRY_SIZE_x[AG]
  ```
  即：条目要按 Slave 的 ODT 条目粒度对齐；Slave 只支持 BYTE 粒度时，Master 必须把多字节变量拆成单字节条目（PL §4.1.2 末段）。

### 4.5 节拍、方向与时间戳：SET_DAQ_LIST_MODE（PL §7.5.4.3，必选）

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xE0` |
| 1 | BYTE | Mode 位掩码（下表） |
| 2 | WORD | DAQ_LIST_NUMBER |
| 4 | WORD | **事件通道号**（决定发送节拍，见 4.1） |
| 6 | BYTE | Transmission rate prescaler（≥1，仅 DAQ 方向） |
| 7 | BYTE | DAQ list 优先级（`0xFF` 最高） |

Mode 位掩码（Table 117/118）：bit0=`ALTERNATING`、bit1=`DIRECTION`（0=DAQ 采集，1=STIM 刺激）、bit3=`DTO_CTR`（DTO 带计数器字段，用于丢包检测/刺激一致性，见 2.4 与 PL §4.3.5.4）、bit4=`TIMESTAMP`、bit5=`PID_OFF`。

规范硬约束（§7.5.4.3 原文）：

- `TIMESTAMP`+`ALTERNATING` 不得同时置位；`ALTERNATING` 只能在设 DAQ 方向时设置；
- Slave `TIMESTAMP_FIXED` 时 Master 试图关时间戳 → `ERR_CMD_SYNTAX`；
- `PID_OFF=1` 仅当识别字段型为"绝对 ODT number"才允许；
- packed 模式的 list 绑到不支持 PACKED 的事件通道、或 packed 但没开 TIMESTAMP → `ERR_DAQ_CONFIG`（§4.9）。

### 4.6 启停：START_STOP_DAQ_LIST + START_STOP_SYNCH（PL §7.5.4.4/5，必选）

**START_STOP_DAQ_LIST**（`0xDE`）单 list 操作：

| 位置 | 类型 | 内容 |
|---|---|---|
| 0 | BYTE | `0xDE` |
| 1 | BYTE | Mode：`00`=stop、`01`=start、`02`=**select**（配置入"被选中"集合，不启动） |
| 2 | WORD | DAQ_LIST_NUMBER `[0..MAX_DAQ-1]`（预定义+可配置都适用） |

正响应 `[FF, FIRST_PID]`（Table 120）：识别字段型为"绝对 ODT number"时，`绝对ODT号 = FIRST_PID(list) + 相对ODT号`；"相对号+list号"型则 FIRST_PID 可忽略。**任一 list 启动后 Slave 进入数据传输态，GET_STATUS 的 `DAQ_RUNNING` 置位**（§7.5.4.4 原文）。

**START_STOP_SYNCH**（`0xDD`）批量同步：请求仅 `[DD, Mode]`：`00`=停全部、`01`=启动所有"被选中"的、`02`=停所有"被选中"的、`03`=为启动做准备（prepare；置位后 Master 不得再改这些 list 的配置，不支持时回 `ERR_MODE_NOT_VALID`）。select 标志在对应 SYNCH/SET_REQUEST 被 ack 后由 Slave 自动清除（§7.5.4.4/5）。

**典型用法**：多条 list 同时钟起跑 = 对每条 `START_STOP_DAQ_LIST(select)` → 一次 `START_STOP_SYNCH(01)`；急停 = `START_STOP_SYNCH(00)`。

### 4.7 收到 DTO 怎么解码

一轮采集（某个事件触发、某个 list 的一个 DAQ cycle）= 该 list 的**每个 ODT 一个 DTO 包**，包序不要求升序但必须齐全（PL §7.1.2.1 "need not be in ascending order but of course complete"）。解码步骤：

1. 读 PID：按 Slave 宣告的识别字段型（GET_DAQ_PROCESSOR_INFO `DAQ_KEY_BYTE`）分支——
   - 型"绝对 ODT number"：`相对ODT号 = PID − FIRST_PID(该list)`（FIRST_PID 在启动该 list 时已拿到）；
   - 型"相对号+list 号"：PID=相对 ODT 号，紧跟的 1/2 字节 = 绝对 list 号；
2. （可选）计数器字段：`DTO_CTR=1` 时 PID 后跟 CTR 字节，用于连续性检查（丢包检测）；
3. （可选）时间戳：`TIMESTAMP=1` 时**仅该 list 本轮第一个 ODT 的 DTO** 带 TS（2.5）；
4. 数据字段 = 该 ODT 各条目按写入顺序拼的原始样本值（字节序/AG 同会话约定）。

溢出：Slave 来不及发时发 `EV_DAQ_OVERLOAD(0x06)` 事件（2.8），数据有丢失——工具必须能报"本轮不完整"。

### 4.8 DAQ PACKED MODE（V1.5 引入，PL §4.1.10，全 optional）

常规模式下每个 DTO 装"每个条目一个样本"。高速变量（如 100 kHz 采样）可把**多个样本打包进一个 DTO** 降低发送频率：

- 支持性：GET_DAQ_LIST_INFO 响应 DAQ_LIST_PROPERTIES 的 `PACKED` 标志；
- 配置：`SET_DAQ_PACKED_MODE(0xC0,0x01)` / `GET_DAQ_PACKED_MODE(0xC0,0x02)`（Level-1，optional），参数三件套 `DAQ_PACKED_MODE`（**element-grouped**=同一元素连续样本相邻 / **event-grouped**=同一事件所有元素样本轮转排列）、`DPM_SAMPLE_COUNT`（每 DTO 打包样本数）、`DPM_TIMESTAMP_MODE`（时间戳指首样本还是末样本）；
- 约束：事件通道须支持 PACKED 且该 list 开 TIMESTAMP，否则 `ERR_DAQ_CONFIG`（§4.5）；event-grouped 只混排**同一 ODT 内**元素，不跨 ODT 混（PL §4.1.10）。

规范例子（element-grouped，DPM_SAMPLE_COUNT=4）一帧 DTO：

```
| PID0 | A0 A1 A2 A3 | B0 B1 B2 B3 | C0 C1 C2 C3 | D0 …   ← 同元素连续样本聚簇
```

### 4.9 端到端测量流水（动态 DAQ 最小闭环）

场景：采集 `0x2000_0000` 的 float 变量 + `0x2000_0100` 的 uint16，绑到事件通道 1，开时间戳；Slave 宣告 DAQ_CONFIG_TYPE=动态、AG=1、Intel、"绝对 ODT number"型识别字段。

```
Master                                     Slave
  │ … CONNECT/GET_VERSION/GET_DAQ_PROCESSOR_INFO(0xDA) …
  │ FREE_DAQ D6                            │
  │ ◄── FF                                 │
  │ ALLOC_DAQ D5 00 01 00                  │   ← 申请 1 条 list（DAQ_COUNT=1）
  │ ◄── FF                                 │
  │ ALLOC_ODT D4 00 [list=MIN_DAQ] 02      │   ← 该 list 2 个 ODT
  │ ◄── FF                                 │
  │ ALLOC_ODT_ENTRY D3 00 [list] 00 02     │   ← ODT0 两个条目
  │ ALLOC_ODT_ENTRY D3 00 [list] 01 01     │   ← ODT1 一个条目
  │                                        │
  │ SET_DAQ_PTR E2 00 [list] 00 00         │   ← 指向 ODT0 条目0
  │ WRITE_DAQ E1 FF 04 00 00 00 00 20      │   ← float@0x20000000（size=4，BIT_OFFSET=FF）
  │ SET_DAQ_PTR E2 00 [list] 00 01         │
  │ WRITE_DAQ E1 FF 02 00 00 00 01 20      │   ← uint16@0x20000100
  │ SET_DAQ_PTR E2 00 [list] 01 00         │
  │ WRITE_DAQ E1 FF 04 00 …                │   ← 第三个元素…
  │                                        │
  │ SET_DAQ_LIST_MODE E0 10 [list] 01 00 01 FF │ ← TIMESTAMP(bit4)；事件通道1；PRESCALER=1；最高优先级
  │ START_STOP_DAQ_LIST DE 01 [list]       │   ← 直接 start（单 list 无需 select+SYNCH）
  │ ◄── FF FIRST_PID                       │   ← 记下 FIRST_PID 用于解码
  │                                        │  ══ 事件通道 1 每触发一次 ══
  │ ◄══ DTO: PID=FIRST_PID   TS DWORD A B  ══  ← ODT0 样本（带时间戳，本轮首包）
  │ ◄══ DTO: PID=FIRST_PID+1 C D E         ══  ← ODT1 样本（无 TS）
  │                                        │
  │ START_STOP_DAQ_LIST DE 00 [list]       │   ← 停
  │ ◄── FF                                 │
```

要点复盘：**FREE→ALLOC×N→PTR/WRITE×N→MODE→START** 是唯一合法序；配置类命令在 DAQ 运行中会被 `ERR_DAQ_ACTIVE(0x11)` 拒绝（2.10），改结构必先停。

### 4.10 测量常见坑

1. **忘了 FIRST_PID 就解不了码**：动态配置的 list 启动响应里的 FIRST_PID 是绝对 ODT 号基准，多 list 时各记各的；
2. **指针跨 ODT 不自增**：WRITE_DAQ 只在 ODT 内自增，ODT 末尾后指针未定义（4.4）——批量写必须显式 SET_DAQ_PTR；
3. **粒度公式别只记概念**：地址不按 GRANULARITY_ODT_ENTRY_SIZE_x 对齐，轻则 ERR_OUT_OF_RANGE 重则采到错位数据（4.4 公式）；
4. **改配置先停流**：运行中改 list 结构 → ERR_DAQ_ACTIVE；动态模式下没有"增量改"，只有 FREE 全重配（4.2）；
5. **时间戳只在首包**：给全部 DTO 找 TS 会解出脏数据；时间对齐按"首包 TS + 样本序号×周期"推（4.7）；
6. **UDP 场景容忍丢包**：DTO 不保证到达（2.8），带宽核算与容错见 5.9。
---

## 第 5 章 以太网传输层：同一套 XCP，跑在 TCP 和 UDP 上

> 本章全部依据 TL V1.5（ASAM-AE-MCD-1 XCP on Ethernet TL V1.5.0）；节号记作 TL §x。

### 5.1 寻址与连接语义——TCP / UDP 的第一组差异（TL §4.1）

Slave 由 **IP 地址（V4 或 V6）+ 端口号**寻址。两种承载的连接模型完全不同，这是写工具前必须想清楚的事：

| | TCP/IP（TL §4.1.1） | UDP/IP（TL §4.1.2） |
|---|---|---|
| 谁发起 | Slave 是 **listener**（被动监听），Master 主动建连 | 无连接概念；CONNECT 是 **XCP 层命令**，不是传输层握手 |
| 连接数 | **同一时刻只接受 1 个连接** | Slave 对每个发来 CONNECT 的地址各记一份 |
| 地址锁定 | 面向字节流，天然点对点 | **未连接**：应答任何来源的 CONNECT，把响应发回其 IP:port；**已连接**：只响应"发 CONNECT 的那个 IP"的报文（**端口可以不同**），其余命令包一律不答 |
| 断链后果 | socket 在 XCP connected 状态下被关闭 → Slave 执行 **XCP disconnect，停掉全部数据采集** | 没有传输层断链事件；靠 XCP 层超时/心跳机制兜底 |
| RESUME 模式 | Master 必须**持续重试建连**，Slave 在断连期间**缓存测量数据**，否则丢数 | TL 未为 UDP 规定专门的恢复流程（无传输层断链事件可依赖） |

**工程含义**：TCP 的"socket 断 = 会话断"是规范明说的强耦合；UDP 的"只认 CONNECT 来源 IP、不认端口"意味着 Master 换本地端口不需要重连。

### 5.2 通信模型（TL §4.2）

XCP on Ethernet 使用 **标准通信模型**（PL 第 2 章那套 request→response 串行握手）；块传输模型、交错通信模型均为 optional。也就是说第 2 章讲的骨架在 TCP、UDP 上都一字不改。

### 5.3 Ethernet XCP Message：Header / Tail（TL §4.3，双协议共用）

```
┌────────────────┬───────────────┬──────────────┐
│ Header (4 B)   │ XCP Packet    │ Tail (0–3 B) │  = 一条 XCP Message
│ LEN WORD       │ (原始报文，    │ 0–3 个 FILL  │
│ CTR WORD       │  LEN 计它)    │ 字节         │
└────────────────┴───────────────┴──────────────┘
```

- **LEN**（WORD，Intel 序）= 原始 XCP Packet 的字节数（TL §4.3.1.1）；
- **CTR**（WORD，Intel 序）：Master 对每个 point-to-point 连接维护**一个**计数器，CMD/STIM 全部递增；Slave 维护**独立的第二个**计数器，RES/ERR/EV/SERV/DAQ 全部递增（TL §4.3.1.2）。CTR 用于检测丢包；**IP 多播报文 CTR 置 0，Slave 对多播报文的 CTR 字段不做处理**（TL §4.3.1.2/§5.1/§5.4）；
- **TCP 复用与 UDP 完全相同的帧格式**——"simplify decoding the original XCP messages"（TL §4.3.1 原文）。TCP 收到的字节流按 LEN 切 XCP Message 即可，**TCP 不引入另一套分帧头**；
- **UDP 一帧可打包多条 XCP Message**，但一条 XCP Frame **不得跨 UDP 帧边界**（TL §4.3.1）；
- Tail：单条消息/帧时 Tail 可选（TL §4.3.2.1）；多条消息同帧时，按 `PACKET_ALIGNMENT_x`（x=8/16/32）用前一条的 Tail 补 FILL，使**每条 XCP Packet 的起始满足对齐**；FILL 内容 do-not-care（TL §4.3.2.2）。对齐值本身是 A2L 以太网 IF_DATA 段的参数（AML 文件见 6.2）。

### 5.4 性能上限（TL §4.4 Table 1）

MAX_CTO/MAX_DTO 的实际能力受主机协议栈限制，规范给的是**值域**：MAX_CTO（BYTE）`0x08–0xFF`；MAX_DTO（WORD）`0x0008–0xFFFF`。CONNECT 响应里协商出的具体值（PL 侧）必须 `mod AG = 0`。

### 5.5 Ethernet 专用命令（TL §5；经 `TRANSPORT_LAYER_CMD 0xF2` + 子命令码）

TL Table 2 总览（**全部 optional**；类别都是 "Ethernet only, **Multicast (UDP/IP only)**"——发现/配置类命令只在 UDP 多播语义下定义）：

| 命令 | 子码 | Timeout | 用途 |
|---|---|---|---|
| GET_SLAVE_ID | `0xFF` | 300 ms | 网络发现 Slave |
| GET_SLAVE_ID_EXTENDED | `0xFD` | 300 ms | 扩展发现（+MAC、多类型 ID） |
| SET_SLAVE_IP_ADDRESS | `0xFC` | 300 ms | 给 Slave 配 IP |
| GET_DAQ_CLOCK_MULTICAST | `0xFA` | 不敏感 | DAQ 时钟多播分发 |
| User-defined | `0x01–0x0F` | 厂商自定义 | — |

> ⚠️ 规范原文：未实现的 optional 命令 Slave **可能连负响应都不发**（TL §5）。工具对这些命令必须容忍超时，不能把"无响应"当协议错误。

**GET_SLAVE_ID**（TL §5.1）：Master 向 **239.255.0.0:5556** 发 IPv4 多播命令（结构：`[0]F2 [1]FF [2]Port WORD（期望响应端口）[4..7]响应多播地址4字节 [8..19]Reserved(预留给 IPv6) [20]IP_VERSION(0=IPv4)`）。Slave **不论连接状态**都要处理，并对**自己监听的每个端口回一条响应**——响应也走 IP 多播（无有效 IPv4 时地址填 `0.0.0.0`）。响应结构：`[0..3]Slave IP [4..15]Reserved [16]Port WORD [18]STATUS [19]RESOURCE（与 CONNECT 响应的位图一致）[20]Length DWORD [24..]GET_ID type0 的识别内容`。STATUS 位掩码（TL Table 5/6）：bit1:0=`IP_TRANSPORT_PROTOCOL`（0=仅 TCP，1=仅 UDP，2=两者）、bit2=`IP_VERSION`（0=IPv4）、bit3=`SLV_AVAILABILITY`（1=已被占用）、bit4=`SLV_ID_EXT_SUPPORTED`。——这是"发现 Slave 支持 TCP 还是 UDP"的规范途径。

**GET_SLAVE_ID_EXTENDED**（TL §5.2）：流程同上，响应以 `[FF, FD]` 开头（更贴近 PL 正响应风格），并追加 Slave **MAC 地址**、可重复的 `[WORD Length_IDT][BYTE identification type][ID 内容]` 块（规范建议实现 type 5/6/7）。Slave 支持性由上一条响应 STATUS bit4 宣告。

**SET_SLAVE_IP_ADDRESS**（TL §5.3）：用 MAC 定位 Slave、下发 IPv4 地址（命令含 `[20]Mode（bit0=IP_VERSION）[21..26]MAC [27..30]新 IP`）。正响应 `[FF, FC, Status, MAC…]`，Status：`0`=地址即刻有效、`1`=已接收稍后生效、`2`=需人工操作才生效。错误处理表（TL Table 20）：`ERR_CMD_BUSY` → **无限重试**；APIPA 场景引用 ISO 13400-2。

**GET_DAQ_CLOCK_MULTICAST**（TL §5.4）：多播时钟分发。Slave 监听组地址由 A2L `CLUSTER_AFFILIATION`（Intel 序，WORD）映射：`239.255.{高字节}.{低字节}`，端口 **5557**；Master 侧要求**最迟每 2 秒发一次**该命令（`[F2, FA, Cluster ID WORD, Counter BYTE]`），Slave 收到回 PL 定义的 **EV_TIME_SYNC** 事件包——多台 Slave / 多 Master 共享同一 DAQ 时钟用的就是这条通路。

### 5.6 传输层错误处理与事件（TL §5.5、§6）

TL §5.5 Table 20 只给了一张小表（SET_SLAVE_IP_ADDRESS 的 BUSY 重试等），其余错误语义回到 PL 第 2.10 节那张总表；**以太网没有专用事件**（TL §6 原文："no specific events … at the moment"）。

### 5.7 总线负载核算（TL §7.1）——测量配置别超预算

A2L 的 `MAX_BUS_LOAD(%) × MAX_BIT_RATE(Mbit)` 让 Master 预留带宽：预留量 = `(100% − MAX_BUS_LOAD) × MAX_BIT_RATE`。核算**只计净数据**（XCP header + packet + tail，不含 TCP/UDP/IP 开销——以太网场景下"每帧可能装多条消息"，TCP/IP 层开销无法折入公式，TL §7.1 原文）：

```
Total_Busload_Consumption = Σ_events ( 1/MIN_CYCLE_TIME × Σ_ODT frame_length(header+packet+tail) )
```

规范算例（无 Tail 假设）：2ms 事件通道 2 个 ODT（40 B、17 B）+ 100ms 事件通道 6 个 ODT（5×40 B、2 B）：

```
= 1/2ms × (2×4 + 40 + 17) + 1/100ms × (6×4 + 5×40 + 2)
= 34 780 B/s ÷ (12% × 10 Mbit = 1.2 Mbit) = 23.18%
```

> 算例数字按 TL 原文照录（其除法把 byte/s 与 bit/s 混写单位）。工程上该式的意义：实际净数据流量 ÷ 预留预算，比值越大越接近挤占非 XCP 流量——Master 在配置 DAQ 时应边配边算（TL §7.1：无固定周期的事件通道用 MIN_CYCLE_TIME 做最坏情况估计）。

注意两点：`MAX_BUS_LOAD` **只限 DAQ/STIM 流，不限 CMD/RES/ERR/EV**（TL §7.1 末段）；Master 还应在线核对实际网卡带宽是否等于 A2L 的 MAX_BIT_RATE，偏低时要先评估能否开工（TL §7.1 原文）。

### 5.8 选 TCP 还是 UDP？（规范事实 + 一句工程常识）

规范层事实：① 发现类命令只定义了 UDP 多播通路；② TCP 断链=会话断、单连接强约束；③ 两者帧格式/通信模型完全相同，**Master 实现通常只需要换 socket 层**。工程常识（非规范）：周期测量主流走 UDP（无队头阻塞、丢包有 CTR 可检、DTO 本来就不保证到达）；需要可靠命令/配置通道时才考虑 TCP。

---

## 第 6 章 A2L 接口与 V1.5 要点

> 材料：PL §8、§3.1；TL §8。本章回答两个问题：Master 的"知识"从哪来（A2L/AML 文件族），以及 V1.5 相对老工具代码要注意什么。

### 6.1 符号名 → 协议参数的映射关系

标定/测量的操作对象在 A2L（ASAM MCD-2 MC 描述文件）里是符号（测量变量、标定参数），符号的地址、数据类型、字节序最终都要落到第 3/4 章那些字节命令上。中间的桥梁是 **IF_DATA**：挂在 MODULE / MEASUREMENT / CHARACTERISTIC 等对象下的 XCP 专用参数块。

### 6.2 AML 文件族与版本组合（PL §8.1/§8.2，TL §8.1）

XCP 的 AML（MCD-2 MC 语言文件）按"协议层 + 传输层"分件：

| 文件 | 内容 |
|---|---|
| `XCP_vX_Y_common.aml` | 协议层通用参数（tag `Common_Parameters`，与传输层无关）|
| `XCP_on_##_vU_V.aml` | 各传输层专用参数（## = UDP_IP / TCP_IP / CAN…）|
| `XCP_definitions.aml` | 把上面两类**引用**组合起来：`/include XCP_common_vX_Y.aml` + 一条或多条 `/include XCP_on_##_vU_V.aml`——同一从机可同时宣告多种传输层及其各自版本 |
| `XCP_vX_Y_on_TCP_IP.aml` / `XCP_vX_Y_on_UDP_IP.aml` | 以太网两传输层的专用参数（TL §8.1 文件名）|
| `XCP_vX_Y_IF_DATA_example.a2l` | 以太网 IF_DATA 示例（TL §8.2，看 `/begin XCP_ON_TCP_IP` 与 `/begin XCP_ON_UDP_IP` 两段即可）|

**协议层与传输层版本独立组合**，以兼容矩阵（PL §3.1.2）为准——这正是 1.3 节 GET_VERSION 分别回 PL/TL 两个版本号的现实意义：A2L 写的组合与 Slave 实际固件版本要互相印证。

### 6.3 IF_DATA 结构与 overrule 规则（PL §8.2.2）

`"XCP"` 块 = 默认 `Common_Parameters` + 各 `/begin XCP_on_## ..` 传输层段；传输层段内可再放一份 `Common_Parameters` **覆盖（overrule）默认值**。两条容易踩的规则（均为 PL §8.1/§8.2.2 原文）：

- overrule 是**整块替换，不是合并**——不能在协议层块里写一半、传输层块里补另一半（例：optional 命令清单不可两段拼接，只能被完整替换）；粒度是块级（PROTOCOL_LAYER、DAQ 等）；
- Master 取参数顺序：先在对应 `/begin XCP_on_##` 内找 Common_Parameters，找到用之，找不到才用外层默认值；
- 同一传输层要**多个实例**（如两个 UDP 口）时必须用 `"XCPplus"` 块，`"XCP"` 不支持。

### 6.4 A2L 与 Slave 在线一致性检查（PL §8.4）

Slave 支持的话，IF_DATA 里几乎每个参数都能用对应命令在线读回（GET_DAQ_PROCESSOR_INFO、GET_PAG_PROCESSOR_INFO、GET_COMM_MODE_INFO…）。A2L 值与在线值并存时 **Master 必须做一致性检查，发现矛盾要提示用户并让用户决定用哪份**（PL §8.4 原文要求）——好工具在这一步会输出差异清单而不是静默偏向某一边。

### 6.5 V1.5 要点回顾清单

散在正文里、接手时最容易漏的五条：

1. **版本协商走 GET_VERSION**（`0xC0 0x00`）：CONNECT 只回 PL/TL 的 Major（高字节），Min 必须 GET_VERSION 拿（第 1.3 节）；
2. **内存访问命令几乎全 optional**：MTA/UPLOAD/SHORT_UPLOAD/SHORT_DOWNLOAD/MODIFY_BITS 在 V1.5 节头都是 optional，唯 DOWNLOAD 对 CAL/PAG mandatory——先探测再定路线（第 3.9 节）；
3. **DAQ PACKED MODE**（`0xC0 0x01/0x02`，PL §4.1.10）：V1.5 新增的 DTO 聚合模式，工具不支持就保持默认关闭（第 4.8 节）；
4. **`*_CFG_LOST` 标志处理流程**：Slave 侧配置变化后必须 `SET_REQUEST(CLEAR_*)` + 重初始化，否则 Slave 断连（第 2.8 节）；
5. **以太网专属**：Header/Tail 对齐规则、双独立 CTR、GET_SLAVE_ID 多播发现、DAQ clock 多播——全部 optional，但 UDP/TCP 行为差异要分开处理（第 5 章）。

### 6.6 建议接手阅读路径

按"能跑起来"的顺序读前三章 + 第 5 章对应小节：① 2.1–2.7 帧与连接（先把 CONNECT 握出来）→ ② 第 3 章标定闭环（SHORT_UPLOAD/SHORT_DOWNLOAD 一对命令即可演示）→ ③ 第 4 章测量闭环（FREE→ALLOC×3→PTR/WRITE→START）→ ④ 5.3 帧对齐与 CTR（抓包排错必备）→ ⑤ 其余按需。每一章末尾的"端到端时序图 + 常见坑"就是排错索引。

---

## 附录 A 术语表

| 术语 | 全称/含义 | 首次出现 |
|---|---|---|
| AG | Address Granularity，访问粒度（BYTE/WORD/DWORD），CONNECT 响应 COMM_MODE_BASIC 声明 | 2.7 |
| AML | ASAM MCD-2 MC Language 文件，定义 IF_DATA 语法 | 6.2 |
| CAL/PAG | 标定 / 页切换资源（CONNECT RESOURCE bit0） | 2.7 |
| CRO / CTO | Command Transfer Object 通道：请求（Master→Slave，旧称 CRO）与响应/异步包 | 2.1 |
| CTR | Packet Counter，以太网 Header 中的 16 位包计数器，Master/Slave 各自独立 | 5.3 |
| DAQ | Data Acquisition，同步数据采集（RESOURCE bit1） | 4.1 |
| DTO | Data Transfer Object 通道，承载 DAQ/STIM 数据 | 2.1 |
| ELEMENT | 一次访问的最小单位 = AG 字节 | 3.3 |
| ERR / EV / RES / SERV | CTO 四类响应/异步包的 PID（0xFE/0xFD/0xFF/0xFC） | 2.1 |
| FIRST_PID | DAQ list 首 ODT 的 DTO PID，START_STOP_DAQ_LIST 响应给出 | 4.6 |
| IF_DATA | A2L 内挂接传输层/协议层参数的块 | 6.1 |
| MTA | Memory Transfer Address，Slave 内的内存传送指针（SET_MTA 设置） | 3.2 |
| ODT | ODT（Object Descriptor Table）= 一组 ODT Entry；Entry = {地址, 扩展, 尺寸, 位偏移} | 4.1 |
| PAGE | 同一地址的多套"值+属性"（标定页） | 3.6 |
| PID | Packet Identifier，包首字节 | 2.1 |
| SEED & KEY | 资源解锁流程（GET_SEED/UNLOCK） | 3.7 |
| SEGMENT | 标定数据的逻辑分块（页的上级） | 3.6 |
| STIM | Synchronous Data Stimulation，同步数据注入（RESOURCE bit2） | 4.5 |
| S0–S3 | 错误/事件严重级；S3=Fatal→DISCONNECTED | 2.10 |
| 事件通道 | Event Channel：定义 DAQ 采样节拍（MIN_CYCLE_TIME/PRESCALER） | 4.1 |
| RESUME | 上电自动恢复 DAQ 流模式（EV_RESUME_MODE/CONNECTED 续 DTO） | 2.2 |

## 附录 B 命令码速查表

> 数据源：PL §7.4 Tables 24–31（Support 列照录；"mandatory for X" 语义见 2.6/4.1）与 TL Table 2（以太网子码）。结构细节见第 2–4 章对应节。

### B.1 标准组（PL Table 24）

| 命令 | 码 | Support | | 命令 | 码 | Support |
|---|---|---|---|---|---|---|
| CONNECT | `0xFF` | mandatory | | BUILD_CHECKSUM | `0xF3` | optional |
| DISCONNECT | `0xFE` | mandatory | | TRANSPORT_LAYER_CMD | `0xF2` | optional |
| GET_STATUS | `0xFD` | mandatory | | USER_CMD | `0xF1` | optional |
| SYNCH | `0xFC` | mandatory | | GET_VERSION | `0xC0,00` | optional |
| GET_COMM_MODE_INFO | `0xFB` | optional | | | | |
| GET_ID | `0xFA` | optional | | | | |
| SET_REQUEST | `0xF9` | optional | | | | |
| GET_SEED | `0xF8` | optional | | | | |
| UNLOCK | `0xF7` | optional | | | | |
| SET_MTA | `0xF6` | optional | | | | |
| UPLOAD | `0xF5` | optional | | | | |
| SHORT_UPLOAD | `0xF4` | optional | | | | |

### B.2 标定组（PL Table 25）

| 命令 | 码 | Support |
|---|---|---|
| DOWNLOAD | `0xF0` | **mandatory for CAL/PAG** |
| DOWNLOAD_NEXT | `0xEF` | optional |
| DOWNLOAD_MAX | `0xEE` | optional |
| SHORT_DOWNLOAD | `0xED` | optional |
| MODIFY_BITS | `0xEC` | optional |

### B.3 页切换组（PL Table 26，全 optional）

| 命令 | 码 | 命令 | 码 |
|---|---|---|---|
| SET_CAL_PAGE | `0xEB` | GET_PAGE_INFO | `0xE7` |
| GET_CAL_PAGE | `0xEA` | SET_SEGMENT_MODE | `0xE6` |
| GET_PAG_PROCESSOR_INFO | `0xE9` | GET_SEGMENT_MODE | `0xE5` |
| GET_SEGMENT_INFO | `0xE8` | COPY_CAL_PAGE | `0xE4` |

### B.4 DAQ/STIM 组（PL Tables 27–28）

| 命令 | 码 | Support | | 命令 | 码 | Support |
|---|---|---|---|---|---|---|
| SET_DAQ_PTR | `0xE2` | mandatory† | | GET_DAQ_RESOLUTION_INFO | `0xD9` | optional |
| WRITE_DAQ | `0xE1` | mandatory† | | GET_DAQ_EVENT_INFO | `0xD7` | optional |
| SET_DAQ_LIST_MODE | `0xE0` | mandatory† | | FREE_DAQ | `0xD6` | mandatory†‡ |
| GET_DAQ_LIST_MODE | `0xDF` | optional | | ALLOC_DAQ | `0xD5` | mandatory†‡ |
| START_STOP_DAQ_LIST | `0xDE` | mandatory† | | ALLOC_ODT | `0xD4` | mandatory†‡ |
| START_STOP_SYNCH | `0xDD` | mandatory† | | ALLOC_ODT_ENTRY | `0xD3` | mandatory†‡ |
| GET_DAQ_CLOCK | `0xDC` | optional | | CLEAR_DAQ_LIST | `0xE3` | mandatory† |
| READ_DAQ | `0xDB` | optional | | GET_DAQ_LIST_INFO | `0xD8` | optional |
| GET_DAQ_PROCESSOR_INFO | `0xDA` | optional | | WRITE_DAQ_MULTIPLE | `0xC7` | optional |
| DTO_CTR_PROPERTIES | `0xC5` | optional | | SET/GET_DAQ_PACKED_MODE | `0xC0,01/02` | optional |

† "mandatory for DAQ/STIM"：宣告支持该资源即须实现。‡ 动态配置四条：码表标 mandatory for DAQ/STIM、节头标 dynamic optional——见 4.1 的双重口径说明。

### B.5 以太网传输层子码（经 TRANSPORT_LAYER_CMD `0xF2`，TL Table 2）

| 子码 | 命令 | 说明 |
|---|---|---|
| `0xFF` | GET_SLAVE_ID | 多播发现（239.255.0.0:5556），§5.5 |
| `0xFD` | GET_SLAVE_ID_EXTENDED | 扩展发现（含 MAC），§5.5 |
| `0xFC` | SET_SLAVE_IP_ADDRESS | 远程配 IP，§5.5 |
| `0xFA` | GET_DAQ_CLOCK_MULTICAST | DAQ 时钟多播分发（:5557），§5.5 |
| `0x01–0x0F` | （厂商自定义） | — |

编程（0xD2–0xC8）、调试、时间同步组不在本手册范围，码表见 PL §7.4 Tables 29–31。
