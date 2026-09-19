# ASAM MCD-1 XCP 1.3.0 中文总结

> 面向“类似 CANape 的 ECU 变量读取/测量工具”实现。依据用户提供的 ASAM MCD-1 XCP Protocol Layer Specification Version 1.3.0。

## 1. XCP 到底解决什么问题

XCP 是 ECU 的测量与标定协议。它把协议层和传输层分离：协议层定义连接、内存访问、DAQ/STIM、标定、Flash Programming、时间同步、安全和错误恢复；XCP on CAN、Ethernet、FlexRay、USB、SxI 等规范负责把这些 Packet 放到具体总线/网络上。

CANape 类工具实际上需要两套标准协同：

```text
A2L / ASAM MCD-2 MC
        ↓
变量名、地址、类型、换算、Event、XCP IF_DATA
        ↓
XCP Master
        ↓
Polling / DAQ / Calibration
        ↓
CAN / CAN FD / TCP / UDP
        ↓
ECU
```

XCP 本身主要按地址访问内存；“VehicleSpeed 是什么、地址在哪里、是什么类型、怎么换算成 km/h”主要来自 A2L。

## 2. 最小可用的 CANape-like Measurement

第一版不用实现完整 XCP。只要完成：

```text
CONNECT
GET_STATUS
GET_COMM_MODE_INFO（可选但建议）
SHORT_UPLOAD
或 SET_MTA + UPLOAD
DISCONNECT
```

再加 A2L Parser，就能实现：

```text
VehicleSpeed
   ↓ A2L
0x70012340 / UWORD / Intel / factor=0.01
   ↓ XCP SHORT_UPLOAD
34 12
   ↓ decode + COMPU_METHOD
46.60 km/h
```

规范明确把 `SHORT_UPLOAD` 作为 Polling Measurement 的推荐方式；另一种是 `SET_MTA + UPLOAD`。

## 3. CONNECT 后必须保存的 Session 参数

`CONNECT` Response 中至少要保存：

```text
RESOURCE
COMM_MODE_BASIC
MAX_CTO
MAX_DTO
Protocol Version
Transport Version
```

其中最容易实现错的是：

**BYTE_ORDER**：Intel / Motorola；

**ADDRESS_GRANULARITY (AG)**：一个地址单位对应 1/2/4 Byte。AG 会影响 MTA、UPLOAD/DOWNLOAD Length、ODT Entry Size 与地址计算，不能简单认为所有地址步长都是 Byte。

`RESOURCE` 告诉 Master Slave 是否提供：CAL/PAG、DAQ、STIM、PGM。

## 4. Polling 和 DAQ 的区别

Polling：

```text
Master → SHORT_UPLOAD
Slave  → Value
Master → SHORT_UPLOAD
Slave  → Value
...
```

优点是简单，不需要配置 DAQ。缺点是每次测量都有 Command/Response 开销，变量越多、周期越快，效率越差。

DAQ：Master 先把变量地址配置到 ODT 中，然后 Slave 被 ECU Event 触发后主动发送 DTO：

```text
Event Channel
   ↓
DAQ List
   ↓
ODT
   ↓
ODT Entry → Address / Extension / Size
```

这才是 CANape 高速测量的核心机制。

## 5. Dynamic DAQ 的标准流程

Master 先查询：

```text
GET_DAQ_PROCESSOR_INFO
GET_DAQ_RESOLUTION_INFO
GET_DAQ_EVENT_INFO
```

然后严格按顺序分配：

```text
FREE_DAQ
   ↓
ALLOC_DAQ
   ↓
ALLOC_ODT
   ↓
ALLOC_ODT_ENTRY
```

之后填写 Entry：

```text
SET_DAQ_PTR
WRITE_DAQ
WRITE_DAQ
...
```

再绑定 Event 和运行参数：

```text
SET_DAQ_LIST_MODE
START_STOP_DAQ_LIST(Select)
GET_DAQ_CLOCK（可选/时间对齐）
START_STOP_SYNCH(Start Selected)
```

停止时：

```text
START_STOP_DAQ_LIST(Select)
START_STOP_SYNCH(Stop Selected)
```

Dynamic Allocation 顺序是协议状态机的一部分，错误顺序会返回 `ERR_SEQUENCE`；如果要在已有配置中增加更高层对象，通常需要 `FREE_DAQ` 后重建完整配置。

## 6. ODT Packing 时必须考虑的约束

`GET_DAQ_RESOLUTION_INFO` 给出：

```text
GRANULARITY_ODT_ENTRY_SIZE_DAQ
MAX_ODT_ENTRY_SIZE_DAQ
GRANULARITY_ODT_ENTRY_SIZE_STIM
MAX_ODT_ENTRY_SIZE_STIM
```

Entry 的 Address 和 Size 必须满足 Granularity Alignment。Slave 还会通过 `DAQ_KEY_BYTE` 描述：

```text
Optimization Method
Address Extension Scope
Identification Field Type
```

所以通用 ODT Packer 不能只做“从第一个变量开始依次塞进 MAX_DTO”，还要满足：对齐、最大 Entry Size、Address Extension 一致性、Timestamp/PID/Counter 占用，以及 Slave 推荐的 Optimization Method。

## 7. DTO 如何知道属于哪个变量

DTO 本身不是按变量名传输，而是按 DAQ/ODT Layout 传输。Identification Field 可能是：

```text
Absolute ODT Number
Relative ODT + DAQ List BYTE
Relative ODT + DAQ List WORD
Relative ODT + DAQ List WORD aligned
```

Master 在配置 DAQ 时必须建立本地 Mapping：

```text
DTO PID / DAQ / ODT
        ↓
Entry0 → SteeringAngle
Entry1 → VehicleSpeed
Entry2 → YawRate
```

收到 DTO 后按这个 Mapping 解包，再应用 A2L Conversion。

## 8. Timestamp

DAQ 可以带 Timestamp。`GET_DAQ_RESOLUTION_INFO` 给出 Timestamp Size、Unit、Ticks；Slave 的 DAQ Clock 是自由运行 Counter，溢出后 Wrap Around。

第一版工具可以只支持单 ECU 的相对 Timestamp。高级 Time Correlation 用于多个 ECU、多个 Transport Layer、ECU Clock/Grandmaster Clock 的统一时间轴，协议通过 `TIME_CORRELATION_PROPERTIES`、`EV_TIME_SYNC`、Cluster ID、Time Sync Bridge 等机制实现。

## 9. A2L 与 XCP Runtime 参数冲突怎么办

XCP 的很多通信参数既可能写在 A2L `IF_DATA`，又可以运行时从 Slave 查询。规范要求 Master 检查二者一致性；如果冲突，应告知用户并允许选择使用 A2L 值还是 Runtime 值。

这意味着你的工具最好有一个明确的 Configuration Validation Layer，而不是解析 A2L 后直接无条件使用。

## 10. Seed&Key

XCP 只规定 `GET_SEED/UNLOCK` 的通信机制，不规定具体 Seed→Key Algorithm。

```text
GET_SEED(Resource)
       ↓
Vendor DLL/SO
Key = f(Seed, Resource)
       ↓
UNLOCK(Key)
```

Windows 可用 DLL，Linux 可用 `.so`。外部算法输入输出按 XCP Packet 原始 Byte Sequence 传递，不应擅自按 Host Endian 转换。

## 11. Calibration

最基础的写内存：

```text
SET_MTA + DOWNLOAD
```

也有 `SHORT_DOWNLOAD`，但传统 CAN 常见 `MAX_CTO=8` 时，Command Header+Address 已占满 8 Byte，因此它无法携带实际数据。

更完整 Calibration 还包括 SEGMENT/PAGE、ECU Access Page、XCP Access Page、Page Copy、Freeze/NVM Store 等机制。

## 12. Programming 不等于 Calibration

Flash Programming 有独立 Session：

```text
PROGRAM_START
PROGRAM_CLEAR
PROGRAM / PROGRAM_NEXT
PROGRAM_VERIFY（可选）
PROGRAM_RESET
```

`PROGRAM_START` 可能返回新的 `MAX_CTO_PGM/MAX_BS_PGM/MIN_ST_PGM`，进入 Programming 后必须使用 PGM 专用通信参数。

建议 CANape-like 项目先完成 Measurement，再完成 Calibration，最后才做 Programming。

## 13. 错误恢复不能只写成“失败就重试”

XCP 把恢复分成：

```text
Pre-Action + Action
```

例如：

```text
UPLOAD timeout
→ SYNCH
→ SET_MTA
→ Retry UPLOAD

WRITE_DAQ error/timeout
→ SET_DAQ_PTR
→ Retry

ERR_ACCESS_LOCKED
→ GET_SEED / UNLOCK
→ Retry

Dynamic DAQ ERR_SEQUENCE
→ Reinitialize DAQ
```

`EV_CMD_PENDING` 表示 Slave 已接收 Command 但还没处理完。此时 Master 应重启 Timer，**不能重发 Command**。

## 14. 推荐 C++ 架构

```text
XcpCore
├── Session
├── CommandCodec
├── ResponseParser
├── MemoryAccess
├── Security
├── DaqManager
│   ├── Allocator
│   ├── OdtPacker
│   └── DtoDecoder
├── Calibration
├── Programming
└── TimeCorrelation

Transport
├── IXcpTransport
├── VectorCanTransport
├── SocketCanTransport
├── PeakCanTransport
├── UdpTransport
└── TcpTransport

A2L
├── Parser / Database
├── Measurement
├── Characteristic
├── CompuMethod
├── RecordLayout
├── EventChannel
└── IfDataXcp
```

Protocol Core 不应直接依赖 Vector/PEAK/SocketCAN。Transport 层只负责 CTO/DTO Byte 的传送；XCP Core 负责协议语义。这样以后从 CAN 扩展到 Ethernet 时无需重写 Master。

## 15. 建议开发顺序

第一里程碑：A2L + `CONNECT + SHORT_UPLOAD`，支持变量名搜索、Raw→Physical 转换和实时表格显示。

第二里程碑：Dynamic DAQ + DTO Decoder + Timestamp，实现高速多变量测量。

第三里程碑：MDF4 Recorder、Seed&Key、Calibration Page/Download。

第四里程碑：STIM/Bypassing、Flash Programming、Advanced Time Correlation。

对当前目标来说，**最优先实现的不是完整 XCP，而是把 A2L → 地址/类型 → Polling Read 路径跑通，再把同一套 Signal Database 接到 DAQ 上。**
