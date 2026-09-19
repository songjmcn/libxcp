# ASAM MCD-1 (XCP) 通用测量与标定协议 - 协议层规范

**版本：1.3.0**  
**日期：2015-05-01**  
**原文：ASAM MCD-1 (XCP) Universal Measurement and Calibration Protocol - Protocol Layer Specification, Base Standard**

> 译者说明：本文件为基于用户提供的 ASAM MCD-1 XCP 1.3.0 英文规范制作的中文技术译文，便于学习、软件设计和实现参考。命令名、参数名、宏、枚举、A2L/AML 标记、报文字段名称等协议标识符尽量保留英文原文，以避免实现时产生歧义；第一次出现的重要术语给出中文解释。规范性关键词（must / shall / should / may）分别按“必须 / 应 / 建议 / 可以”处理。原 PDF 中每页重复的版权/授权页脚在译文中省略。对于图形，保留图号和中文说明；实现时应与原 PDF 图形对照。此译文不替代 ASAM 官方英文标准。

---

# 中文总览与实现导读

XCP（Universal Measurement and Calibration Protocol，通用测量与标定协议）面向 ECU 的测量、标定、数据刺激、Flash 编程和时间同步。XCP 将“协议层”和“传输层”分离：协议层定义与总线无关的命令、响应、DAQ/STIM、内存访问等机制；CAN、TCP/IP、UDP/IP、SxI、USB、FlexRay 等分别由独立的传输层规范定义。

对于类似 CANape 的测量工具，可以把 XCP 软件栈理解为：

```text
A2L / ASAM MCD-2 MC
        |
        | 变量名 -> 地址 / 类型 / 换算 / Event
        v
XCP Master Protocol Layer
        |
        +-- Polling: SHORT_UPLOAD / SET_MTA + UPLOAD
        |
        +-- DAQ: DAQ List -> ODT -> ODT Entry
        |
        +-- Calibration: DOWNLOAD / Page Switching
        |
        +-- Seed&Key / Checksum / Programming
        v
Transport Layer
 CAN / CAN FD / TCP / UDP / ...
        v
      ECU
```

若目标首先只是“像 CANape 一样按变量名读取 ECU 变量”，最小实现路径为：解析 A2L -> 建立 XCP 连接 -> 获取地址粒度/字节序等连接参数 -> 用 `SHORT_UPLOAD` 或 `SET_MTA + UPLOAD` 读取原始内存 -> 根据 A2L 数据类型、字节序和 COMPU_METHOD 转换为物理值。需要高频、同步、带时间戳的测量时，再实现 DAQ。

动态 DAQ 的核心对象层级为：

```text
Event Channel
    |
    +-- DAQ List
          |
          +-- ODT
                |
                +-- ODT Entry -> ECU memory address + address extension + size
```

典型动态 DAQ 配置顺序：`FREE_DAQ -> ALLOC_DAQ -> ALLOC_ODT -> ALLOC_ODT_ENTRY -> SET_DAQ_PTR -> WRITE_DAQ -> SET_DAQ_LIST_MODE -> START_STOP_DAQ_LIST(select) -> START_STOP_SYNCH(start)`。

---

# 目录（中文）

1. 前言  
2. 引言  
3. 与其他标准的关系  
   3.1 与早期版本的向后兼容性  
   3.2 对其他标准的引用；CCP 与 XCP  
4. XCP 特性与概念  
   4.1 同步数据传输：DAQ、STIM、ODT、事件通道、动态 DAQ、RESUME、优先级与优化  
   4.2 测量模式：轮询、标准突发、改进突发、交替模式  
   4.3 Bypassing（旁路/RCP）  
   4.4 在线标定：SECTOR、SEGMENT、PAGE、页面切换/冻结  
   4.5 Flash 编程  
   4.6 时间相关/时间同步  
   4.7 ECU 状态  
5. XCP 协议：拓扑、通信模型、状态机、保护、帧格式  
6. 性能极限与 ECU 资源消耗  
7. XCP 协议层  
   7.1 XCP Packet、CTO、DTO、PID  
   7.2 Event Code  
   7.3 Service Request Code  
   7.4 Command Code  
   7.5 命令详细说明  
       - 标准命令  
       - 标定命令  
       - 页面切换命令  
       - DAQ/STIM 命令  
       - 非易失存储器编程命令  
       - 时间相关命令  
   7.6 通信错误处理  
   7.7 Event 详细说明  
8. 与 ASAM MCD-2 MC（A2L）描述文件的接口  
9. 外部 Seed&Key 函数接口  
10. 外部 Checksum 函数接口  
11. 外部 A2L 解压/解密函数接口  
12. 示例：会话建立、标定、DAQ、重编程、会话关闭、时间相关  
13. 符号与缩略语  
14. 参考文献  

---

# 1 前言

XCP 是 **Universal Measurement and Calibration Protocol（通用测量与标定协议）** 的缩写。它的主要用途是从电子控制单元（ECU）中进行数据采集以及访问标定数据。因此，XCP 定义了一个通用的、与具体总线无关的协议层；实际传输介质可以采用不同的物理总线或网络，并且每一种被标准认可的传输介质都有独立的传输层规范。

这种分层也反映在标准文档结构中：

- 一份 Base Standard（基础标准）；
- 针对每一种物理总线或网络类型的一份 Associated Standard（配套标准）。

基础标准包含：

- Protocol Layer（协议层）；
- 与 ASAM MCD-2 MC 的接口；
- 与外部 Seed&Key 函数的接口；
- 与外部 Checksum 函数的接口；
- 与外部 A2L 解压/解密函数的接口；
- 通信序列示例。

本版本定义的传输层包括：

- XCP on CAN；
- XCP on Ethernet（TCP/IP、UDP/IP）；
- XCP on SxI（SPI、SCI）；
- XCP on USB；
- XCP on FlexRay。

XCP 中的 “X” 一方面表示协议族可以运行在多种不同的传输层之上；另一方面，由于 XCP 是在 CCP 基础上发展而来的，“X” 也体现 XCP 相比 CCP 扩展了协议能力。

# 2 引言

XCP 可以用于 ECU 开发的各个阶段，例如：

- ECU 开发；
- ECU 测试；
- 在快速控制原型（Rapid Control Prototyping, RCP）系统中用于 Bypass 功能开发。

除测量数据采集和标定以外，XCP 还可以用于 ECU Flash 编程以及 HIL（Hardware-in-the-Loop）仿真。每个 ECU 通常都有一个描述文件，其中包含测量量和标定量所需的基础描述信息，例如地址、数据类型和维度等。

XCP 的设计原则是：

- 尽可能减少 Slave 端 RAM、ROM 和运行时间开销；
- 高效通信；
- Slave 实现尽可能简单。

XCP 采用 **单 Master、多 Slave** 的概念。

基本能力包括：

- 同步数据采集（Synchronous Data Acquisition）；
- 同步数据刺激（Synchronous Data Stimulation）；
- 在线内存标定，即读写访问；
- 标定数据页初始化和切换；
- 用于 ECU 开发目的的 Flash Programming。

可选能力包括：

- 多种传输层，例如 CAN、Ethernet、USB 等；
- Block Communication Mode（块通信模式）；
- Interleaved Communication Mode（交错通信模式）；
- 动态数据传输配置；
- 带时间戳的数据传输；
- 数据传输同步；
- 数据传输优先级；
- 原子位修改；
- 按位数据刺激。

XCP 协议本身不使用 ASAM 数据类型来表达传输中的内存片段。ASAM 数据类型是在相应接口层以及 A2L 描述文件中使用的；工具在这一层将 ECU 的原生数据解释/转换为 ASAM 数据类型。

XCP 帧可理解为：

```text
XCP Frame = XCP Header + XCP Packet + XCP Tail
```

其中 `XCP Packet` 是与传输层无关的通用协议内容；Header 和 Tail 依赖实际 Transport Layer，因此不在本 Base Standard 中定义。

# 3 与其他标准的关系

## 3.1 与早期版本的向后兼容性

### 3.1.1 XCP Protocol Layer 版本号

Base Standard 描述的是 XCP Packet，即与传输层无关的通用协议部分。

XCP Protocol Layer Version Number 是一个 16 位值：

```text
高字节 = Major Version (X)
低字节 = Minor Version (Y)
```

如果协议层发生的修改要求 Slave 驱动软件进行功能性修改，则增加 Major Version。例如：修改已有命令的参数，或者加入新的强制命令。

如果修改不会直接影响 Slave 驱动软件，则增加 Minor Version。例如：仅重新措辞说明文字或修改 AML 描述。

Slave 在 `CONNECT` 响应中只返回 XCP Protocol Layer Version Number 的最高有效字节，即主版本信息。

### 3.1.2 兼容性矩阵

描述 Slave 的主 A2L 文件可以包含 `XCP_definitions.aml`，其中引用特定版本的 Protocol Layer Specification，并引用一个或多个特定版本的 Transport Layer Specification。

当某一版本的 Protocol Layer 对 Transport Layer 版本有前置要求时，协议层规范必须给出该约束；反之亦然。规范中的 Compatibility Matrix 用于列出允许组合的 Protocol Layer 与 Transport Layer 版本。

## 3.2 对其他标准的引用

### 3.2.1 CCP 与 XCP

XCP **不与现有 CCP 实现保持协议级向后兼容**。

与 CCP 2.1 相比，XCP 重点增强了：

- 兼容性与规范化程度；
- 效率和吞吐量；
- 上电后的数据传输；
- 数据页冻结；
- 自动配置；
- Flash 编程。

---

# 4 XCP 特性与概念

## 4.1 同步数据传输

### 4.1.1 DAQ、STIM 与 ODT

位于 Slave 内存中的数据元素，通过 Data Transfer Object（DTO）进行同步传输：

- **DAQ**：Slave -> Master，用于同步数据采集；
- **STIM**：Master -> Slave，用于同步数据标定。

**ODT（Object Descriptor Table，对象描述表）**用于描述同步 DTO 与 Slave 内存之间的映射关系。

同步数据传输对象由 **PID（Packet Identifier）**标识，PID 用于确定哪一个 ODT 描述了该 DTO 中的数据布局。

概念上：

```text
ECU Memory
   | address + length
   v
ODT Entry
   |
   +--> ODT
          |
          +--> PID + data bytes -> DAQ DTO / STIM DTO
```

### 4.1.2 ODT Entry

一个 ODT Entry 通过以下信息引用一个数据元素：

- 地址 Address；
- 地址扩展 Address Extension；
- 以 `ADDRESS_GRANULARITY (AG)` 为单位表示的元素大小；
- 对于位变量，还包括 Bit Offset。

`GRANULARITY_ODT_ENTRY_SIZE_x` 表示 ODT Entry 所能引用的数据元素的最小粒度，其字节数不得小于 Address Granularity：

```text
GRANULARITY_ODT_ENTRY_SIZE_x[BYTE] >= AG[BYTE]
```

ODT Entry 的地址和大小还必须满足相应的对齐约束。允许的 `GRANULARITY_ODT_ENTRY_SIZE_x` 为 `{1, 2, 4, 8}` 字节；允许的 `ADDRESS_GRANULARITY` 为 `{1, 2, 4}` 字节，并且 ODT Entry 粒度必须能够被地址粒度整除。

`MAX_ODT_ENTRY_SIZE_x` 给出单个 ODT Entry 可描述元素大小的上限。如果 Slave 只支持大小为 1 Byte 的元素，则 Master 必须把多字节变量拆成多个单字节元素。

ODT Entry 通过 `ODT_ENTRY_NUMBER` 编号。

### 4.1.3 ODT

多个 ODT Entry 组成一个 ODT。

静态 DAQ 配置时，`MAX_ODT_ENTRIES` 表示每个 ODT 最多包含多少 ODT Entry；动态配置时，该数量不是固定值，因此 `MAX_ODT_ENTRIES` 可以为 0。

每一个 ODT 内部的 `ODT_ENTRY_NUMBER` 都从 0 重新编号并连续递增。

### 4.1.4 DAQ List

多个 ODT 可以组成一个 **DAQ List**。一个 Slave 可以同时存在并运行多个 DAQ List。每个 DAQ List 的采样和传输由 Slave 内部的特定事件触发，事件绑定通过 `SET_DAQ_LIST_MODE` 配置。

静态配置中，`MAX_ODT` 表示该 DAQ List 的 ODT 个数；动态配置中，ODT 个数不是固定值，因此该参数可为 0。

`MAX_DAQ` 是 Slave 中可用 DAQ List 的总数，既包括预定义、不可配置的 DAQ List，也包括可配置列表。`MIN_DAQ` 表示预定义 DAQ List 的数量；`DAQ_COUNT` 表示动态分配的 DAQ List 数量。

每个 DAQ List 内部的 `ODT_NUMBER` 从 0 开始连续编号；Slave 中 `DAQ_LIST_NUMBER` 也必须形成连续编号范围。

为了降低实际传输速率，可对 DAQ List 使用 Prescaler。无降频时 Prescaler 必须为 1，降频时必须大于 1；Prescaler 只允许用于方向为 DAQ 的列表。

允许定义不包含任何 Entry 的“dummy DAQ list”。

### 4.1.5 Event Channel

Event Channel（事件通道）是决定数据传输时序的通用触发源。

对于非固定周期事件，例如偶发事件或曲轴同步事件，可以提供 `MIN_CYCLE_TIME`，便于 Master 对 ECU CPU 负载或所需传输带宽进行最坏情况估算。

`MAX_EVENT_CHANNEL` 表示可用 Event Channel 数量。每个 Event Channel 的 `MAX_DAQ_LIST` 表示最多可绑定多少 DAQ List：

- `0x00`：该 Event 存在，但当前不能用于 DAQ；
- `0xFF`：不限制可分配的 DAQ List 数量。

事件通道可以有固定优先级，优先级为 `0xFF` 时最高。

A2L 中可通过 `DAQ_EVENT` 对 MEASUREMENT 与 Event Channel 的关系进行描述：

- `FIXED_EVENT_LIST`：固定绑定，工具不能修改；
- `AVAILABLE_EVENT_LIST`：允许工具从给定 Event 中选择；
- `DEFAULT_EVENT_LIST`：默认推荐绑定，可由工具修改。

### 4.1.6 动态 DAQ 配置

Slave 可采用静态或完全动态的 DAQ 配置。具体能力由 `GET_DAQ_PROCESSOR_INFO` 返回的 `DAQ_PROPERTIES.DAQ_CONFIG_TYPE` 指示。

动态配置使用以下命令：

```text
FREE_DAQ
ALLOC_DAQ
ALLOC_ODT
ALLOC_ODT_ENTRY
```

动态分配受 Slave 的 DAQ List 总数、ODT 总数和 ODT Entry 总数等资源限制。如果内存不足，Slave 返回 `ERR_MEMORY_OVERFLOW`；一旦出现该错误，整个 DAQ List 配置应视为无效。

动态 DAQ 的分配顺序有严格要求：

```text
1. FREE_DAQ
2. ALLOC_DAQ
3. 对所有 DAQ List 执行 ALLOC_ODT
4. 对所有 ODT 执行 ALLOC_ODT_ENTRY
```

违反该顺序时 Slave 返回 `ERR_SEQUENCE`。该规则意味着不能简单在已经存在的动态配置尾部再加入一个新的 DAQ List；需要重新配置整个 DAQ 结构。

### 4.1.7 DAQ 配置保存与上电数据传输

把 DAQ 配置保存到非易失存储器的主要价值有两点：

- 对重复使用且不变化的测量配置，缩短每次启动时的配置时间；
- 支持上电后自动传输，即 **RESUME Mode**。

Master 可以通过 `START_STOP_DAQ_LIST(Select)` 选择需要持久化的 DAQ List，并依据当前配置计算一个 **Session Configuration Id**。Master 自身应保存这个 ID，并通过 `SET_REQUEST` 发送给 Slave。

如果请求 `STORE_DAQ_REQ_RESUME` 或 `STORE_DAQ_REQ_NO_RESUME`，Slave 在条件满足时应把所选择的 DAQ List 和 Session Configuration Id 保存到非易失存储器。之后 Master 可以通过 `GET_STATUS` 返回的 ID 验证当前自动恢复的 DAQ 配置是否与自己预期一致。

#### 4.1.7.1 保存 DAQ 配置但不上电自动传输

使用 `STORE_DAQ_REQ_NO_RESUME` 可以仅保存配置。上电后 Master 比较 Session Configuration Id，确认配置有效后再启动 DAQ/STIM，从而避免每次重新分配和配置全部 DAQ List。

#### 4.1.7.2 保存 DAQ 配置并进入 RESUME Mode

RESUME Mode 的目的是：Slave 上电后无需等待 Master 重新完成所有 XCP 配置，就能够自动启动 DAQ，或自动准备接收 STIM。

Slave 是否支持 RESUME 由 `GET_DAQ_PROCESSOR_INFO` 中 `DAQ_PROPERTIES.RESUME_SUPPORTED` 指示。

在 RESUME 模式下：

- Master 可以通过 `GET_STATUS` 判断 Slave 是否处于 RESUME；
- `GET_DAQ_LIST_MODE` 可确认具体 DAQ List 是否属于 RESUME 配置；
- Slave 上电恢复配置后应发送 `EV_RESUME_MODE`，其中包含 Session Configuration Id；
- 如果支持时间戳，还应携带当前数据采集时钟；
- DAQ 方向列表可在 Master 尚未发送任何 XCP Command 前就开始发送 DAQ Packet；
- STIM 方向列表可在此时已经准备接收 STIM Packet。

Master 和 Slave 必须记住保存 RESUME 配置时所使用的通信参数，并在上电自动传输时继续使用一致的参数。

### 4.1.8 DAQ List 优先级

XCP 允许 DAQ List 具有优先级。配合 DTO 的有限长度，高优先级 DAQ List 可以在可接受延迟内打断低优先级列表的发送。

### 4.1.9 ODT 优化

Slave 可以在 ODT 层实现专用拷贝例程，以降低运行时间或提高有效传输速率。为了利用这些优化，Master 在构造 ODT 时应遵守 Slave 给出的 `Optimization_Method`。

常见优化模式包括：

- `OM_DEFAULT`：无额外特殊要求，只需满足 Entry 粒度和最大 Entry 大小限制；
- `OM_ODT_TYPE_16`：按 16 位类型优化，同一 ODT 内的 Entry 建议使用相同类型并满足对应对齐；
- `OM_ODT_TYPE_32`：按 32 位类型优化；
- 其他模式以规范给出的 `DAQ_KEY_BYTE` 定义为准。

### 4.1.10 按位刺激（Bitwise Stimulation）

XCP 可以对位级数据执行同步刺激。位级 Entry 通过 ODT Entry 中的 Bit Offset 描述。Master 必须遵守 Slave 对位刺激、Entry 尺寸和对齐的支持限制。

### 4.1.11 同步数据采集

在 DAQ 方向中，当事件触发某个 DAQ List 时，Slave 对相应变量进行采样，并以 DTO 发送给 Master。

XCP 至少保证 **ODT 级一致性**：同一个 ODT 中的全部元素应一致地采样。完整 DAQ List 可能包含多个 ODT，因此采集并发送整个列表需要一定时间。

如果上一事件周期的数据尚未发送完成，下一个事件周期已经到来，则发生 **OVERLOAD**。Slave 可以向 Master 指示该状态，具体指示方式通过 `GET_DAQ_PROCESSOR_INFO` 的 `OVERLOAD_x` 能力标志描述。

A2L Event Channel 中的：

- `CONSISTENCY_DAQ` 表示同一 DAQ List 内的数据一致采样；
- `CONSISTENCY_EVENT` 表示绑定到该 Event 的所有 DAQ List 数据均保持事件级一致性。

### 4.1.12 同步数据刺激

同步数据刺激是同步数据采集的反向过程。通过 `DIRECTION` 标志，可以将 DAQ List 配置为 STIM 方向。

Master 使用 DTO Packet 把刺激数据发送给 Slave。ODT 描述 DTO 中数据与 Slave 内存之间的映射。STIM Processor 缓存收到的数据；当触发该 DAQ List 的事件发生时，缓存数据被同步写入 Slave 内存。

对于 STIM，同样可以通过 `CONSISTENCY_DAQ` 和 `CONSISTENCY_EVENT` 描述 DAQ List 级或 Event 级的数据一致性。

## 4.2 测量模式

### 4.2.1 Polling（轮询）

轮询是最简单的测量方式。原则上，每一个测量值都由 Master 通过一条额外 XCP 命令主动请求。

特点：

- 有效采样率受 Master 与 Slave 的处理性能以及链路延迟影响；
- 不能使用 XCP 时间戳机制；
- 不同测量变量之间不保证采样一致性；
- 不需要为 Slave 建立 DAQ List 测量配置。

可用命令：

```text
SHORT_UPLOAD      # 推荐
```

或者：

```text
SET_MTA
UPLOAD
```

### 4.2.2 同步数据传输 - DAQ 方向 - 标准 Burst

XCP 的标准测量模式通过预先配置 ODT 来优化读取 ECU 内部变量的过程。Master 在配置阶段将感兴趣的数据描述为 ODT，并将 ODT 所属 DAQ List 绑定到 DAQ Event。测量启动后，Slave 依据内部 Event 自动发送 ODT，而不需要 Master 对每个变量逐次请求。

该模式至少保证 **ODT consistency**：一个 ODT 的全部内容在同一采样时刻取得。能否满足所要求的采样率取决于 Slave 性能以及完整 DAQ 消息的传输时间。

时间戳机制是可选能力。发生 DAQ Overflow 时，Slave 可向 Master 发送事件，用于表示测量要求没有得到满足。

配置该模式的关键命令为：`SET_DAQ_LIST_MODE`。

### 4.2.3 同步数据传输 - DAQ 方向 - 改进 Burst

改进 Burst 基于标准模式，但采用一次 Event 驱动来完成整个 DAQ 的采样，因此可以实现 **DAQ consistency**：一个 DAQ List 中全部 ODT 在同一个采样时刻取得。

时间戳同样可以作为可选功能。发生处理能力不足时，可通过 DAQ Overflow 通知 Master。

A2L Event Channel 的 `CONSISTENCY_DAQ` / `CONSISTENCY_EVENT` 可用于描述具体的数据一致性能力。

### 4.2.4 同步数据传输 - DAQ 方向 - Alternating

Alternating 是一种非常轻量、资源占用低，但测量性能也较低的显示模式。其主要目的只是以很低 ECU/XCP Slave 资源消耗显示 ECU 内部数据。

虽然所有 ODT 在结构上仍可属于同一 DAQ List，但该模式允许采样间隙且不会上报，因此这些数据**不适合作为严格测量数据**。

限制包括：

- 不允许使用 XCP Timestamp；
- 每次 DAQ Event 只采样一个 ODT；
- 内部延迟不会触发 DAQ Overflow；
- Master 无法确定完整 DAQ List 的真实刷新周期，只能保证 ODT 顺序稳定。

通过 `SET_DAQ_LIST_MODE` 配置，并设置 `ALTERNATING` 标志。Master 不允许同时设置 `ALTERNATING` 和 `TIMESTAMP`。

## 4.3 Bypassing（旁路/RCP）

Bypassing 可以通过同时使用同步数据采集（DAQ）和同步数据刺激（STIM）实现。至少需要：

- 一个 DAQ 方向的 DAQ List：ECU -> Bypass/RCP 工具；
- 一个 STIM 方向的 DAQ List：Bypass/RCP 工具 -> ECU；
- 面向 Bypass 使用的 Event Channel。

典型流程：ECU 在原始函数执行前采样 Bypass 输入，并通过 DAQ 发送给外部工具；外部工具收到 DAQ 后执行替代算法；结果再通过 STIM 返回 ECU，并在合适的事件点写入，通常用于覆盖原始函数输出。

### 4.3.1 Delayed Bypassing

Delayed Bypassing 表示某一 Bypass 周期采集的 DAQ 数据，用于产生后续周期的 STIM 数据。此时必须在 Bypass 工具或 XCP Slave 中缓存一个或多个周期的 STIM 数据。

DAQ 传输、Bypass 算法计算和 STIM 返回所需的总时间称为 **Bypassing Turnaround Time**。当该时间无法满足单周期闭环要求时，可以通过延迟 Bypass 给算法执行留出更多时间。

### 4.3.2 Bypass Activation

为了支持 Bypass 而对 ECU 代码加入的接入点称为 **Bypass Hook**。出于安全考虑，Bypass Hook 可能需要显式激活后才能工作。具体激活机制属于实现相关内容，不由本 XCP 规范定义。


### 4.3.3 Bypassing 启动

由于 Bypass 会直接影响 ECU 行为，因此必须保证 ECU 不会使用未初始化或不一致的数据。如何保证这一点由 ECU 软件实现自行决定。

### 4.3.4 合理性检查（Plausibility Checks）

XCP Slave 可以对通过 STIM 接收到的数据执行合理性检查，例如检查最小值/最大值边界。检查边界以及检查失败后的处理动作，可以通过普通标定方式配置，不需要为此增加专用 XCP 命令。

### 4.3.5 Bypassing 一致性

Bypassing consistency 是指 DAQ 数据和 STIM 数据在周期上的对应关系正确，也就是返回的 STIM 数据确实属于与之对应的 DAQ Event Cycle。

如果构成 Bypass 的 Event Channel 满足以下条件，就可以检查这种一致性：

- 每个 Event Channel 在每个 Bypass Cycle 中恰好触发一次；
- 各 Event Channel 始终按相同顺序触发；
- DAQ 方向 Event 与 STIM 方向 Event 的关系为 1:1、1:n 或 n:1。

#### 4.3.5.1 Event Channel 关系

Bypass 中 DAQ Event Channel 与 STIM Event Channel 的关联关系可以是固定的，也可以由 XCP Master 动态配置。规范图 13 给出了 3 个 DAQ/STIM Event Channel 通过不同组合构造不同 Bypass 的示例。

#### 4.3.5.2 DTO CTR Event Channel 属性

DTO CTR Event Channel 属性用于控制 DTO CTR 字段如何处理，是 Bypass 一致性检查的核心。对每个 Event Channel，这些属性均可以是可选实现。

**RELATED EVENT CHANNEL NUMBER**

一个 Event Channel 可以关联到另一个 Event Channel，也可以关联到自身。关联对象由 Related Event Channel Number 指定；该属性可以固定，也可以通过 `DTO_CTR_PROPERTIES` 命令配置。

Bypass 工具可以通过 `DTO_CTR_PROPERTIES` 查询，也可以从 A2L 中的 `RELATED_EVENT_CHANNEL_NUMBER` 与 `RELATED_EVENT_CHANNEL_NUMBER_FIXED` 获取信息。

**EVENT COUNTER**

Event Channel 可以维护一个自由运行计数器。每触发一次该 Event，计数器加 1，不论 DAQ 测量是否正在运行。若实现该计数器，则其大小为 BYTE，并在 `0xFF` 后回卷。

是否支持 Event Counter 可通过 `DTO_CTR_PROPERTIES` 或 A2L 的 `EVENT_COUNTER_PRESENT` 判断。

**DTO CTR DAQ MODE**

该属性定义方向为 DAQ 的 DAQ List 如何产生 DTO CTR。它可以固定，也可以通过 `DTO_CTR_PROPERTIES` 配置。

如果某 DAQ List 绑定的 Event Channel 要插入 DTO CTR，则根据模式使用关联 Event Channel 的：

- `INSERT_COUNTER`：插入自由运行 Event Counter；
- `INSERT_STIM_COUNTER_COPY`：插入最近一次成功 STIM Cycle 保存的 DTO CTR 副本。

若 Event Channel 关联到自身，插入自由运行计数器时应使用**递增后的新值**；STIM Counter Copy 同理按规范定义处理。

A2L 中可通过 `DTO_CTR_DAQ_MODE_FIXED` 与 `DTO_CTR_DAQ_MODE` 描述。

**DTO CTR STIM MODE**

该属性定义 STIM 方向 DAQ List 收到 DTO CTR 时如何处理：

- `CHECK_COUNTER`：将 DTO CTR 与关联 Event Channel 的自由运行计数器比较；
- `DO_NOT_CHECK_COUNTER`：不检查 DTO CTR。

如果 Event Channel 关联到自身，检查时应与自由运行计数器**递增前的旧值**进行比较。

可通过 `DTO_CTR_PROPERTIES` 或 A2L 的 `DTO_CTR_STIM_MODE`、`DTO_CTR_STIM_MODE_FIXED` 查询。

**STIM DTO CTR COPY**

支持 STIM 的 Event Channel 可以在每次成功刺激后保存 DTO CTR。这个副本以后可被 DAQ 方向 Event 引用。是否支持该能力可通过 `DTO_CTR_PROPERTIES` 或 A2L 的 `STIM_DTO_CTR_COPY_PRESENT` 判断。

#### 4.3.5.3 DTO CTR 字段

为了进行 Bypass consistency 检查，DAQ Processor 和 STIM Processor 都必须能处理 DTO CTR 字段。工具可从 A2L 的 `DTO_CTR_FIELD_SUPPORTED` 判断该能力。

如果要对某个 Bypass 做一致性检查，则参与该 Bypass 的所有 DAQ List 都必须通过 `SET_DAQ_LIST_MODE` 设置 `DTO_CTR` 模式位。

此后：

- 对 DAQ 方向：插入关联 Event 的自由运行计数器或 STIM Counter Copy；
- 对 STIM 方向：根据 Event 属性检查 DTO CTR，或者明确不检查。

#### 4.3.5.4 DTO CTR 检查

对于某个 Event Channel，如果所有已启动且绑定于该 Event、方向为 STIM 的 DAQ List 均满足：

- 所有 DTO 都已收到；
- 每个 DAQ List 的第一个 DTO 的 DTO CTR 与关联 Event Channel 的期望值一致；

则 DTO CTR 检查成功。

检查失败后的动作不由 XCP 规范强制规定。实现可以选择：不做处理、通知应用层、向 Master 发送 `EV_STIM_TIMEOUT`，或继续刺激旧数据等。

检查成功后的动作同样由实现决定，例如立即执行数据刺激或继续进行其他校验。

#### 4.3.5.5 Bypass 一致性示例

规范给出了多种典型拓扑。所有示例中，Bypass 工具都需要通过 `SET_DAQ_LIST_MODE` 为相关 DAQ List 设置 `DTO_CTR` 位。

**一个 DAQ Event + 一个 STIM Event**

这是最常见的 Bypass。DAQ Event 推荐配置：

- 存在 Event Counter；
- `DTO CTR DAQ mode = INSERT_COUNTER`；
- Related Event Channel Number 指向自身。

STIM Event 推荐配置：

- `DTO CTR STIM mode = CHECK_COUNTER`；
- Related Event Channel Number 指向 DAQ Event。

DAQ Processor 将 DAQ Event 的自由运行计数器放入 DAQ DTO；Bypass 工具原样把该计数值带回 STIM DTO；STIM Processor 再与 DAQ Event Counter 比较。

**一个 DAQ Event + n 个 STIM Event**

一个 DAQ 周期的数据用于刺激多个 STIM Event。DAQ Event 的计数器放入 DAQ DTO，Bypass 工具将同一个计数值写入各 STIM DTO；各 STIM Event 分别校验该值。

**n 个 DAQ Event + 一个 STIM Event**

多个 Event 的 DAQ 数据共同用于一个 STIM Event。DAQ 方向各 Event 的 `DTO CTR DAQ mode` 设置为 `INSERT_COUNTER`，且 Related Event 指向唯一 STIM Event。STIM Event 自身提供 Event Counter 并执行 `CHECK_COUNTER`。

**单个 Event Channel 同时承担 DAQ 和 STIM**

同一个 Event 同时触发：

- STIM Processor 对上一周期的结果进行刺激；
- DAQ Processor 为下一周期采样输入。

因此至少两个 DAQ List 绑定到同一个 Event，一个方向 DAQ，一个方向 STIM。该 Event 必须具备 Event Counter，并同时配置 `INSERT_COUNTER` 与 `CHECK_COUNTER`，Related Event 指向自身。

STIM 比较的是计数器递增前的旧值；DAQ 插入的是递增后的新值。

**Software-in-the-Loop（SiL）**

DAQ 与 STIM 同时使用的另一场景是 SiL。SiL 工具先刺激 ECU 函数输入，再在函数执行之后采样输出。这种情况下，一致性检查由 SiL 工具负责。

DAQ Event：

- `DTO CTR DAQ mode = INSERT_STIM_COUNTER_COPY`；
- Related Event 指向 STIM Event。

STIM Event：

- 必须支持 `STIM DTO CTR Copy`；
- `DTO CTR STIM mode = DO_NOT_CHECK_COUNTER`。

SiL 工具给 STIM DTO 自行赋 CTR；Slave 成功刺激输入后保存该 CTR；函数执行完毕后，DAQ DTO 带回保存的 STIM CTR Copy，SiL 工具即可验证输出是否对应本次输入。

### 4.3.6 最小间隔时间（MIN_ST_STIM）

Slave 应能够从 Bypass 工具连续接收多个 STIM DTO。如果 Slave 在两个 DTO 之间需要一定处理时间，则必须把该要求告知工具。参数 `MIN_ST_STIM` 即用于描述这种最小 DTO 间隔。

---

## 4.4 在线标定

### 4.4.1 SECTOR、SEGMENT 与 PAGE

Slave 的内存空间在物理上被看作连续地址空间。数据元素使用一个逻辑上的 **40 位地址**引用：

```text
32-bit XCP Address + 8-bit Address Extension
```

XCP 使用三类对象描述标定内存：

- **SECTOR**：描述物理布局，尤其与 Flash 擦写/重编程边界有关；
- **SEGMENT**：描述标定数据在 Slave 内存中的逻辑位置，即“标定对象在哪里”；
- **PAGE**：同一个 SEGMENT 可拥有多个 PAGE，它们表示相同地址上的同一组逻辑数据，但可以具有不同数值或访问属性。

SEGMENT 的起始地址和大小不要求与 SECTOR 边界一致。

对每一个 SEGMENT，任意时刻 ECU 控制算法只能访问其中一个 PAGE，该 PAGE 称为 **active PAGE for ECU access**。

同样，对每一个 SEGMENT，XCP Master 任意时刻也只能通过 XCP 命令访问一个 PAGE，称为 **active PAGE for XCP access**。

ECU Access Page 和 XCP Access Page 可以相互独立切换，而且不同 SEGMENT 的 Active Page 也可以分别切换。


### 4.4.2 逻辑布局：SEGMENT

Slave 内存的逻辑布局使用 **SEGMENT** 描述。SEGMENT 说明可标定数据对象在 Slave 内存中的位置。

SEGMENT 的起始地址和大小不必遵守 SECTOR 的物理边界限制。A2L 中通常使用 `MEMORY_SEGMENT` 描述 SEGMENT，其中包含名称、地址、大小以及镜像 Segment 的 Offset 等信息；XCP 专用信息放在 `IF_DATA` 中。

为了支持 40 位寻址，每个 SEGMENT 都有一个 Address Extension，该值对位于该 SEGMENT 内的所有可标定对象有效。

XCP 使用 `SEGMENT_NUMBER` 引用 SEGMENT。同一个 XCP Slave 内，编号必须从 0 开始连续：

```text
SEGMENT_NUMBER = 0 ... 255
```

### 4.4.3 PAGE 的可访问性

每个 SEGMENT 可以包含多个 PAGE。一个 SEGMENT 的不同 PAGE 描述的是**相同地址上的相同逻辑数据**，但属性可以不同，例如：

- 数值不同；
- 读/写权限不同；
- ECU/XCP 是否可同时访问不同。

每个 SEGMENT 至少必须有一个 PAGE，即 `PAGE 0`。Slave 必须初始化其所有 SEGMENT 的所有 PAGE。某 PAGE 的初始化数据由其 `INIT_SEGMENT` 的 `PAGE 0` 提供。

Master 可通过 `GET_CAL_PAGE` 查询当前 ECU Access 和 XCP Access 分别使用哪个 PAGE。

`ECU_ACCESS_x` 标志描述 ECU 是否以及在什么条件下可访问该 PAGE，例如：

- 仅当 XCP Master 不同时访问时才允许 ECU 访问；
- 仅当 XCP Master 同时访问时才允许 ECU 访问；
- ECU 不关心 XCP Master 是否同时访问。

`XCP_x_ACCESS_y` 标志描述 XCP Master 的访问能力，并区分 `READABLE` / `WRITEABLE`。读访问和写访问都可以进一步规定是否要求 ECU 同时不访问、必须同时访问，或无需关心 ECU 是否访问。

每个 SEGMENT 内 `PAGE_NUMBER` 均从 0 重新编号：

```text
PAGE_NUMBER(segment j) = 0 ... 255
```

### 4.4.4 标定数据 PAGE 切换

若 Slave 实现可选命令 `GET_CAL_PAGE` 和 `SET_CAL_PAGE`，则支持 PAGE Switching。

对每个 SEGMENT：

- ECU 控制算法任意时刻只访问一个 Active ECU PAGE；
- XCP Master 任意时刻只访问一个 Active XCP PAGE。

`GET_CAL_PAGE` 查询当前 Active PAGE；`SET_CAL_PAGE` 修改 Active PAGE。

PAGE 切换由 Master 完全控制，Slave 不允许自主切换。ECU Access PAGE 与 XCP Access PAGE 可独立切换，不同 SEGMENT 也可独立切换。Master 还可以把所有 SEGMENT 同步切换到同一个 PAGE。

执行切换时 Master 必须遵守 `XCP_ACCESS_TYPE` 和 `ECU_ACCESS_TYPE` 给出的约束。

### 4.4.5 标定数据 PAGE 冻结

`GET_PAG_PROCESSOR_INFO` 返回的 `PAG_PROPERTIES.FREEZE_SUPPORTED` 表示是否支持将全部 SEGMENT 置于 Freeze 模式。

- `SET_SEGMENT_MODE`：选择需要 Freeze 的 SEGMENT；
- `GET_SEGMENT_MODE`：查询 SEGMENT 是否已选择用于 Freeze；
- `SET_REQUEST(STORE_CAL_REQ)`：请求 Slave 将标定数据写入非易失存储器。

对于每个处于 Freeze 模式的 SEGMENT，Slave 应把该 SEGMENT 当前 Active XCP PAGE 保存到此 PAGE 所属 `INIT_SEGMENT` 的 `PAGE 0`。

保存完成后，`GET_STATUS` 中的 `STORE_CAL_REQ` 位由 Slave 清零；Slave 可以通过 `EV_STORE_CAL` 事件通知 Master。

### 4.4.6 地址处理

Slave 内存按连续物理地址空间描述，XCP 地址由：

```text
32-bit Address + 8-bit Address Extension
```

组成。Address Extension 取自当前地址所属的 SEGMENT。

`MEMORY_SEGMENT` 描述可用于生成 ECU Flash 镜像的地址范围。判断某个 `CHARACTERISTIC` 属于哪个 `MEMORY_SEGMENT` 时，Master 应：

1. 取得 `CHARACTERISTIC` 中定义的地址；
2. 如适用，应用 `ECU_CALIBRATION_OFFSET`；
3. 如适用，解引用 NearPointer；
4. 判断最终地址落在哪个 `MEMORY_SEGMENT`。

对于 `SET_MTA`、`SHORT_UPLOAD` 和 `SHORT_DOWNLOAD` 实际使用的目标地址，在上述结果基础上还可能需要应用 `ADDRESS_MAPPING`，从 Source Address 映射到 Destination Address。一个 SEGMENT 的不同区域允许使用不同的 `ADDRESS_MAPPING`。

因此实际工具中的地址计算链可以表示为：

```text
A2L CHARACTERISTIC address
       |
       +-- ECU_CALIBRATION_OFFSET
       |
       +-- NearPointer dereference (if needed)
       |
       +-- locate MEMORY_SEGMENT
       |
       +-- ADDRESS_MAPPING (if needed)
       v
XCP MTA / SHORT_UPLOAD / SHORT_DOWNLOAD address
```

### 4.4.7 Master-Slave 操作

Slave 必须能够对 SECTOR 或 SEGMENT 描述的全部地址范围执行 Checksum 计算。

对具有 `XCP_ACCESS_ALLOWED` 的 PAGE，也必须能够执行 Checksum。

若 PAGE 对 Master：

- `READABLE`：可通过 `UPLOAD`、`SHORT_UPLOAD` 读取，支持时也可使用 Block Mode；
- `WRITEABLE`：可通过 `SHORT_DOWNLOAD`、`DOWNLOAD_MAX`、`DOWNLOAD` 写入；支持 Block Mode 时可配合 `DOWNLOAD_NEXT`；
- `WRITEABLE`：还可以使用 `MODIFY_BITS` 对位进行原子修改。

### 4.4.8 PAGE 到 PAGE 操作

如果 Slave 有多个 PAGE，Master 可以使用 `COPY_CAL_PAGE` 在 PAGE 之间复制数据。

原则上任意 SEGMENT 的任意 PAGE 都可以复制到另一个 SEGMENT 的任意 PAGE，但 Slave 可以施加限制，并以 `ERR_PAGE_NOT_VALID`、`ERR_SEGMENT_NOT_VALID` 或 `ERR_WRITE_PROTECTED` 等错误报告。

---

## 4.5 Flash Programming

### 4.5.1 物理布局：SECTOR

Slave 非易失存储器的物理布局使用 **SECTOR** 描述。SECTOR 的起始地址和大小对 Flash 擦除/重编程非常重要。

XCP 通过 `SECTOR_NUMBER` 引用 SECTOR，同一个 Slave 内编号从 0 开始连续：

```text
SECTOR_NUMBER = 0 ... 255
```

### 4.5.2 一般过程

完整 Flash 流程从工程角度通常分为三部分：

1. 编程前管理，例如版本匹配检查；
2. 真正的 Flash 擦除与写入；
3. 编程后管理，例如版本或 Checksum 校验。

XCP 的重点是第 2 部分，即实际 Programming 动作。XCP 提供专用 Programming 命令，但具体项目中命令的使用顺序必须由项目自己的 **Programming Flow Control** 定义；本标准不定义这种项目专用描述文件的统一格式。因此在量产工程中，ECU 供应方和工具供应方仍需约定具体 Flash 流程。

相关命令包括：

```text
PROGRAM_START
PROGRAM_CLEAR
PROGRAM_FORMAT
PROGRAM       # 通常循环调用，可选 Block Transfer
PROGRAM_VERIFY
PROGRAM_RESET
```

编程前的版本检查通常在工具侧完成，用于判断待刷写内容是否匹配当前 ECU。XCP 没有专用“版本检查命令”；项目可以规定使用哪些普通 XCP 命令取得 ECU Identification。

XCP 支持两种 Flash Access Method：

- **Absolute Access Mode**：按真实地址编程；
- **Functional Access Mode**：按 Flash Area/逻辑数据流编程。

擦除阶段与写入阶段甚至可以使用不同 Access Mode。

### 4.5.3 Absolute Access Mode - 按地址访问

Absolute Mode 是默认思路，前提是工具知道 Flash 的物理布局，同时知道待写数据的真实目标地址。

项目可以通过描述文件提供 Memory Layout，也可以通过 ECU/XCP 查询得到部分信息。工具还需要项目专用 Programming Flow Control 中的命令时序。

`PROGRAM` CTO 中携带的数据块被写入以 MTA 为起点的非易失存储器。成功写入后，MTA 按实际写入的数据字节数自动后移。

因此 Absolute Mode 允许目标地址中存在 Gap，只要工具通过 MTA 明确指定每个数据块的目标地址。

### 4.5.4 Functional Access Mode - 按 Flash Area 访问

Functional Mode 适用于工具不掌握物理地址映射、或待刷数据经过压缩/加密的场景。

工具只需要知道 Flash Area。此时 MTA 不再表示实际物理地址，而作为**相对指针/Block Sequence Counter** 使用，从逻辑 0 开始。ECU 自己根据 `PROGRAM_CLEAR` 选择的目标区域以及内部 Flash 逻辑决定真实写入位置和 Gap。

Functional Mode 中，工具必须把新的 Flash 内容作为连续 Data Stream 发送，不允许在 XCP Data Transfer 序列中跳过相对指针。

收到 `PROGRAM_FORMAT` 后，Slave 的 Block Sequence Counter 应初始化为 `1`，第一个后续 `PROGRAM` 请求使用计数值 1，之后每成功处理一个 Data Transfer Request 加 1；达到最大值后回卷至 `0x00`。

该计数器还改善了超时重试：

- 若 Slave 已正确执行 `PROGRAM`，但 Positive Response 丢失，Master 超时后重发相同请求和相同 Block Sequence Counter。Slave 可识别这是重复请求，只重新发送响应，不再次写 Flash；
- 若原请求根本未被 Slave 正确接收，重发时相同 Counter 对 Slave 来说仍是新的有效请求，因此 Slave 正常执行并应答。

Flash Session 结束前可以选择切回 Absolute Access Mode。

Functional Mode 主要影响：`PROGRAM_CLEAR`、`PROGRAM_FORMAT`、`PROGRAM`、`SET_MTA`。

### 4.5.5 Checksum Control 与 Program Verify

实际 Flash 完成后，通常需要验证新内容。XCP 支持：

- `BUILD_CHECKSUM`：由工具读取/计算意义上的 Checksum 检查；
- `PROGRAM_VERIFY`：启动 Slave 内部验证，并把 Verification Value 交给 Slave 检查。

### 4.5.6 Flash Session 结束

整个 Programming Sequence 通过 `PROGRAM_RESET` 结束。执行后 Slave 进入 Disconnected State，通常还会发生 ECU Hardware Reset。

---

## 4.6 时间相关（Time Correlation）

### 4.6.1 引言

早期 XCP 时间相关主要依赖 `GET_DAQ_CLOCK`：Master 与 Slave 成对采样参考时间戳，由 Master 建立时钟映射。本规范把这种方式称为 **Legacy Time Correlation**。

受 Master/Slave 实现和通信基础设施延迟影响，这种方法的精度有限。而现代测量系统可能要求个位数微秒甚至更高的同步精度，因此 XCP 扩展了 **Advanced Time Correlation**。

高级时间相关首先允许 Master 获取 Slave 的 Clock System 详细信息，包括：

- Slave 能观察到多少个 Clock；
- 各 Clock 的类型、状态和特性；
- Clock 是否与外部 Grandmaster 同步/同频；
- Clock 之间的对应关系。

规范给出三类主要技术。

**技术 1：XCP 原生 Multicast 事件**

Master 通过 `GET_DAQ_CLOCK_MULTICAST` 触发一个尽可能同时到达多个 Slave 的事件。所有参与 Slave 在该事件到达时立即采样本地 Timestamp，然后通过扩展 `EV_TIME_SYNC` 返回时间信息。

这样 Master 可以直接相关多个 Slave 的时钟，而不一定要求 Master Clock 本身成为全局参考。

高精度要求：

- Slave 必须在事件到达时尽可能即时采样 Timestamp；
- Master 到各 Slave 的传播延迟应尽可能一致。

由于这需要 Broadcast/Multicast 语义，具体如何在 CAN、Ethernet、FlexRay 等 Transport Layer 上实现，由对应传输层标准规定。

高级模式与传统 XCP Command/RES 不同：`GET_DAQ_CLOCK_MULTICAST` 更像 Master 发起的 Event。支持该能力的 Slave 不返回传统 Positive Response，而是发送扩展的 `EV_TIME_SYNC`。

为保持向后兼容，刚 `CONNECT` 后仍先使用 Legacy 格式。支持高级时间相关的 Master 可通过 `TIME_CORRELATION_PROPERTIES` 的 `SET_PROPERTIES.RESPONSE_FMT` 启用扩展格式。

**技术 2：利用 XCP 之外的全局时钟同步**

例如 IEEE 1588 PTP。若 Slave Clock 已经与某个 Grandmaster：

- synchronized：Epoch/时间值在给定误差范围内一致；
- syntonized：秒的频率一致，但 Epoch 和 Timestamp 表示未必相同；

则 Master 需要知道 Slave 当前同步状态以及对应 Grandmaster 的唯一标识，从而把多个 Slave 组织到同一同步 Clock Domain 中。

**技术 3：Timestamp Tuple**

对于资源有限、无法真正同步本地 Clock 的 Slave，可以向 Master 提供时间戳对：

```text
(local slave timestamp, global synchronized clock timestamp)
```

Master 根据这些 Tuple 在工具侧建立高精度 Clock Correlation。

Advanced Time Correlation 相关协议元素包括：

- `TIME_CORRELATION_PROPERTIES`；
- `GET_DAQ_CLOCK` 扩展 Positive Response；
- 扩展 `EV_TIME_SYNC`；
- CAN/FlexRay/Ethernet Transport Layer 的 `GET_DAQ_CLOCK_MULTICAST` 子命令。

### 4.6.2 XCP Slave Clock Subsystem

支持高级时间同步时，Slave 至少能观察到一个 Clock，也可能有多个。数量取决于硬件/软件架构以及运行时状态，例如 Clock 与 Grandmaster 建立或丢失同步。

#### 4.6.2.1 场景 1：一个可观察 Clock - 自由运行 Slave Clock

最简单情况下，Slave 只有一个可随机读取的 Free-running Clock，DAQ Timestamp 直接基于该 Clock。

收到 `GET_DAQ_CLOCK_MULTICAST` 时，Slave 立即读取该 Clock，并在 `EV_TIME_SYNC` 中返回；对于普通 `GET_DAQ_CLOCK`，概念相同，只是 Timestamp 位于该命令的 Positive Response 中。

#### 4.6.2.2 场景 2：一个可观察 Clock - Slave Clock 与 Grandmaster 同步

Slave 的唯一 Clock 可以通过 IEEE 1588 等方式与外部 Grandmaster 同步。

`GET_DAQ_CLOCK_MULTICAST` 和 `GET_DAQ_CLOCK` 的读取过程与场景 1 基本相同，但 Master 还需要知道：

- 该 Slave Clock 已与外部 Grandmaster 同步；
- Grandmaster Clock 的 Unique ID。

这样，Master 可以把同步到同一 Grandmaster 的多个 Slave 放入同一个 Logical Clock Domain。对于这些 Slave 的 DAQ Timestamp，原则上无需再执行额外的相互 Clock Correlation。


#### 4.6.2.3 场景 3：一个可观察 Clock - Slave Clock 与 Grandmaster 同频（syntonized）

该场景与完全 synchronized 类似，但两个 Clock 在同一时刻读取时存在固定或缓慢变化的 Offset；两者的速率变化已经对齐。

为了把 Timestamp 相互转换，Master 需要知道：

- 两个 Clock 在某个参考 Slave Timestamp 下的 Offset；
- 两个 Clock 的 Basic Clock Rate。

Master 首先发送 `TIME_CORRELATION_PROPERTIES` 请求 Clock Detail。Slave 在 Positive Response 中通过 `CLOCK_INFO = 0x7` 表示可提供 Clock 及相互关系的详细信息，随后 Master 使用 `UPLOAD` 取得这些信息。

#### 4.6.2.4 场景 4：两个可观察 Clock - 自由运行 Slave Clock + 全局同步 Clock

如果 Slave 无法把自己的 Free-running Clock 直接同步到 Grandmaster，但系统中另有一个可以观察且已全局同步的 Clock（例如 Ethernet PHY 内的 IEEE 1588 Clock），则可以同时捕获：

```text
Free-running XCP Slave Clock Timestamp
Global synchronized Clock Timestamp
```

Master 根据同时采样的 Timestamp Pair 建立两者映射，并把本地 DAQ Timestamp 转换到 Grandmaster Time Domain，从而与其他 ECU/Slave 的数据相关。

`GET_DAQ_CLOCK_MULTICAST` 的一个重要价值是让多个 Slave 尽可能同时采样其时钟，同时只占用一次广播发送时隙，相比逐个 Slave 调用 `GET_DAQ_CLOCK` 可降低总线负载。

如果全局同步 Clock 不能随机读取，例如位于 Ethernet PHY 且访问受限，但硬件能够周期性生成 PPS（Pulse Per Second）等触发，则 Slave 可以在该触发发生时锁存本地 XCP Clock，并使用已知的全局 Clock 时刻组成 Timestamp Pair，随后主动发送 `EV_TIME_SYNC`。

#### 4.6.2.5 场景 5：两个可观察 Clock - 自由运行 XCP Slave Clock + ECU Clock

该场景适用于 **External XCP Slave**。XCP Slave 自身有一个 Free-running Clock，同时还可观察 ECU Clock，而实际 DAQ Timestamp 与 ECU Clock 相关。

如果 ECU Clock 可以随机读取，则 Master 理论上可以只看到 ECU Clock；但保留 XCP Slave Clock 的意义在于处理 ECU Clock Reset 等不连续事件。Slave 可以在 ECU Reset Release 时发送 `EV_TIME_SYNC`，同时给出 XCP Slave Clock 与 ECU Clock 的 Timestamp Pair，帮助 Master 在 Reset 前后重建时间相关关系。

如果 ECU Clock 无法随机读取，则 Slave 必须周期性产生 `EV_TIME_SYNC`，发送 XCP Slave Clock/ECU Clock Timestamp Pair。为获得较好精度，两个 Timestamp 应尽可能同时捕获。Reset Release 时也可额外发送 `EV_TIME_SYNC`。

#### 4.6.2.6 场景 6：三个可观察 Clock

该场景组合了前述双 Clock 场景，典型配置为：

```text
ECU Clock                 <- DAQ Timestamp 所基于的时钟
Free-running XCP Clock    <- 用于建立 ECU Clock 与全局 Clock 的桥接
Global synchronized Clock <- 与 Grandmaster 同步
```

在最典型实现中，ECU Clock 和 Global synchronized Clock 都不能随机读取，因此需要通过 Event/Trigger 产生 Timestamp Tuple；Free-running XCP Slave Clock 作为中间桥接时钟。该结构常见于体积较小的 External XCP Slave。

#### 4.6.2.7 场景 7：只有 ECU Clock

某些 XCP Slave 自身没有内部 Clock，但已知所有 DAQ Timestamp 都基于 ECU Clock，同时 ECU Clock 又与其他 Clock/Grandmaster 同步。

由于 Slave 没有可读本地 Clock，且也不能随机读取 ECU Clock，因此无法响应 `GET_DAQ_CLOCK_MULTICAST` 的时间采样请求。

Master 可以先通过 `TIME_CORRELATION_PROPERTIES` 请求 Clock 信息。Slave 以 `CLOCK_INFO = 0x18` 表示可提供 ECU Clock 和其 Grandmaster 的信息，然后 Master 使用 `UPLOAD` 取得详细 Clock Data/UUID。

---

## 4.7 ECU States

### 4.7.1 引言

ECU States 的目的，是在 XCP Session 过程中让 Slave 当前可用能力对 Master 更透明。Slave 可以告诉 Master ECU State 已经发生变化，而不同 Resource 可以随 ECU State 变为 Active 或 Inactive。

一个 ECU State 表示当前所有 XCP Resource 的运行状态，因此 Master 可以动态知道当前哪些能力真正可用。

这对于 **ECU 与 XCP Slave 是不同实例** 的系统尤其重要。例如 External XCP Slave 本身可以继续运行，但实际 ECU 可能尚未启动、已经重启或暂时不可访问。此时 Protocol Handler 与 ECU 之间存在独立连接，Resource Availability 会随 ECU 状态动态改变。

### 4.7.2 向 Master 传递 State 信息

XCP 延续已有 Resource 分类：

- `CAL/PAG`
- `DAQ`
- `STIM`
- `PGM`

`CONNECT` 响应中的 `RESOURCE` 位图描述这些资源在该 Slave 上**总体是否存在**；而 ECU State 描述它们**当前是否处于 Active 状态**。

XCP 有两种向 Master 报告 ECU State 的机制：

1. **强制机制**：Slave 在 `GET_STATUS` Positive Response 中返回当前 `STATE_NUMBER`；
2. **可选异步机制**：若支持 Event Message，State 改变时 Slave 主动发送 `EV_ECU_STATE_CHANGE`，其中携带新的 `STATE_NUMBER`。之后 Master 再次调用 `GET_STATUS` 时仍可读到该 State Number。

`STATE_NUMBER` 的具体语义在 A2L 文件中定义。

DAQ、STIM 和 PGM 可以按 State 直接描述 Active/Inactive。`CAL/PAG` 的粒度更细，因为标定 Memory 不一定全部位于同一个 ECU/Slave 中，某些状态下可能只允许访问部分内存。

因此 A2L 可以通过 `MEMORY_ACCESS` 描述某个 State 下：

- 哪个 SEGMENT/PAGE 可读取；
- 哪个 SEGMENT/PAGE 可写入。

若某 State 中整个 CAL/PAG Resource 都不可用，则无需定义 `MEMORY_ACCESS`。

State 只是在既有 `MEMORY_SEGMENT` PAGE Access 基础上进一步限制访问，不会放宽原始 PAGE Access 规则；关于 ECU/XCP PAGE 切换的限制仍以 `MEMORY_SEGMENT` 定义为基础。

### 4.7.3 A2L 语义一致性

本规范配套的 `XCP_vX_Y_IF_DATA_example.a2l` 在 `/begin ECU_STATES` 部分提供 ECU State 的 IF_DATA 示例。Master 实现应确保其 State 解析与 A2L 中 Resource/Memory Access 的定义保持一致。


# 5 XCP 协议

## 5.1 拓扑

XCP 的基本通信关系是 **single-master / single-slave**。通信总是由 Master 发起；对于 Master 的请求，Slave 必须返回相应 Response。

但 XCP 使用“软”Master/Slave 模式：连接建立以后，Slave 可以自主发送某些消息，包括：

- Event；
- Service Request；
- Data Acquisition Message（DAQ DTO）。

同样，Master 可以发送 Data Stimulation Message（STIM DTO），而不期待 Slave 逐帧直接响应。

Master 每次建立的是与某一个特定 Slave 的连续、逻辑、无歧义点对点连接。一个 Slave Device Driver 不能同时处理多个连接。

因此，严格说 XCP 协议层**并不是一条连接上的 single-master/multi-slave 协议**，Master 不能把普通 XCP 命令广播给多个 Slave。例外包括特定 Transport Layer 上的 `GET_SLAVE_ID` 以及 `GET_DAQ_CLOCK_MULTICAST`。

但是同一个物理网络允许同时存在多个相互独立的 single-master/single-slave 通信通道。Transport Layer 的识别参数（例如 CAN 上的 CAN ID）必须配置成相互独立、可明确区分的连接。

XCP 还允许 Gateway。Master 直接连接的网络称为 **Master Network**；通过 Gateway 间接访问的网络称为 **Remote Network**。Gateway 必须根据两侧 Transport Layer 对 XCP Header/Tail 做适配，并在 Master Network 中逻辑表示 Remote Network 中的节点。

## 5.2 XCP 通信模型

### 5.2.1 Standard Communication Model

Connected 状态下，每个 Request Packet 都对应一个 Response Packet 或 Error Packet，除非具体 Command 描述明确规定例外。

标准模式规则：

```text
Request k
   -> wait
Response k
   ->
Request k+1
```

即 Master 在收到前一个请求的响应之前，不允许发送下一个请求。

### 5.2.2 Block Transfer Communication Model

为提高 Memory Upload、Download 和 Flash Programming 的吞吐率，`UPLOAD`、`DOWNLOAD`、`PROGRAM` 可以支持类似 ISO/DIS 15765-2 的 Block Transfer。

Block Transfer 与 Interleaved Communication Mode 互斥。

**Master Block Mode**

`GET_COMM_MODE_INFO` 的 `COMM_MODE_OPTIONAL.MASTER_BLOCK_MODE_SUPPORTED` 表示是否允许 Master 使用 Master Block Transfer。

Slave 可以限制：

- `MAX_BS`：Maximum Block Size；
- `MIN_ST`：Minimum Separation Time。

这些参数通过 `GET_COMM_MODE_INFO` 获取，Master 必须确保自己的发送行为满足 Slave 限制。

**Slave Block Mode**

`CONNECT` 返回的 `COMM_MODE_BASIC.SLAVE_BLOCK_MODE_SUPPORTED` 表示 Slave 是否支持 Slave Block Transfer。此模式不允许对 Master 能力设置额外限制，连续 Response 的 Separation Time 可以为 0，Master 必须能接收最大可能 Block Size。

### 5.2.3 Interleaved Communication Model

Interleaved 模式允许 Master 在收到上一个 Request 的 Response 之前发送后续 Request，从而提高链路利用率。

Interleaved Mode 与 Block Transfer Mode 互斥。

`GET_COMM_MODE_INFO` 中的 `INTERLEAVED_MODE_SUPPORTED` 表示 Slave 是否支持该模式；`QUEUE_SIZE` 表示 Slave 最多可缓存多少个连续 Request，Master 必须遵守这个限制。

## 5.3 状态机

XCP Slave 主要状态包括：

```text
DISCONNECTED
CONNECTED
RESUME
```

Slave 启动后首先检查非易失存储器中是否存在用于 RESUME 的 DAQ List 配置：

- 不存在 -> 进入 `DISCONNECTED`；
- 存在 -> 进入 `RESUME`，并自动启动保存的 DAQ List。

**DISCONNECTED**

- 没有正常 XCP Session；
- Session Status、DAQ List 运行状态和 Protection Status 均复位；
- 受保护 Resource 再次需要 Seed&Key；
- 默认只处理 `CONNECT`；
- 在 CAN/Ethernet Transport Layer 上还可以例外接受 `GET_SLAVE_ID`。

`CONNECT` 建立与 Slave 的连续逻辑点对点连接并进入 `CONNECTED`。

**CONNECTED**

Slave 处理正常 XCP Command，并返回 Response/Error。

`CONNECT(USER_DEFINED)` 可以在建立 XCP 通信的同时要求 Slave 进入一个项目自定义模式。该模式本身不改变 XCP Driver 协议行为。对于 `CONNECT(USER_DEFINED)`，标准 Timeout Recovery 规则不适用；Master 应每隔 `t6` 重复发送该命令，直到收到 ACK，无需执行 `SYNCH`、Pre-action 或 Action。

`CONNECT(NORMAL)` 建立/恢复普通模式。即使已经处于 CONNECTED，Slave 收到新的 CONNECT 仍必须应答。

**RESUME**

Slave 自动运行保存在 NVM 中且标记为 RESUME 的 DAQ List。在 RESUME 状态中，Slave 默认不处理普通 XCP 命令，只处理 `CONNECT`。收到 CONNECT 后按照连接命令处理，但当前 DTO 数据传输保持运行。

CONNECTED 状态收到 `DISCONNECT` 后进入 DISCONNECTED。

错误严重度：

- S0 - S2：不改变当前连接状态；
- S3 Fatal Error：进入 DISCONNECTED。

## 5.4 Protection Handling

XCP 可以保护以下功能：

- Measurement / Stimulation；
- Calibration；
- Flashing。

标准机制为 Seed&Key：Master 先取得 Seed，使用项目/供应商安全算法计算 Key，再通过 `UNLOCK` 解锁对应 Resource。Key 长度足以支持较长算法输出，包括非对称算法使用场景。

主要命令：

```text
GET_STATUS
GET_SEED
UNLOCK
```

除此之外，项目可能还要求隐藏 ECU 软件/内存内容。以下命令可直接读取内存：

```text
UPLOAD
SHORT_UPLOAD
BUILD_CHECKSUM
```

它们不能完全依赖标准 Resource Unlock 机制来实现“永久不可读”的信息隐藏。因此 Slave 可以直接返回：

```text
ERR_ACCESS_DENIED
```

这与 `ERR_ACCESS_LOCKED` 不同：

- `ERR_ACCESS_LOCKED`：资源当前锁定，但 Master 有机会通过 Seed&Key 解锁；
- `ERR_ACCESS_DENIED`：当前请求不允许，Master 不能通过普通 Unlock 获得该访问权。

无论如何，如果 Master 继续操作需要 Slave Identification，`GET_ID` 所需的信息必须能够读取。

## 5.5 XCP Message / Frame 格式

XCP Message 始终封装在具体 Transport Layer 的 Data Field 中，例如 CAN、TCP/IP 或 UDP/IP。

Transport Layer 必须保证：

- Message 长度和内容不被改变；
- Message 顺序不被改变；
- Message 不被重复。

逻辑结构：

```text
XCP Message / Frame
  = XCP Header
  + XCP Packet
  + XCP Tail
```

其中：

- `XCP Packet` 是与 Transport Layer 无关的通用协议部分；
- `XCP Header` 和 `XCP Tail` 由具体 Transport Layer 定义。

XCP Packet 本身由：

```text
Identification Field
+ optional Timestamp Field
+ Data Field
```

组成。

---

# 6 性能极限

## 6.1 通用性能参数

`MAX_CTO`：CTO Packet 最大字节数。协议范围 `0x08 - 0xFF`。

`MAX_DTO`：DTO Packet 最大字节数。协议范围 `0x0008 - 0xFFFF`。

`MAX_DTO_STIM`：STIM 方向 DTO 最大字节数，范围同样为 `0x0008 - 0xFFFF`。

具体 Transport Layer 可能进一步缩小这些上限。

如果定义了 `MAX_DTO_STIM`，则 `MAX_DTO` 只作用于 DAQ 方向；如果没有定义，则 `MAX_DTO` 同时适用于 DAQ 和 STIM。

## 6.2 DAQ/STIM 专用性能参数

`MAX_EVENT_CHANNEL` 表示 Slave 的 Event Channel 数量；Event Channel 使用 `EVENT_CHANNEL_NUMBER` 标识。

主要范围：

```text
MAX_EVENT_CHANNEL        WORD  0x0000 .. 0xFFFF
EVENT_CHANNEL_NUMBER     WORD  0x0000 .. 0xFFFE
```

`MAX_DAQ` 表示 DAQ List 总数；`MIN_DAQ` 表示预定义、只读 DAQ List 数量；`DAQ_COUNT` 表示动态配置时可分配 DAQ List 数量。DAQ List 通过 `DAQ_LIST_NUMBER` 标识，编号从 0 开始。

ODT 相关：

```text
MAX_ODT_ENTRIES      单个 ODT 最多 Entry 数
ODT_ENTRIES_COUNT    动态配置时实际分配 Entry 数
ODT_ENTRY_NUMBER     Entry 编号，从 0 开始
```

### 6.2.1 DAQ 方向参数

`MAX_ODT`：DAQ List 中最大 ODT 数量。

`MAX_ODT_ENTRY_SIZE_DAQ`：DAQ 方向单个 ODT Entry 所描述数据元素的最大尺寸。

`ODT_COUNT`：动态 DAQ 配置时该 DAQ List 实际分配的 ODT 数量。

`ODT_NUMBER`：ODT 编号，从 0 开始。

### 6.2.2 STIM 方向参数

含义与 DAQ 类似，但使用 `MAX_ODT_ENTRY_SIZE_STIM` 描述 STIM 方向 Entry 的尺寸上限。由于 PID 编码空间不同，STIM 方向 ODT 数量/编号的协议上限也与 DAQ 略有不同。

### 6.2.3 ECU Resource Consumption

规范还定义了估算 DAQ/STIM Measurement Configuration 所消耗 ECU Resource 的方法，重点包括：

- ECU RAM Consumption；
- CPU Execution Time / CPU Load；
- 多核系统中的 Core Load。

这些计算以 Measurement Variable 与其 Event 绑定关系为输入，并使用 A2L `IF_DATA XCP` 中描述的实现相关参数进行估算。

工具可以利用这些结果，在配置的 DAQ 对 ECU RAM 或 CPU 负载超过限制时提前告警，防止测量配置对控制系统造成不可接受的影响。

### 6.2.3.1 ECU RAM Consumption

DAQ Processor 通常要在 ECU RAM 中保存 DAQ Configuration，并可能使用发送/接收队列缓存 DTO。

规范定义的主要 RAM 估算参数包括：

| 参数 | 含义 |
|---|---|
| `ODT_SIZE` | 保存一个 ODT Configuration 所需 Memory Element 数 |
| `ODT_ENTRY_SIZE` | 保存一个 ODT Entry 所需 Memory Element 数 |
| `ODT_DAQ_BUFFER_ELEMENT_SIZE` | DAQ 发送队列中每个 Buffer Element 的大小；若不在 RAM 缓存可为 0 |
| `ODT_STIM_BUFFER_ELEMENT_SIZE` | STIM 接收队列 Buffer Element 大小 |
| `ODT_DAQ_BUFFER_ELEMENT_RESERVE` | DAQ Send Queue 预留比例，可用于考虑 Event Jitter |
| `ODT_STIM_BUFFER_ELEMENT_RESERVE` | STIM Queue 预留比例 |
| `DAQ_SIZE` | 保存一个 DAQ List Configuration 所需 Memory Element 数 |
| `DAQ_MEMORY_LIMIT` | DAQ Configuration 可使用的总 Memory 上限 |

所有 Element Size 与因子都以 Address Granularity `AG` 为基础。例如 `AG = 1` 时，一个 Memory Element 就是一个 Byte。

规范原文给出了 RAM 计算公式；实现工具时应按原公式逐项对 Event、DAQ List、ODT 和 ODT Entry 求和，其中 `ODTPayload(k)` 由 Master 根据实际 Measurement Signal Configuration 计算。

### 6.2.3.2 CPU Execution Time

DAQ 会增加 ECU CPU Load，因为数据需要从其原始 Memory Location 搬运到发送队列，并由 Transport Layer/Lower Layer 继续发送。

规范给出一组用于**估算** CPU Load 的数学模型。该模型并不宣称覆盖所有 ECU 实现，而是用于得到足够可靠的工程估计，使工具能在测量配置过重时告警。

如果 Event 不是周期性的，则公式中的 `CycleTime` 应使用 A2L `IF_DATA` 中定义的 `MIN_CYCLE_TIME`。

多核系统中，Event 的不同处理部分可以分配到不同 Core，例如：

- DAQ List Sample Process 在某 Core；
- Queue Receive/Send Process 在另一个 Core；
- 不同 Event 的不同 DAQ List 可分布在不同 Core。

因此规范分别定义：

- Event Part Load；
- 单 Core Load；
- Total Core Load；
- 全部 CPU Measurement Load。

CPU Load 参数包括：

| 参数 | 含义 |
|---|---|
| `DAQ_FACTOR` | 每个 DAQ List 的基础 CPU 开销 |
| `ODT_FACTOR` | 处理每个 ODT 的基础开销 |
| `ODT_FACTORQueue` | 将每个 ODT 放入传输 Queue 的基础开销 |
| `ODT_ELEMENT_LOAD` | Copy 一个数据 Element 的 CPU 开销 |
| `ODT_ENTRY_FACTOR` | 处理一个 ODT Entry 的基础开销 |
| `SIZE[n]` / `SIZE_FACTOR[n]` | `ODT_ENTRY_SIZE_FACTOR_TABLE` 中不同 Entry Size 对应的 Load Factor |
| `CORE_NR` | Core 编号 |
| `CORE_LOAD_MAX_TOTAL` | 所有 Core 总 Load 上限 |
| `CORE_LOAD_MAX(CORE_NR)` | 指定 Core 的 Load 上限 |
| `CORE_LOAD_EP_MAX` | 单个 Event Part 的 Core Load 上限 |
| `CPU_LOAD_MAX_TOTAL` | 整体 DAQ Measurement 的 CPU Load 上限 |
| `CPU_LOAD_MAXEvent` | 单 Event CPU Load 上限 |

Master 可以把计算结果与这些 Limit 比较并向用户显示负载百分比。

`CPU_LOAD_CONSUMPTION_DAQ` 若被定义，则 `ODT_ENTRY_SIZE_FACTOR_TABLE` 必须至少包含一项。每个记录由 `SIZE` 和对应 `FACTOR` 构成，对指定 Size 或其倍数的 ODT Entry 应用。

若某 Entry Size 没有直接表项：

- 优先选择下一个较小的 Size；
- 如果不存在较小 Size，则选下一个较大 Size；
- 选中的 Size 需要重复计入，直到覆盖实际 Entry Size。

规范示例中，表项 `SIZE=1, FACTOR=150`、`SIZE=4, FACTOR=420`。当 ODT Entry Size 为 13（`3*4+1`）时，按规范选择/累计得到示例 Load `4 * 420 = 1680`；另外还需要加上 ODT 和 DAQ List 自身的固定处理开销。


# 7 XCP 协议层

## 7.1 XCP Packet

### 7.1.1 Packet 类型

全部 XCP 通信都以 **XCP Packet** 形式传输。基本分为两类：

- **CTO - Command Transfer Object**：传输通用控制信息；
- **DTO - Data Transfer Object**：传输同步 DAQ/STIM 数据。

CTO 用于：

- `CMD`：Protocol Command；
- `RES`：Command Positive Response；
- `ERR`：Error Packet；
- `EV`：Event Packet；
- `SERV`：Service Request Packet。

DTO 用于：

- `DAQ`：Slave -> Master 的同步数据采集；
- `STIM`：Master -> Slave 的同步数据刺激。

普通 Command Packet 必须以 Response 或 Error 应答，除非具体命令章节另有规定。

Event、Service Request 和 DAQ Packet 属于异步发送。在 UDP/IP 等无确认 Transport 上，不能保证 Master 一定收到。

如果 XCP Handler 临时无法访问实际 Slave Resource，可以向 Master 返回 `ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE`。

### 7.1.2 Packet 格式

通用 XCP Packet 可抽象为：

```text
+----------------------+----------------+-----------------+-----------+
| Identification Field | Counter (opt.) | Timestamp(opt.) | Data      |
+----------------------+----------------+-----------------+-----------+
```

`MAX_CTO` 与 `MAX_DTO` 分别定义 CTO 和 DTO 的最大长度。

#### 7.1.2.1 Identification Field

Master 与 Slave 必须能够根据 Identification Field 唯一确定 Packet 类型和 Data Field 的解释方式。

XCP Packet 的第一个字节基本总是 **PID（Packet Identifier）**。

**CTO：CTO Packet Code**

CTO 的 Identification Field 只包含 PID，用 PID 区分 CMD、RES、ERR、EV、SERV。

**DTO：Absolute ODT Number**

DTO 必须能够定位到具体 DAQ List 和该 List 内的 ODT。由于每个 DAQ List 内 `ODT_NUMBER` 都从 0 重新开始，因此相对 ODT Number 在整个 Slave 中并不唯一。

一种办法是把相对 ODT Number 映射成 Absolute ODT Number：

```text
absolute_ODT_NUMBER(ODT i in DAQ list j)
  = FIRST_PID(DAQ list j) + relative_ODT_NUMBER(i)
```

`FIRST_PID` 在 `START_STOP_DAQ_LIST` 时由 Slave 告知 Master。Slave 分配 FIRST_PID 时必须保证所有活动 ODT 的 Absolute ODT Number 唯一，且 PID 落在 DAQ/STIM 合法范围内。

这种模式下 Identification Field 只需要一个 PID。

**DTO：Relative ODT Number + Absolute DAQ List Number**

另一种方法是在 DTO 内同时携带：

- PID：Relative ODT Number；
- DAQ 字段：Absolute DAQ List Number。

DAQ List Number 可以用 BYTE 表示，也可以用 WORD 表示。为了满足某些 CPU/Transport 的对齐要求，WORD 模式还可以加入 FILL Byte。

Slave 通过 `GET_DAQ_PROCESSOR_INFO` 返回的 `DAQ_KEY_BYTE` 告知 Master 实际采用哪种 Identification Field Type。Master 发送 STIM 时必须使用相同格式。

**Empty Identification Field / PID_OFF**

如果 `DAQ_PROPERTIES.PID_OFF_SUPPORTED` 置位，某 DAQ List 可以关闭 DTO Identification Field，但仅允许在基础 Identification Type 为 **absolute ODT number** 时使用。

此时 DTO 的唯一识别必须由 Transport Layer 提供。例如 CAN 上可为每个 DAQ List 使用单独 CAN ID，并且每个 List 只包含一个 ODT，这样能够把完整 8 Byte CAN Payload 用于 Signal Data。

#### 7.1.2.2 Counter Field

DTO 可选包含一个 1 Byte `CTR` 字段，位置紧跟 Identification Field。

A2L 的 `DTO_CTR_FIELD_SUPPORTED` 表示 Slave 是否支持 DTO Counter。Master 使用 `SET_DAQ_LIST_MODE` 的 `DTO_CTR` 标志为指定 DAQ List 启用。

DAQ 方向：

- Slave 仅在一个 DAQ List 当前采样周期的**第一个 ODT DTO**中插入 CTR；
- 插入何种 Counter 由 Event Channel 的 DTO CTR 属性决定。

STIM 方向：

- Slave 期望 Master 在一个 DAQ List 当前刺激周期的第一个 ODT DTO 中提供 CTR；
- 如何校验同样由 Event Channel 属性决定。

当 Identification Field Type 为 `relative ODT + absolute DAQ WORD, aligned` 时，为保持对齐，第一个 DTO 直接使用 CTR 替换原来的 FILL Byte，从而避免再增加额外填充。

#### 7.1.2.3 Timestamp Field

DTO 可以包含 Timestamp Field（TS）。若存在 Counter，则 TS 在 CTR 后；否则紧跟 Identification Field。

`GET_DAQ_PROCESSOR_INFO.TIMESTAMP_SUPPORTED` 表示 Slave 是否支持带时间戳的 DAQ/STIM；Master 可通过 `SET_DAQ_LIST_MODE.TIMESTAMP` 开启某个 DAQ List 的 Timestamp Mode。

如果 `GET_DAQ_RESOLUTION_INFO.TIMESTAMP_MODE.TIMESTAMP_FIXED` 置位，Slave 固定使用 Timestamp，Master 不能关闭。

DAQ 方向：Slave 在一个 DAQ Cycle 的**第一个 ODT DTO**中发送当前 Clock Value。

STIM 方向：Master 同样在第一个 ODT DTO 中发送 Timestamp Field，但本规范不定义该字段值的具体含义。

Timestamp Clock 是 Slave 中自由运行、不会被复位或修改的同步数据传输计数器。根据 Timestamp Type，TS 可以是：

- BYTE；
- WORD；
- DWORD。

`GET_DAQ_RESOLUTION_INFO` 的 `TIMESTAMP_MODE` 和 `TIMESTAMP_TICKS` 描述 Timestamp Width 与 Clock Resolution。Master 发送 STIM 时必须使用与 Slave DAQ 相同的 Timestamp Field Type。

#### 7.1.2.4 Data Field

每个 XCP Packet 最终包含 Data Field：

- CTO：包含相应 Command/Response/Event/Service 的参数；
- DTO：包含同步 DAQ/STIM 的实际数据。

### 7.1.3 CTO Packet

CTO 不含 Timestamp Field。Identification Field 只有 PID，Data Field 保存具体 CTO 参数。

#### 7.1.3.1 CMD

```text
Position 0 : BYTE PID = CMD, range 0xC0..0xFF
Position 1..MAX_CTO-1 : Command Data
```

#### 7.1.3.2 RES

成功执行命令时返回：

```text
Position 0 : BYTE RES = 0xFF
Position 1.. : Command-specific response data
```

#### 7.1.3.3 ERR

命令执行失败时返回：

```text
Position 0 : BYTE ERR = 0xFE
Position 1 : BYTE Error Code
Position 2.. : optional error information
```

`0x00` 保留用于 `SYNCH` 同步处理；正常 Error Code 使用 `ERR_* >= 0x01`。

通常 Error Packet 只包含 Error Code，但有例外：

- `BUILD_CHECKSUM` 返回 `ERR_OUT_OF_RANGE (0x22)` 时，可附带允许的 Maximum Block Size（DWORD）；
- `ERR_GENERIC (0x31)` 时，可附带实现相关 Slave Error Code（WORD）。

#### 7.1.3.4 EV

```text
Position 0 : EV = 0xFD
Position 1 : Event Code
Position 2.. : optional Event Information
```

EV 是 Slave 主动发送的异步事件，可选实现且不进行 ACK，因此在非可靠 Transport 上不保证送达。

#### 7.1.3.5 SERV

```text
Position 0 : SERV = 0xFC
Position 1 : Service Request Code
Position 2.. : optional Service Data
```

SERV 用于 Slave 请求 Master 执行某项操作。

### 7.1.4 DTO Packet

DTO 同时承载 DAQ 和 STIM。其布局由：

```text
Identification Field
+ optional Counter
+ optional Timestamp
+ Data
```

构成。Identification Type 与 Timestamp Type 可以自由组合。

#### 7.1.4.1 DAQ Packet

```text
PID range: 0x00..0xFB
```

PID 中包含 Absolute 或 Relative ODT Number，具体 ODT 描述后续 Data Byte 对应哪些 Measurement Element。

#### 7.1.4.2 STIM Packet

```text
PID range: 0x00..0xBF
```

含义与 DAQ 类似，但方向为 Master -> Slave，对应 ODT 描述如何把 DTO Data 写入刺激对象。

### 7.1.5 PID 空间

**Master -> Slave**

- `0x00..0xBF`：STIM ODT PID；
- `0xC0..0xFF`：CMD Code。

**Slave -> Master**

- `0x00..0xFB`：DAQ ODT PID；
- `0xFC`：SERV；
- `0xFD`：EV；
- `0xFE`：ERR；
- `0xFF`：RES。

## 7.2 Event Code

常用 Event：

| Event | Code | 含义 | Severity |
|---|---:|---|---|
| `EV_RESUME_MODE` | `0x00` | Slave 以 RESUME Mode 启动 | S0 |
| `EV_CLEAR_DAQ` | `0x01` | NVM 中 DAQ 配置已清除 | S0 |
| `EV_STORE_DAQ` | `0x02` | DAQ 配置已保存到 NVM | S0 |
| `EV_STORE_CAL` | `0x03` | Calibration Data 已保存到 NVM | S0 |
| `EV_CMD_PENDING` | `0x05` | Slave 请求 Master 重新开始 Timeout 计时 | S1 |
| `EV_DAQ_OVERLOAD` | `0x06` | DAQ Processor Overload | S1 |
| `EV_SESSION_TERMINATED` | `0x07` | Slave 主动结束 Session | S3 |
| `EV_TIME_SYNC` | `0x08` | 传输外部触发的 Timestamp/Clock Correlation 信息 | S0 |
| `EV_STIM_TIMEOUT` | `0x09` | STIM Timeout | S0 |
| `EV_SLEEP` | `0x0A` | Slave 进入 Sleep | S1 |
| `EV_WAKE_UP` | `0x0B` | Slave 离开 Sleep | S1 |
| `EV_ECU_STATE_CHANGE` | `0x0C` | ECU State 改变 | S0 |
| `EV_USER` | `0xFE` | 用户自定义 Event | S0 |
| `EV_TRANSPORT` | `0xFF` | Transport Layer 专用 Event | 由传输层定义 |

## 7.3 Service Request Code

Service Request 是 Slave -> Master 的异步 Packet。Slave 可选实现，但 Master 必须能够处理；Packet 不需要 ACK，因此传输不保证可靠。

| Request | Code | 含义 |
|---|---:|---|
| `SERV_RESET` | `0x00` | Slave 请求被 Reset |
| `SERV_TEXT` | `0x01` | Slave 向 Master 传输 ASCII Text Stream；行结束为 LF 或 CR/LF，可跨多个 Packet，最后一个 Packet 使用 Null-terminated String 表示文本结束 |

## 7.4 Command Code

未实现的可选命令应返回 `ERR_CMD_UNKNOWN`，且不得产生副作用。这样 Master 可以主动探测 Optional Command 能力。

依赖关系：

- 若实现 `GET_SEED`，必须实现 `UNLOCK`；
- 若实现 `SET_CAL_PAGE`，必须实现 `GET_CAL_PAGE`。

### Standard Command

| Command | Code | 支持要求 |
|---|---:|---|
| `CONNECT` | `0xFF` | mandatory |
| `DISCONNECT` | `0xFE` | mandatory |
| `GET_STATUS` | `0xFD` | mandatory |
| `SYNCH` | `0xFC` | mandatory |
| `GET_COMM_MODE_INFO` | `0xFB` | optional |
| `GET_ID` | `0xFA` | optional |
| `SET_REQUEST` | `0xF9` | optional |
| `GET_SEED` | `0xF8` | optional |
| `UNLOCK` | `0xF7` | optional |
| `SET_MTA` | `0xF6` | optional |
| `UPLOAD` | `0xF5` | optional |
| `SHORT_UPLOAD` | `0xF4` | optional |
| `BUILD_CHECKSUM` | `0xF3` | optional |
| `TRANSPORT_LAYER_CMD` | `0xF2` | optional |
| `USER_CMD` | `0xF1` | optional |

### Calibration Command

| Command | Code | 支持要求 |
|---|---:|---|
| `DOWNLOAD` | `0xF0` | mandatory（CAL/PAG Resource 可用时） |
| `DOWNLOAD_NEXT` | `0xEF` | optional |
| `DOWNLOAD_MAX` | `0xEE` | optional |
| `SHORT_DOWNLOAD` | `0xED` | optional |
| `MODIFY_BITS` | `0xEC` | optional |

### Page Switching Command

`SET_CAL_PAGE 0xEB`、`GET_CAL_PAGE 0xEA`、`GET_PAG_PROCESSOR_INFO 0xE9`、`GET_SEGMENT_INFO 0xE8`、`GET_PAGE_INFO 0xE7`、`SET_SEGMENT_MODE 0xE6`、`GET_SEGMENT_MODE 0xE5`、`COPY_CAL_PAGE 0xE4`，均为 Optional。

### DAQ/STIM Command

基本命令：

- `SET_DAQ_PTR 0xE2` - mandatory；
- `WRITE_DAQ 0xE1` - mandatory；
- `SET_DAQ_LIST_MODE 0xE0` - mandatory；
- `START_STOP_DAQ_LIST 0xDE` - mandatory；
- `START_STOP_SYNCH 0xDD` - mandatory。

Optional：`WRITE_DAQ_MULTIPLE 0xC7`、`READ_DAQ 0xDB`、`GET_DAQ_CLOCK 0xDC`、`GET_DAQ_PROCESSOR_INFO 0xDA`、`GET_DAQ_RESOLUTION_INFO 0xD9`、`GET_DAQ_LIST_MODE 0xDF`、`GET_DAQ_EVENT_INFO 0xD7`、`DTO_CTR_PROPERTIES 0xC5`。

Static DAQ：`CLEAR_DAQ_LIST 0xE3` mandatory，`GET_DAQ_LIST_INFO 0xD8` optional。

Dynamic DAQ：`FREE_DAQ 0xD6`、`ALLOC_DAQ 0xD5`、`ALLOC_ODT 0xD4`、`ALLOC_ODT_ENTRY 0xD3`，在支持 Dynamic DAQ 时均为 mandatory。

### Programming Command

`PROGRAM_START 0xD2`、`PROGRAM_CLEAR 0xD1`、`PROGRAM 0xD0`、`PROGRAM_RESET 0xCF` 为核心命令；其余包括 `GET_PGM_PROCESSOR_INFO 0xCE`、`GET_SECTOR_INFO 0xCD`、`PROGRAM_PREPARE 0xCC`、`PROGRAM_FORMAT 0xCB`、`PROGRAM_NEXT 0xCA`、`PROGRAM_MAX 0xC9`、`PROGRAM_VERIFY 0xC8`。

### Time Correlation

`TIME_CORRELATION_PROPERTIES = 0xC6`，Optional。

## 7.5 Command 描述的通用规则

后续章节定义全部 XCP Command Packet 与 Response。

- 未使用、标记为 `reserved` 的 Byte 必须填 0；
- WORD 参数必须从可被 2 整除的位置开始；
- DWORD 参数必须从可被 4 整除的位置开始；
- Multi-byte Parameter 的字节序（MOTOROLA/INTEL）由 Slave 决定，并在连接信息中说明。

通用 Command：

```text
Position 0 : CMD Code
Position 1..MAX_CTO-1 : Command-specific parameters
```

Positive Response：

```text
Position 0 : RES = 0xFF
Position 1.. : response-specific parameters
```

Negative Response：

```text
Position 0 : ERR = 0xFE
Position 1 : Error Code
Position 2.. : optional command-specific error information
```

以下译文仅在 Response/Error 带有特殊参数时单独列出；普通只有 `RES 0xFF` 或 `ERR + code` 的情况不重复展开。


## 7.5.1 标准命令（Standard Commands）

### 7.5.1.1 `CONNECT`——与 Slave 建立连接

**类别：** Standard，Mandatory。命令码 `0xFF`。

`CONNECT` 用于在 Master 与 Slave 之间建立持续的、逻辑上的点到点 XCP 会话。Slave 未进入 `CONNECTED` 状态时，除自动检测相关操作外，不响应其他 XCP 命令。`Mode=0x00` 表示普通模式；`Mode=0x01` 表示用户自定义模式，可在建立连接的同时要求 Slave 进入供应商定义的特殊模式。

成功响应包含 XCP Master 初始化后续通信所需的关键能力信息：

| 字段 | 含义 |
|---|---|
| `RESOURCE` | Slave 提供的资源：CAL/PAG、DAQ、STIM、PGM |
| `COMM_MODE_BASIC` | 基本通信属性，包括字节序、地址粒度、Slave Block Mode 等 |
| `MAX_CTO` | 最大 CTO 长度，单位 Byte |
| `MAX_DTO` | 最大 DTO 长度，单位 Byte |
| Protocol Layer Version | XCP Protocol Layer 主版本号 |
| Transport Layer Version | 当前 Transport Layer 主版本号 |

`RESOURCE` 位定义中：bit0=`CAL/PAG`，bit2=`DAQ`，bit3=`STIM`，bit4=`PGM`。若某资源位为 1，则该资源规定的 Mandatory Command 必须由 Slave 实现。但即使资源存在，运行时也可能暂时不可访问，此时可返回 `ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE`。

`COMM_MODE_BASIC` 中最重要的两个信息是 **Byte Order** 与 **Address Granularity (AG)**：

- `BYTE_ORDER=0`：Intel，小端格式；
- `BYTE_ORDER=1`：Motorola，大端格式，MSB 位于较低地址/位置；
- `AG=BYTE`：1 Byte/Address；
- `AG=WORD`：2 Byte/Address；
- `AG=DWORD`：4 Byte/Address。

地址粒度不是“变量数据类型”，而是 **一个 Slave 地址单位对应多少字节**。因此 Master 在 A2L 地址运算、MTA 增量、UPLOAD/DOWNLOAD 长度计算时必须使用 AG。协议要求：

```text
MAX_CTO mod AG = 0
MAX_DTO mod AG = 0
```

凡是描述 Slave 地址空间长度的字段，通常以 `AG`（Element）为单位；描述 XCP 数据流本身长度的字段则以 Byte 为单位。这一点对实现 C++ Master 非常关键。

### 7.5.1.2 `DISCONNECT`——断开连接

**类别：** Standard，Mandatory。命令码 `0xFE`。

使 Slave 进入 `DISCONNECTED` 状态。若当前状态不允许断开，Slave 返回 `ERR_CMD_BUSY`。

### 7.5.1.3 `GET_STATUS`——获取当前会话状态

**类别：** Standard，Mandatory。命令码 `0xFD`。

用于获取 Slave 当前的资源保护、DAQ 运行状态、RESUME 状态，以及 NVM 保存/清除请求状态。响应包括：

```text
Current Session Status
Current Resource Protection Status
STATE_NUMBER
Session Configuration ID
```

`Current Session Status` 的关键位包括：

| 位 | 名称 | 含义 |
|---|---|---|
| bit7 | `RESUME` | Slave 当前处于 RESUME Mode |
| bit6 | `DAQ_RUNNING` | 至少一个 DAQ List 正在运行 |
| bit3 | `CLEAR_DAQ_REQ` | 存在清除持久化 DAQ 配置请求 |
| bit2 | `STORE_DAQ_REQ` | 存在保存 DAQ 配置请求 |
| bit0 | `STORE_CAL_REQ` | 存在保存标定数据请求 |

保存/清除请求完成后 Slave 应清掉对应标志，也可以通过 `EV_STORE_CAL`、`EV_STORE_DAQ`、`EV_CLEAR_DAQ` Event Packet 通知 Master。

`Current Resource Protection Status` 使用与 `RESOURCE` 相同的 CAL/PAG、DAQ、STIM、PGM 位，但语义是“当前是否被 Seed&Key 保护”。Standard Command 永远不受 Seed&Key 保护；若受保护资源未成功执行 `GET_SEED + UNLOCK`，相关命令返回 `ERR_ACCESS_LOCKED`。

若 Slave 支持 ECU State，`STATE_NUMBER` 返回当前 ECU 状态编号。`Session Configuration ID` 与之前 `SET_REQUEST(STORE_DAQ_REQ)` 保存的 DAQ 配置关联，可用于 Master 验证 RESUME 后自动启动的 DAQ 配置是否正是预期配置。

### 7.5.1.4 `SYNCH`——超时后的命令执行同步

**类别：** Standard，Mandatory。命令码 `0xFC`。

用于通信超时后的 Master/Slave 状态重新同步。该命令具有一个特殊行为：Slave 对 `SYNCH` **始终以 Negative Response `ERR_CMD_SYNCH` 应答**。Master 看到该特定错误即可确认 Slave 已重新回到可接收正常 Command 的同步状态；因此这里的“Error”实际上承担同步确认作用。

### 7.5.1.5 `GET_COMM_MODE_INFO`——读取扩展通信模式信息

**类别：** Standard，Optional。命令码 `0xFB`。

只有 `CONNECT` 响应中的 `OPTIONAL` 标志表明扩展信息可用时才有意义。返回内容包括：

- `COMM_MODE_OPTIONAL`：Master Block Mode / Interleaved Mode 等能力；
- `MAX_BS`：Block Mode 最大块大小；
- `MIN_ST`：最小分离时间，单位为 100 μs；
- `QUEUE_SIZE`：Interleaved Mode 的队列深度；
- XCP Driver Version：高/低 nibble 分别表示 major/minor。

Master 应根据这里的能力决定是否启用块传输或交错通信，而不能仅因 Transport Layer 带宽足够就自行假设支持。

### 7.5.1.6 `GET_ID`——获取 Slave 标识或 A2L 信息

**类别：** Standard，Optional。命令码 `0xFA`。

Master 通过 `Identification Type` 指定要获取的标识信息。标准类型包括：

| Type | 含义 |
|---:|---|
| 0 | ASCII 类型的通用标识字符串 |
| 1 | ASAM MCD-2 MC 文件名，不含路径和扩展名 |
| 2 | A2L 文件完整路径/文件名 |
| 3 | 指向 A2L 的 URL |
| 4 | 直接从 Slave 上传 A2L 内容 |
| 128..255 | 用户自定义 |

响应中的 `Length` 始终以 Byte 表示。`TRANSFER_MODE=1` 时，标识数据直接附在响应后部；`TRANSFER_MODE=0` 时，Slave 自动把 MTA 指向标识数据，Master 再通过 `UPLOAD` 读出。

若 Type=4 且 `COMPRESSED_ENCRYPTED=1`，表示上传的 A2L 数据被压缩和/或加密，Master 必须调用供应商提供的外部解压/解密函数。标识字符串为纯 ASCII Byte Stream，不包含结尾 `\0`。

### 7.5.1.7 `SET_REQUEST`——请求保存/清除非易失数据

**类别：** Standard，Optional。命令码 `0xF9`。

`Mode` 可以发起：

- `STORE_CAL_REQ`：把 Calibration Data 保存到非易失存储；
- `STORE_DAQ_REQ_NO_RESUME`：保存已选择的 DAQ Lists，但上电后不自动运行；
- `STORE_DAQ_REQ_RESUME`：保存 DAQ Lists，并使 Slave 进入 RESUME Mode；
- `CLEAR_DAQ_REQ`：清除非易失存储中的 DAQ 配置。

要保存的 DAQ List 必须先通过 `START_STOP_DAQ_LIST(Select)` 选中。保存 DAQ 时 Slave 还要保存 `Session Configuration ID`；Master 后续可用该 ID 判断恢复出的配置是否匹配当前配置。

执行 `CLEAR_DAQ_REQ` 时，持久化的 ODT Entry 应恢复为：

```text
address    = 0
extension  = 0
size       = 0
bit_offset = 0xFF
Session Configuration ID = 0
```

如果 Slave 不支持请求的模式，返回 `ERR_OUT_OF_RANGE`。

### 7.5.1.8 `GET_SEED`——读取解锁 Seed

**类别：** Standard，Optional（与 `UNLOCK` 成对）。命令码 `0xF8`。

Seed&Key 每次只解锁一个资源：CAL/PAG、DAQ、STIM 或 PGM。`Mode=0` 请求 Seed 的第一部分，响应同时给出 Seed 总长度；当 Seed 长于 `MAX_CTO-2` 时，继续用 `Mode=1` 请求后续部分。若没有先执行 `Mode=0` 就直接执行 `Mode=1`，返回 `ERR_SEQUENCE`。

若一次请求 0 个或多个 Resource，返回 `ERR_OUT_OF_RANGE`。若响应 `Length=0`，说明该资源没有保护，无需执行 `UNLOCK`。

Master 不应假设 Seed→Key 算法属于 XCP 标准。该算法是 **供应商自定义** 的；外部函数文件名来自 A2L，Master 调用供应商提供的 DLL/SO 等实现计算 Key。

### 7.5.1.9 `UNLOCK`——发送 Key 并解锁资源

**类别：** Standard，Optional。命令码 `0xF7`。

`UNLOCK` 必须紧跟一个成功的 `GET_SEED` 流程。第一个 `UNLOCK` 的 Length 字段填 Key 的总长度；若 Key 大于 `MAX_CTO-2`，后续通过连续的 `UNLOCK` 发送剩余部分，并在 Length 中给出剩余长度。

只有所有 Key Byte 都接收完后 Slave 才校验 Key。顺序错误返回 `ERR_SEQUENCE`；Key 错误返回 `ERR_ACCESS_LOCKED`，并且 Slave 随后进入 `DISCONNECTED` 状态。成功响应包含新的 `Current Resource Protection Status`。

### 7.5.1.10 `SET_MTA`——设置 Memory Transfer Address

**类别：** Standard，Optional。命令码 `0xF6`。

将 Slave 内部的 MTA 设置为：

```text
Address Extension : 8 bit
Address           : 32 bit
```

合起来可视为一个带扩展空间的 40-bit 地址描述。后续 `UPLOAD`、`DOWNLOAD`、`BUILD_CHECKSUM` 以及 Programming Command 等都可以基于当前 MTA 工作。

### 7.5.1.11 `UPLOAD`——Slave → Master 内存上传

**类别：** Standard，Optional。命令码 `0xF5`。

从当前 MTA 指向的位置读取 `Number of Data Elements`，发送给 Master，并把 MTA 向后递增相同数量的 Element。

普通模式下：

```text
1 <= NumberOfElements <= MAX_CTO / AG - 1
```

若支持 Slave Block Mode，可请求更大的块并由 Slave 连续发送多个响应 Packet。Master 必须负责检测数据丢失及保持块传输状态一致性。Response 中实际数据的排布要符合当前 `AG=BYTE/WORD/DWORD` 的对齐规则。

### 7.5.1.12 `SHORT_UPLOAD`——带地址的一次性读取

**类别：** Standard，Optional。命令码 `0xF4`。

这是轮询读取变量时非常实用的命令。请求中直接带：

```text
Number of Data Elements
Address Extension
32-bit Address
```

因此无需先执行 `SET_MTA`。读取完成后，Slave 仍会把内部 MTA 更新到被读数据块末尾之后。

范围为：

```text
1 <= NumberOfElements <= MAX_CTO / AG
```

`SHORT_UPLOAD` 不使用 Block Transfer。对于“按变量名 → A2L 地址 → 读取当前值”的 CANape-like 第一版工具，这是最直接的 Polling Command。

### 7.5.1.13 `BUILD_CHECKSUM`——计算内存校验和

**类别：** Standard，Optional。命令码 `0xF3`。

从当前 MTA 开始，对指定 Block Size 的内存计算 Checksum；完成后 MTA 向后递增 Block Size。标准定义的算法编号包括：

| Code | 算法 |
|---:|---|
| `0x01` | `XCP_ADD_11` |
| `0x02` | `XCP_ADD_12` |
| `0x03` | `XCP_ADD_14` |
| `0x04` | `XCP_ADD_22` |
| `0x05` | `XCP_ADD_24` |
| `0x06` | `XCP_ADD_44` |
| `0x07` | `XCP_CRC_16` |
| `0x08` | `XCP_CRC16_CITT` |
| `0x09` | `XCP_CRC_32` |
| `0xFF` | `XCP_USER_DEFINED` |

CRC 参数：

| 算法 | Width | Poly | Init | RefIn | RefOut | XORout |
|---|---:|---:|---:|---|---|---:|
| XCP_CRC_16 | 16 | `0x8005` | `0x0000` | TRUE | TRUE | `0x0000` |
| XCP_CRC16_CITT | 16 | `0x1021` | `0xFFFF` | FALSE | FALSE | `0x0000` |
| XCP_CRC_32 | 32 | `0x04C11DB7` | `0xFFFFFFFF` | TRUE | TRUE | `0xFFFFFFFF` |

如果 MTA 或 Block Size 不满足 Slave 的对齐约束，Slave 可返回 `ERR_OUT_OF_RANGE`，并附带所需的 `MTA_BLOCK_SIZE_ALIGN`；如果 Block Size 超过最大值，也返回 `ERR_OUT_OF_RANGE` 并给出允许的最大块大小。

当 Checksum Type 为 `XCP_USER_DEFINED` 时，算法由外部函数实现，函数文件由 A2L 中的描述指定。

### 7.5.1.14 `TRANSPORT_LAYER_CMD`

**类别：** Standard，Optional。命令码 `0xF2`。

用于承载与具体 Transport Layer 相关、而非通用 Protocol Layer 的子命令。Sub-command 与参数由相应的 XCP on CAN / Ethernet / USB / FlexRay / SxI Associated Standard 定义。例如 XCP on CAN 可以在此定义 Slave ID 发现相关操作。

### 7.5.1.15 `USER_CMD`

**类别：** Standard，Optional。命令码 `0xF1`。

为供应商/用户自定义功能预留的命令入口。自定义功能不应重复 XCP 标准已经定义的服务，避免出现两套语义不同但功能重叠的协议接口。

## 7.5.2 Calibration Command

### 7.5.2.1 `DOWNLOAD`——Master → Slave 写内存

**类别：** Calibration，Mandatory（CAL/PAG Resource 可用时）。命令码 `0xF0`。

从当前 MTA 开始，把 Command 中的 `n` 个数据 Element 写入 Slave 内存，完成后 MTA 自动增加 `n`。`ELEMENT` 的宽度由 Address Granularity 决定：AG=1/2/4 时分别为 BYTE/WORD/DWORD；AG=DWORD 时需要额外的对齐 Byte。

普通模式中数据必须装进一个 CTO；若 Slave 支持 Block Transfer，可由一个 `DOWNLOAD` 加若干 `DOWNLOAD_NEXT` 组成块下载。Slave 在开始写之前必须确认有资源完成**整个请求**；资源不足时返回 `ERR_MEMORY_OVERFLOW`，且不能造成部分内存已经被修改的状态，这意味着一次被拒绝的下载在协议语义上应保持原子性。

Block Mode 的最大连续 Packet 数由 `GET_COMM_MODE_INFO` 返回的 `MAX_BS` 约束，Packet 间最小时间由 `MIN_ST` 约束。无错误时 Slave 只对最后一个 `DOWNLOAD_NEXT` 给出 Positive Response；若中途出现内部错误，可立即返回 Negative Response。

### 7.5.2.2 `DOWNLOAD_NEXT`——Block Mode 后续数据

**类别：** Calibration，Optional。命令码 `0xEF`。

用于继续前一个 `DOWNLOAD` 的 Block Transfer。其结构与 `DOWNLOAD` 类似，但 Length 字段表示**尚待发送的数据 Element 数**，Slave 利用它检测丢包或乱序。

若当前 Length 与 Slave 期望的剩余数量不一致，则返回：

```text
ERR_SEQUENCE
Expected Number of Data Elements
```

因此 Master 在 Block Download 中必须维护严格的剩余计数，不能仅依赖底层总线的帧顺序。

### 7.5.2.3 `DOWNLOAD_MAX`——固定最大长度写入

**类别：** Calibration，Optional。命令码 `0xEE`。

一次写入固定的：

```text
MAX_CTO / AG - 1
```

个 Element，数据从当前 MTA 开始，写完后 MTA 自动移动。该命令没有显式 Length 字段，适合已知固定满载 CTO 的场景。

Slave 同样要在实际写入前确认能够完成全部请求；否则 `ERR_MEMORY_OVERFLOW` 且内存不得部分改变。`DOWNLOAD_MAX` 不支持 Block Transfer，也不能插入 Block Download 序列中。

### 7.5.2.4 `SHORT_DOWNLOAD`——带地址的短写入

**类别：** Calibration，Optional。命令码 `0xED`。

请求中直接携带：`Number of Data Elements + Address Extension + Address + Data`，因此不需要提前 `SET_MTA`。写完后 MTA 指向数据块末尾之后。

如果数据长度超过 `(MAX_CTO-8)/AG`，返回 `ERR_OUT_OF_RANGE`。同样要求资源不足时整个请求不产生部分写入。

特别需要注意：当 `MAX_CTO=8` 时（典型传统 XCP on CAN），8 Byte 已全部被 Command Header/Address 占用，因此 `SHORT_DOWNLOAD` **无法携带任何数据，实际上没有写入用途**。这也是实际 CAN 工具通常使用 `SET_MTA + DOWNLOAD` 的原因。

### 7.5.2.5 `MODIFY_BITS`——原子位修改

**类别：** Calibration，Optional。命令码 `0xEC`。

对 MTA 指向的 32-bit 内存位置执行基于 16-bit AND/XOR Mask 的位操作。协议给出的核心计算为：先通过 AND Mask 清指定 Bit，再通过 XOR Mask 翻转指定 Bit；`Shift Value S` 把两组 16-bit Mask 一起向高位移动，因此能够操作 32-bit Value 的任意 16-bit 窗口。

例如设置某 Bit 为 0：对应 AND Mask Bit 设 0、XOR Mask Bit 设 0；设置某 Bit 为 1：先通过 AND 清零，再通过 XOR 翻转为 1。执行该命令**不会修改 MTA**。

## 7.5.3 Page Switching Command

### 7.5.3.1 `SET_CAL_PAGE`

**类别：** Page Switching，Optional。命令码 `0xEB`。

指定某个 Calibration `SEGMENT` 的某个逻辑 `PAGE` 分别用于 ECU 访问、XCP Master 访问，或两者同时访问。Mode 中：

- `ECU`：该 Page 供 ECU Application 使用；
- `XCP`：该 Page 供 XCP Driver/Master 访问；
- `ALL`：忽略 Segment Number，对全部 Segment 应用设置。

若请求组合不可行，返回 `ERR_MODE_NOT_VALID`；Page/Segment 不存在时分别返回 `ERR_PAGE_NOT_VALID` / `ERR_SEGMENT_NOT_VALID`。

### 7.5.3.2 `GET_CAL_PAGE`

**类别：** Page Switching，Optional。命令码 `0xEA`。

查询指定 Segment 当前处于激活状态的 Calibration Page。Access Mode 只允许：

```text
0x01 = ECU access
0x02 = XCP access
```

响应返回对应的逻辑 Page Number。

### 7.5.3.3 `GET_PAG_PROCESSOR_INFO`

**类别：** Page Switching，Optional。命令码 `0xE9`。

返回 Paging 子系统的总体能力：

- `MAX_SEGMENT`：Slave 中 Segment 总数；
- `PAG_PROPERTIES.FREEZE_SUPPORTED`：是否支持将 Segment 设置为 FREEZE，从而配合 `STORE_CAL_REQ` 保存标定页数据。

### 7.5.3.4 `GET_SEGMENT_INFO`

**类别：** Page Switching，Optional。命令码 `0xE8`。

根据 `Mode` 查询一个 Segment 的不同信息：

**Mode 0：基础地址信息**

- `SEGMENT_INFO=0` → 返回 Segment Address；
- `SEGMENT_INFO=1` → 返回 Segment Length。

**Mode 1：标准属性**

返回：

```text
MAX_PAGES
ADDRESS_EXTENSION
MAX_MAPPING
Compression Method
Encryption Method
```

`ADDRESS_EXTENSION` 是访问这个 Segment 内 Page 时供 `SET_MTA`、`SHORT_UPLOAD`、`SHORT_DOWNLOAD` 使用的地址扩展。Compression/Encryption Method 应与要刷入的新 Flashware Segment 相匹配。

**Mode 2：Address Mapping 信息**

由 `MAPPING_INDEX` 指定映射区间，再按 `SEGMENT_INFO` 分别读取 Source Address、Destination Address 或 Length。

若 Segment 无效返回 `ERR_OUT_OF_RANGE`。

### 7.5.3.5 `GET_PAGE_INFO`

**类别：** Page Switching，Optional。命令码 `0xE7`。

查询指定 Segment/Page 的访问属性以及 `INIT_SEGMENT`。Page Property 详细描述 ECU 和 XCP 对该 Page 的并发访问条件：

| 访问者 | 能力 |
|---|---|
| ECU | 不允许 / 仅 XCP 不访问时 / 仅 XCP 同时访问时 / 无所谓 |
| XCP Read | 不允许 / 仅 ECU 不访问时 / 仅 ECU 同时访问时 / 无所谓 |
| XCP Write | 不允许 / 仅 ECU 不访问时 / 仅 ECU 同时访问时 / 无所谓 |

这组属性允许工具判断某 Page 是否能作为“ECU 工作页”和“XCP 编辑页”同时使用，避免工具在不允许并发写的 Page 上直接标定。

一个 Page 的 `INIT_SEGMENT` 的 Page 0 包含该 Page 的初始化数据。

### 7.5.3.6 `SET_SEGMENT_MODE`

**类别：** Page Switching，Optional。命令码 `0xE6`。

设置 Segment 的 `FREEZE` 标志。`FREEZE=1` 表示该 Segment 被选入随后 `STORE_CAL_REQ` 的冻结/持久化处理；`FREEZE=0` 取消。Segment 不存在时返回 `ERR_OUT_OF_RANGE`。

### 7.5.3.7 `GET_SEGMENT_MODE`

**类别：** Page Switching，Optional。命令码 `0xE5`。

读取指定 Segment 当前的 Mode，主要用于查询 `FREEZE` 状态。

### 7.5.3.8 `COPY_CAL_PAGE`

**类别：** Page Switching，Optional。命令码 `0xE4`。

强制 Slave 将一个 Segment/Page 复制到另一个 Segment/Page。原则上允许任意 Page 间复制，但 Slave 可以施加限制。若目标区域例如位于 Flash、不能直接写，返回 `ERR_WRITE_PROTECTED`，此时应走 Flash Programming 流程；Page 或 Segment 无效时返回相应错误。

## 7.5.4 Data Acquisition and Stimulation Command

这一组命令是实现 CANape 类实时测量工具最核心的协议部分。基本思想是：Master 先定义“哪些内存变量放进哪些 ODT/DAQ List”，再把 DAQ List 绑定到 ECU 的 Event Channel；开始后，Slave 在 Event 触发时主动采样并发送 DTO，而不是等待 Master 逐变量轮询。

### 7.5.4.1 `SET_DAQ_PTR`——设置 ODT Entry 配置指针

**类别：** DAQ/STIM Basic，Mandatory。命令码 `0xE2`。

设置后续 `WRITE_DAQ` / `READ_DAQ` 操作的 DAQ Pointer：

```text
DAQ_LIST_NUMBER
ODT_NUMBER
ODT_ENTRY_NUMBER
```

其中 ODT Number 是 DAQ List 内的相对编号，ODT Entry Number 是 ODT 内的相对编号。指定对象不存在时返回 `ERR_OUT_OF_RANGE`。

### 7.5.4.2 `WRITE_DAQ`——写一个 ODT Entry

**类别：** DAQ/STIM Basic，Mandatory。命令码 `0xE1`。

向当前 DAQ Pointer 指向的 Entry 写入：

```text
BIT_OFFSET
Size [AG]
Address Extension
32-bit Address
```

只能修改 configurable DAQ List；若之前 `SET_DAQ_PTR` 指向 PREDEFINED DAQ List，返回 `ERR_WRITE_PROTECTED`。

`BIT_OFFSET=0xFF` 表示普通数据 Element；`BIT_OFFSET=0..31` 表示该 Entry 描述单个 Bit 的状态。对于 bitwise STIM，A2L 中 `BIT_MASK` 需要由 Master 转换成 `BIT_OFFSET`，例如 `BIT_MASK=0x80` 对应 bit7，即 `BIT_OFFSET=0x07`。

Entry 的地址和 Size 必须满足 `GET_DAQ_RESOLUTION_INFO` 给出的 Granularity/Max Size 约束。成功写入后 DAQ Pointer 在同一个 ODT 内自动移动到下一个 Entry；写过最后一个 Entry 后 Pointer 值未定义，因此跨 ODT/DAQ List 配置时 Master 必须重新 `SET_DAQ_PTR`。

### 7.5.4.3 `SET_DAQ_LIST_MODE`——设置 DAQ List 工作模式

**类别：** DAQ/STIM Basic，Mandatory。命令码 `0xE0`。

关键参数：

```text
Mode
DAQ_LIST_NUMBER
Event Channel Number
Prescaler
DAQ List Priority
```

Mode 标志包括：

| Flag | 0 | 1 |
|---|---|---|
| `DIRECTION` | DAQ：Slave→Master | STIM：Master→Slave |
| `ALTERNATING` | 普通模式 | Alternating Display Mode |
| `DTO_CTR` | 不使用 DTO Counter | 使用 DTO Counter |
| `TIMESTAMP` | 不带时间戳 | 带时间戳 |
| `PID_OFF` | DTO 带 Identification Field | DTO 不带 Identification Field |

`ALTERNATING` 只能用于 DAQ Direction，并且不能和 `TIMESTAMP` 同时置位。若 `GET_DAQ_RESOLUTION_INFO` 表示 `TIMESTAMP_FIXED`，Master 不允许关闭 Timestamp，否则 Slave 返回 `ERR_CMD_SYNTAX`。

`PID_OFF` 只有 Identification Field Type 为 **Absolute ODT Number** 时才允许。关闭 PID 后，DTO 的唯一标识必须由 Transport Layer 保证。例如 XCP on CAN 可以为不同 DAQ List 使用不同 CAN ID，并让每个 List 仅有一个 ODT，这样完整 8 Byte CAN Payload 都可以用于 Signal Data。

`Event Channel Number` 决定该 DAQ List 的采样/发送触发源。`Prescaler=1` 表示不降频；大于 1 表示降低 DAQ 发送频率，Prescaler 仅适用于 DAQ Direction。

Priority 取值越大优先级越高，`0xFF` 最高；0 表示允许 Slave 缓冲并在后台处理。若请求的优先级能力不支持，返回 `ERR_OUT_OF_RANGE`。

### 7.5.4.4 `START_STOP_DAQ_LIST`——启动/停止/选择一个 DAQ List

**类别：** DAQ/STIM Basic，Mandatory。命令码 `0xDE`。

Mode：

```text
0x00 = Stop
0x01 = Start
0x02 = Select
```

`Select` 并不立即开始传输，而是把 DAQ List 标记为 SELECTED，用于之后的 `START_STOP_SYNCH` 同步启动/停止，或用于 `SET_REQUEST` 保存 DAQ 配置到 NVM。

一旦至少一个 DAQ List 运行，`GET_STATUS.DAQ_RUNNING` 应为 1。

成功响应还返回 `FIRST_PID`。若 Identification Field Type 是 Absolute ODT Number：

```text
Absolute_ODT_Number = FIRST_PID(DAQ list) + Relative_ODT_Number
```

对于“Relative ODT + Absolute DAQ List Number”的 Identification 类型，`FIRST_PID` 可忽略。

### 7.5.4.5 `START_STOP_SYNCH`——同步启动/停止多个 DAQ List

**类别：** DAQ/STIM Basic，Mandatory。命令码 `0xDD`。

Mode：

```text
0x00 = stop all
0x01 = start selected
0x02 = stop selected
```

对之前通过 `START_STOP_DAQ_LIST(Select)` 标记的多个 List 执行同步动作。成功后 Slave 清除这些 List 的 SELECTED 标志。

### 7.5.4.6 `WRITE_DAQ_MULTIPLE`——一次写多个 ODT Entry

**类别：** DAQ/STIM Basic，Optional。命令码 `0xC7`。

一次 CTO 配置多个连续 Entry，以降低配置开销。每个 Entry 仍含 Bit Offset、Size、Address 和 Address Extension，并带用于对齐的 Dummy Byte。

限制：

- 所有 Entry 必须位于**同一个 ODT**；
- 不允许跨越 ODT 边界；
- 错误处理与 `WRITE_DAQ` 相同，但若失败无法知道具体是哪个 Entry 引发，因此整个该次配置应视为无效；
- 使用此命令要求 `MAX_CTO >= 10`。

### 7.5.4.7 `READ_DAQ`——读取一个 ODT Entry 配置

**类别：** DAQ/STIM Basic，Optional。命令码 `0xDB`。

读取当前 DAQ Pointer 指向 Entry 的：`BIT_OFFSET / Size / Address Extension / Address`，然后在同一 ODT 内自动递增 Pointer。与 `WRITE_DAQ` 不同，`READ_DAQ` 可以读取 PREDEFINED 和 configurable DAQ List。

### 7.5.4.8 `GET_DAQ_CLOCK`——读取 Slave DAQ Clock

**类别：** DAQ/STIM Basic，Optional。命令码 `0xDC`。

用于把 Slave 的自由运行 DAQ Clock 与 Master Clock 做时间关联。若不支持 Timestamped DAQ，可不实现。

规范定义 Legacy Format 与 Extended Format。连接后默认使用 Legacy Format；Master 通过 Time Correlation Property 命令启用高级时间相关后可进入 Extended Format，但 `MAX_CTO=8` 时仍使用 Legacy Format。

Legacy 响应核心是当前 DAQ Timestamp。Master 可以结合“发送 GET_DAQ_CLOCK 的本地时刻”和 Slave 返回的 Timestamp 估计时钟 Offset。在 CAN 场景中，Master 通常可以较准确知道 Command Frame 的发送时刻，因此可以进行基本时间对齐。

如果 DAQ Timestamp 关联到一个当前无法随机读取的 ECU Clock，Slave 不能伪造 Timestamp，应返回 `ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE`。

Extended Format 至少包含 XCP Slave Clock Timestamp，并可按能力附加：

```text
Grandmaster-synchronized clock timestamp
ECU clock timestamp
SYNC_STATE
```

这用于高级多时钟相关和全局时间同步。

### 7.5.4.9 `GET_DAQ_PROCESSOR_INFO`——查询 DAQ Processor 总体能力

**类别：** DAQ/STIM Basic，Optional。命令码 `0xDA`。

这是 Master 自动配置 DAQ 时的首要能力查询命令。返回：

```text
DAQ_PROPERTIES
MAX_DAQ
MAX_EVENT_CHANNEL
MIN_DAQ
DAQ_KEY_BYTE
```

`DAQ_PROPERTIES` 重要标志：

| Flag | 含义 |
|---|---|
| `DAQ_CONFIG_TYPE` | 0=Static，1=Dynamic |
| `PRESCALER_SUPPORTED` | 支持 DAQ 降频 |
| `RESUME_SUPPORTED` | 支持 RESUME Mode |
| `BIT_STIM_SUPPORTED` | 支持 Bitwise STIM |
| `TIMESTAMP_SUPPORTED` | 支持 Timestamp |
| `PID_OFF_SUPPORTED` | 可关闭 DTO Identification Field |
| `OVERLOAD_MSB/EVENT` | DAQ Overload 的通知方式 |

Overload 可以不通知、在下一个 PID 的 MSB 中表示，或者通过 `EV_DAQ_OVERLOAD` Event Packet 通知；MSB 方式会压缩可用 ODT Number 范围。

`MIN_DAQ` 是 PREDEFINED DAQ List 数。对于 Dynamic DAQ：

```text
MAX_DAQ = MIN_DAQ + DAQ_COUNT
```

可配置 List 的编号范围从 `MIN_DAQ` 开始。

`DAQ_KEY_BYTE` 进一步描述：

- ODT Optimization Method；
- Address Extension 必须在 Entry/ODT/DAQ 哪个范围内一致；
- DTO Identification Field Type。

Optimization Type 包括 Default、ODT_TYPE_16/32/64、Alignment 和 Max Entry Size 优化。Address Extension 可以允许每 Entry 不同，或要求同 ODT 相同、甚至同 DAQ List 全部相同。

Identification Field Type 有四种：

```text
Absolute ODT Number
Relative ODT + Absolute DAQ List Number (BYTE)
Relative ODT + Absolute DAQ List Number (WORD)
Relative ODT + Absolute DAQ List Number (WORD aligned)
```

Master 解析 DAQ DTO 和生成 STIM DTO 时必须使用与 Slave 相同的 Identification 形式。

### 7.5.4.10 `GET_DAQ_RESOLUTION_INFO`——查询 ODT Entry 与 Timestamp 分辨率

**类别：** DAQ/STIM Basic，Optional。命令码 `0xD9`。

返回 DAQ/STIM 两个方向各自的：

```text
GRANULARITY_ODT_ENTRY_SIZE_x
MAX_ODT_ENTRY_SIZE_x
```

以及：

```text
TIMESTAMP_MODE
TIMESTAMP_TICKS
```

ODT Entry 地址与长度必须满足 Entry Granularity 的对齐要求，且 Size 不得超过 `MAX_ODT_ENTRY_SIZE_x`。常见 Granularity 为 1、2、4、8 Byte。

若支持 Timestamp，DAQ Clock 是一个自由运行、不会主动 Reset/Modify 的 Counter，溢出时 Wrap Around。Timestamp Size 可以是 0、1、2 或 4 Byte；若 DTO 中 Timestamp 比底层 DAQ Clock 宽度短，则 DTO 携带的是 Clock 的低有效字节，高位被截断。

时间单位由 `TIMESTAMP_MODE` 高 nibble 表示，覆盖 1ns、10ns、100ns、1μs、10μs、100μs、1ms、10ms、100ms、1s，以及 ps 级单位；`TIMESTAMP_TICKS` 表示每单位有多少 Tick。最终 Tick 周期由“Unit × Ticks”共同决定。

`TIMESTAMP_FIXED=1` 表示 Timestamp 强制存在，Master 不能通过 `SET_DAQ_LIST_MODE` 关闭。

### 7.5.4.11 `GET_DAQ_LIST_MODE`——查询 DAQ List 当前状态

**类别：** DAQ/STIM Basic，Optional。命令码 `0xDF`。

响应包含 Current Mode、Current Event Channel、Prescaler 与 Priority。Current Mode 主要 Flag：

```text
SELECTED
DIRECTION
DTO_CTR
TIMESTAMP
PID_OFF
RUNNING
RESUME
```

其中 `SELECTED` 表示上一次 `START_STOP_DAQ_LIST(Select)` 已选择该 List；下一条若是 `START_STOP_SYNCH`，该 List 将参与同步启动/停止；若下一条是 `SET_REQUEST`，则它参与持久化 DAQ 配置。

`RUNNING` 既可能由 Master 主动启动，也可能由 Slave 在 RESUME Mode 上电后自动启动；`RESUME` 表示它属于持久化的 RESUME 配置。

### 7.5.4.12 `GET_DAQ_EVENT_INFO`——查询 Event Channel

**类别：** DAQ/STIM Basic，Optional。命令码 `0xD7`。

返回某 Event Channel 的：

```text
DAQ_EVENT_PROPERTIES
MAX_DAQ_LIST
EVENT_CHANNEL_NAME_LENGTH
EVENT_CHANNEL_TIME_CYCLE
EVENT_CHANNEL_TIME_UNIT
EVENT_CHANNEL_PRIORITY
```

Event 可以只支持 DAQ、只支持 STIM，或同时允许两类 List。Consistency 可定义在：

```text
ODT level（默认）
DAQ List level
Event Channel level
```

Consistency 范围越大，意味着同一次 Event Trigger 中更多变量必须属于一致的采样快照。

`MAX_DAQ_LIST=0` 表示 Event 存在但当前不可分配；`0xFF` 表示没有数量限制。

命令还会把 MTA 自动设置到 Event Channel Name，Master 可随后通过 `UPLOAD` 读取 ASCII 名称。名称不带 NUL 结尾。

周期表示为 `TIME_CYCLE × TIME_UNIT`。Event Time Unit 的编码位于参数**低 nibble**，这一点与 Timestamp Unit 位于高 nibble 不同。Priority 为 Slave 固有只读属性，`0xFF` 最高。

### 7.5.4.13 `DTO_CTR_PROPERTIES`——DTO Counter 属性

**类别：** DAQ/STIM Basic，Optional。命令码 `0xC5`。

用于查询或修改 Event Channel 对 DTO Counter 的处理方式，包括：

- STIM 收包时是否检查 Counter；
- DAQ 发包时是插入 Event Counter，还是复制相关 Event 保存的 STIM Counter；
- Related Event Channel Number。

`MODIFIER` 决定哪些属性真正被修改，未置位的属性保持原值。响应 `PROPERTIES` 指出这些能力是否存在以及是否 Fixed，包括 Related Event、DAQ/STIM Mode、STIM CTR Copy 与 Event Cycle Counter 等。

此机制主要服务于对 Bypassing/DAQ-STIM 一致性要求较高的系统，可检测丢失、错周期或使用错误 Cycle 的 DTO。

### 7.5.4.14 `CLEAR_DAQ_LIST`——清除一个 Static/Configurable DAQ List

**类别：** DAQ/STIM Static，Mandatory。命令码 `0xE3`。

清除指定 DAQ List。对于 configurable List，所有 ODT Entry 被重置为：

```text
address = 0
extension = 0
size = 0
bit_offset = 0xFF（若适用）
```

无论 PREDEFINED 还是 configurable List，正在运行的数据传输都会停止，DAQ List 状态被复位。

### 7.5.4.15 `GET_DAQ_LIST_INFO`——查询 Static DAQ List 信息

**类别：** DAQ/STIM Static，Optional。命令码 `0xD8`。

返回：

```text
DAQ_LIST_PROPERTIES
MAX_ODT
MAX_ODT_ENTRIES
FIXED_EVENT
```

Properties 表明：

- `PREDEFINED`：DAQ 配置是否固定、不可更改；
- `EVENT_FIXED`：Event Channel 是否固定；
- `DAQ/STIM`：允许哪个 Direction。

在 Static DAQ 中，`MAX_ODT` 与 `MAX_ODT_ENTRIES` 给出固定结构；`FIXED_EVENT` 给出不能改变时所绑定的 Event Channel。

### 7.5.4.16 `FREE_DAQ`——释放 Dynamic DAQ 配置

**类别：** DAQ/STIM Dynamic。命令码 `0xD6`。

删除所有动态分配的 DAQ Lists、ODTs 和 ODT Entries。任何新的 Dynamic DAQ Allocation Sequence 都必须以 `FREE_DAQ` 开始。

### 7.5.4.17 `ALLOC_DAQ`——分配 DAQ Lists

**类别：** DAQ/STIM Dynamic。命令码 `0xD5`。

分配 `DAQ_COUNT` 个 configurable DAQ Lists。内存不足返回 `ERR_MEMORY_OVERFLOW`。

该命令属于严格 Allocation State Machine。若在不允许的前序命令后调用，例如 `ALLOC_ODT`/`ALLOC_ODT_ENTRY` 后未先重新 `FREE_DAQ` 就再次 `ALLOC_DAQ`，返回 `ERR_SEQUENCE`。

### 7.5.4.18 `ALLOC_ODT`——给 DAQ List 分配 ODT

**类别：** DAQ/STIM Dynamic。命令码 `0xD4`。

为指定 configurable DAQ List 分配 `ODT_COUNT` 个 ODT。仅允许 `DAQ_LIST_NUMBER` 位于动态可配置区间。对象不存在返回 `ERR_OUT_OF_RANGE`，内存不足返回 `ERR_MEMORY_OVERFLOW`，违反分配顺序返回 `ERR_SEQUENCE`。

### 7.5.4.19 `ALLOC_ODT_ENTRY`——给 ODT 分配 Entry

**类别：** DAQ/STIM Dynamic。命令码 `0xD3`。

在指定 DAQ List 的指定 ODT 中分配 `ODT_ENTRIES_COUNT` 个 Entry。同样只允许 configurable DAQ List，并遵循严格顺序。

Dynamic DAQ 的正确分配顺序必须是：

```text
FREE_DAQ
   ↓
ALLOC_DAQ
   ↓
ALLOC_ODT        （为所有 DAQ List 分配 ODT）
   ↓
ALLOC_ODT_ENTRY  （为所有 ODT 分配 Entry）
   ↓
SET_DAQ_PTR + WRITE_DAQ
   ↓
SET_DAQ_LIST_MODE
   ↓
START_STOP_DAQ_LIST(Select)
   ↓
START_STOP_SYNCH(Start Selected)
```

任何跨级、逆序或在中途新增更高层对象的行为都可能触发 `ERR_SEQUENCE`。要增加新的 DAQ List，通常需要 `FREE_DAQ` 后重新配置整套 Dynamic DAQ。

## 7.5.5 Non-volatile Memory Programming

这一组命令用于 ECU 开发阶段的 Flash/NVM Programming。Programming Session 有独立的能力和通信参数，不能把 Calibration Download 与 Flash Programming 混为一谈。

### 7.5.5.1 `PROGRAM_START`

**类别：** Programming，Mandatory。命令码 `0xD2`。

表示一次 NVM Programming Sequence 正式开始。在 `PROGRAM_START` 成功前，不允许执行 `PROGRAM_CLEAR`、`PROGRAM`、`PROGRAM_MAX`、`PROGRAM_NEXT`。

Programming 可能有项目特定前置条件，例如 ECU 必须处于安全物理状态、先下载 Flash Driver 到 RAM、禁止 DAQ 正在运行等。进入 Programming 后至少必须继续允许：

```text
SET_MTA
PROGRAM_CLEAR
PROGRAM
PROGRAM_MAX 或 PROGRAM_NEXT
```

`UPLOAD`、`BUILD_CHECKSUM` 可选，用于验证。

如果刷写前需要先下载辅助代码，应在 `PROGRAM_START` 之前完成下载，并让 MTA 指向该程序入口。

成功响应可重新定义 Programming Session 的通信能力：

```text
COMM_MODE_PGM
MAX_CTO_PGM
MAX_BS_PGM
MIN_ST_PGM
QUEUE_SIZE_PGM
```

因此进入 Programming 后，Master 必须切换到这些 PGM 专用参数，不能继续假设普通 Session 的 `MAX_CTO/MAX_BS/MIN_ST` 仍有效。

### 7.5.5.2 `PROGRAM_CLEAR`

**类别：** Programming，Mandatory。命令码 `0xD1`。

擦除待重新编程的 NVM 区域，支持两种模式。

**Absolute Access Mode (`Mode=0`)：** MTA 指向一个 Sector 的起始地址，`Clear Range` 表示要清除的范围。Sector 信息由 A2L 描述。如果需要依次擦多个 Sector，Master 必须遵守每个 Sector 的 `Clear Sequence Number`。

**Functional Access Mode (`Mode=1`)：** MTA 对擦除无影响，`Clear Range` 被解释为功能 Bit Mask，例如：

```text
0x00000001  清除所有 Calibration Data Area
0x00000002  清除所有 Code Area（不含 Boot Area）
0x00000004  清除 NVRAM Area
0x00000100..0xFFFFFF00  项目自定义区域
```

Functional Mode 的价值在于 Master 不必了解实际 Flash 物理布局，ECU 自己把“标定区/代码区”等功能概念映射到真实 Sector。

### 7.5.5.3 `PROGRAM`

**类别：** Programming，Mandatory。命令码 `0xD0`。

把数据写入 NVM。Element 宽度由 AG 决定，AG=DWORD 时需要对齐 Byte。`n=0` 表示当前 Memory Segment 数据结束；整个 Programming Sequence 的结束由 `PROGRAM_RESET` 表示。

普通模式一次写入一个 CTO；支持 Block Mode 时，后续数据使用 `PROGRAM_NEXT`。块大小与间隔使用 `PROGRAM_START` 返回的 `MAX_BS_PGM/MIN_ST_PGM`。

**Absolute Access Mode：** 从 MTA 指向的真实地址开始写，写完 MTA 自动递增。多 Sector Programming 必须遵守 A2L 中的 `Programming Sequence Number`。

**Functional Access Mode：** ECU 自己知道新 Flash 内容应写到哪里，Master 连续发送一个逻辑数据流。此时 MTA 不再表示物理地址，而用作 **Block Sequence Counter**。Counter 在 `PROGRAM_FORMAT` 后从 1 开始，每个数据传输请求递增，到最大值后回绕为 0，用于改善编程序列出错后的诊断与恢复。

### 7.5.5.4 `PROGRAM_RESET`

**类别：** Programming，Mandatory/结束动作。命令码 `0xCF`。

表示 Programming Sequence 结束。该命令可以有响应，也可以没有响应；无论哪种情况 Slave 都进入 `DISCONNECTED`。典型实现随后执行 ECU Hardware Reset。也可以把该命令用于其他需要强制 Slave Reset 的场景。

### 7.5.5.5 `GET_PGM_PROCESSOR_INFO`

**类别：** Programming，Optional。命令码 `0xCE`。

返回 Programming 总体能力：`PGM_PROPERTIES + MAX_SECTOR`。属性包括：

- Absolute Access Mode 是否支持；
- Functional Access Mode 是否支持；
- Compression 是否支持/是否强制；
- Encryption 是否支持/是否强制；
- Non-sequential Programming 是否支持/是否强制。

Compression/Encryption/Non-sequential 的结果是对全部可编程 Segment/Sector 能力的汇总。`MAX_SECTOR` 为 Sector 总数。

### 7.5.5.6 `GET_SECTOR_INFO`

**类别：** Programming，Optional。命令码 `0xCD`。

主要用于 Absolute Access Mode。Mode：

```text
0 = Sector Start Address
1 = Sector Length [Byte]
2 = Sector Name Length，并把 MTA 指向 Sector Name
```

Mode 0/1 的响应同时返回：

```text
Clear Sequence Number
Program Sequence Number
Programming Method
SECTOR_INFO
```

Clear/Program Sequence Number 定义多个 Sector 擦除和写入的先后顺序，并要求各 Sequence Number 唯一。Master 可以跳过不需要 Programming 的 Sector，但对于实际执行的 Sector 必须保持规定顺序。

Mode 2 会自动设置 MTA，之后用 `UPLOAD` 获取 ASCII Sector Name，名称不带 `\0`。

### 7.5.5.7 `PROGRAM_PREPARE`

**类别：** Programming，Optional。命令码 `0xCC`。

用于通知 Slave：接下来将先把一段辅助 Programming Code 下载到易失性 RAM。MTA 指向目标 RAM 起点，`Codesize [AG]` 给出长度；真正的数据传输仍使用标准 `SET_MTA/DOWNLOAD`。

Slave 必须确认目标 RAM 可用且当前状态允许代码下载，否则返回 `ERR_GENERIC`。

### 7.5.5.8 `PROGRAM_FORMAT`

**类别：** Programming，Optional。命令码 `0xCB`。

定义紧随其后的、不间断 Programming Data Stream 的格式：

```text
Compression Method
Encryption Method
Programming Method
Access Method
```

默认全部为 0，即：不压缩、不加密、Sequential Programming、Absolute Access。用户自定义格式通常用 `0x80..0xFF`。

`Access Method=0x01` 表示 Functional Access，此时 MTA 用作 Block Sequence Number。

需要强调：**Master 本身并不负责执行这些项目特定压缩/加密重格式化算法**。规范描述的用法是 Master 从 A2L/项目 Programming Flow Control 取得方法标识，再传给 Slave。具体文件/数据如何形成通常由项目工具链决定。

若 Slave 期望 Modified Data，而 Master 未发送 `PROGRAM_FORMAT`，返回 `ERR_SEQUENCE`。`SET_MTA` 等其他命令会终止当前“不间断格式序列”。

### 7.5.5.9 `PROGRAM_NEXT`

**类别：** Programming，Optional。命令码 `0xCA`。

用于 `PROGRAM` Block Mode 的后续 Packet。剩余 Element Count 必须与 Slave 期望值一致，否则返回 `ERR_SEQUENCE`，并在 Negative Response 中给出 Expected Number of Data Elements。

### 7.5.5.10 `PROGRAM_MAX`

**类别：** Programming，Optional。命令码 `0xC9`。

以固定最大长度一次 Programming；数据从当前 MTA 开始，完成后 MTA 自动递增。该命令不支持 Block Transfer，也不能出现在 Block Sequence 内。

### 7.5.5.11 `PROGRAM_VERIFY`

**类别：** Programming，Optional。命令码 `0xC8`。

用于项目特定 Flash Verification：

- `Verification Mode=0`：请求 Slave 启动内部验证 Routine；
- `Verification Mode=1`：Master 向 Slave 提供 Verification Value。

Verification Type 标准预留了：Calibration Area、Code Area、Complete Flash，以及项目自定义范围。具体 Verification Algorithm/Value 的意义由项目 Programming Flow Control 定义，XCP Master 只需要传递参数，并不需要理解算法内部细节。

## 7.5.6 Time Correlation

### 7.5.6.1 `TIME_CORRELATION_PROPERTIES`

**类别：** Time Correlation，Optional。命令码 `0xC6`。

这是 XCP 1.3 高级时间相关机制的中心配置命令。它既用于查询 Slave 中可观察 Clock 的状态/特征，也用于开启 Extended Time Correlation、配置 Time Sync Bridge 和 Cluster ID。

为了向后兼容，Slave 上电后或发生 Fatal Error 后必须先回到 Legacy Mode。在 Legacy Mode：

- 不响应 `GET_DAQ_CLOCK_MULTICAST`；
- `GET_DAQ_CLOCK` Positive Response 使用 Legacy Format；
- `EV_TIME_SYNC` 使用 Legacy Format。

`SET_PROPERTIES` 可以配置三个方面：

**Response Format**：0 不改变；1/2 开启高级时间相关。Mode 1 仅允许针对规定的 Trigger Initiator 发送 `EV_TIME_SYNC`；Mode 2 允许所有 Trigger Condition。

**Time Sync Bridge**：允许同一物理 Device 内处于不同 Transport Layer 的 XCP Slave 共同捕获 Clock，从而把 CAN 与 Ethernet 等不同网络上的时间轴桥接起来。收到一侧的 Multicast Clock 请求后，另一侧 Slave 可通过设备内部机制同步采样并发送 `EV_TIME_SYNC`，Master 因而能建立跨 Transport 的 Timestamp Correlation。

**Cluster ID**：当同一物理网络存在多个 XCP Master 时，用 Cluster Identifier 区分 `GET_DAQ_CLOCK_MULTICAST` 属于哪个 Master。Slave 只应对与自己 `CLUSTER_AFFILIATION` 相同的 Cluster 请求应答。默认 Cluster ID 为 0。规范建议人工分配；也允许工具随机生成，但应先监听网络避免冲突。

`GET_PROPERTIES_REQUEST.GET_CLK_INFO=1` 时，Slave 把 MTA 指向 Clock Information Data Block，Master 后续通过 Memory Transfer Command 上传详细 Clock Metadata。这通常在第一次建立高级时间相关树，或检测到 Clock 同步/同频状态变化时使用。

Positive Response 主要包括：

```text
SLAVE_CONFIG
OBSERVABLE_CLOCKS
SYNC_STATE
CLOCK_INFO
CLUSTER_ID
```

`SLAVE_CONFIG` 描述：当前 Response Format、DAQ Timestamp 是关联 XCP Slave Clock 还是 ECU Clock、Time Sync Bridge 是否存在/是否启用。

`OBSERVABLE_CLOCKS` 用于描述三类 Clock：

- XCP Slave Clock：自由运行，或可与 Grandmaster syntonize/synchronize；
- Dedicated Grandmaster-related Clock：可随机读或只能通过 `EV_TIME_SYNC` 给出同步采样值；
- ECU Clock：可随机读、不可随机读但可通过同步 Event 观察，或仅从 ECU Trace 中获得 Timestamp。

若 `ECU_CLK > 0`，DAQ Timestamp 必须被标记为关联 ECU Clock。

一个重要约束是：**DAQ/STIM 正在运行时，XCP Slave Clock 不允许执行会造成 Timestamp 跳变的 Synchronization；只允许 Syntonization（调整频率而不跳时钟）。** 真正的 Clock Synchronization 只能在没有 DAQ/STIM 的状态进行。

`SYNC_STATE` 分别描述 Slave Clock、Dedicated Grandmaster Clock 与 ECU Clock 的同步/同频状态。例如 Slave Clock 可以处于 synchronizing、synchronized、syntonizing、syntonized 或不支持同步状态。

`CLOCK_INFO` 表示随后可上传的数据块中包含哪些信息：

```text
SLV_CLK_INFO
GRANDM_CLK_INFO
CLK_RELATION
ECU_CLK_INFO
ECU_GRANDM_CLK_INFO
```

Clock 信息使用 EUI-64 UUID 标识，并包含 Timestamp Ticks、Unit、Clock Quality/Stratum、Native Timestamp Size、Wrap-around 前最大值等。若多个 XCP Slave 观察的是同一个 ECU Clock，规范强烈建议它们报告相同 UUID，从而让 Master 识别为同一个时钟源。

`CLK_RELATION` 给出同时采样的 Slave Clock 与 Grandmaster Clock Timestamp Tuple，Master 用它计算两个 Clock Domain 的 Offset。对于 UDP 等可能丢 Event 的网络，如果 Master 发现 Slave 已经 syntonized 却没有收到所需 Tuple，可以再次以 `GET_CLK_INFO=1` 请求 Slave 提供该关系数据块。

Grandmaster Epoch 的标准值包括 TAI、UTC 或 Unknown；Native Timestamp Size 只允许 4 Byte 或 8 Byte，并始终按 Unsigned 解释。若 DAQ DTO 中 Timestamp Size 小于 Native Clock Size，则 DTO 中对应 Native Timestamp 的低有效字节。

## 7.6 Communication Error Handling

### 7.6.1 基本定义

Master 发送 CMD 后，在规定时间内收到 Positive Response `RES` 表示无错误；规定时间内没有任何 Response 为 **Timeout Error**；收到 Negative Response `ERR` 为 **Error Code Error**。

恢复错误通常分为两阶段：

```text
Pre-Action → Action
```

Pre-Action 的目标是先把 Slave 恢复到一个定义明确的状态。协议列举的 Pre-Action 包括：等待 `t7`、`SYNCH`、`GET_SEED/UNLOCK`、重新 `SET_MTA`、重新 `SET_DAQ_PTR`、`START_STOP_x`、重新初始化 DAQ 等。

Action 则是真正的恢复行为，例如显示错误、换 Syntax/Parameter 重试、改从 A2L 读取信息、使用 Alternative Command、重试 2 次/无限重试、Restart Session 或 Terminate Session。

Error/Event 的 Severity 分为：

| Level | 含义 |
|---|---|
| S0 | Information |
| S1 | Warning / Request |
| S2 | Resolvable Error |
| S3 | Fatal Error |

Severity 用于帮助 Master 判断是否发生状态机迁移以及应采取何种恢复策略。

### 7.6.2 Timeout Handling

XCP 定义 `t1..t7` 七类 Timeout，具体值由 ASAM MCD-2 MC/A2L Description File 提供；不同 Command 使用不同 `tx`。

在 Standard Communication Model 中，Master 发 Command 时启动 Timer；收到响应则 Reset。Timeout 后通常：

```text
SYNCH
  ↓
ERR_CMD_SYNCH
  ↓
重新发送原 Command
```

该“Pre-Action + Action”序列原则上尝试两次；仍失败后由 Master 决定进一步动作。对于依赖隐含指针状态的 Command，重试前还必须重新建立状态，例如 `UPLOAD` 前重新 `SET_MTA`，`WRITE_DAQ` 前重新 `SET_DAQ_PTR`。

Block Transfer 中 Timer 的起止点放在整个 Block 边界：Master Block Mode 在最后一个 Request Frame 后开始等待，重试必须重发构成该 Command 的**整个 Block**；Slave Block Mode 则收到 Response Block 最后一帧后 Reset Timer。

Interleaved Mode 使用相同总体恢复原则，但 Master 要考虑多条 Outstanding Command/Response 的队列关系。

`EV_CMD_PENDING` 是 Timeout Handling 的重要机制：Slave 表示“Command 已正确接收、参数有效，但当前还不能给最终 Response”。Master 收到它后**不能重发原 Command**，而应重新启动 Timeout Timer，继续等待最终 `RES/ERR`。

### 7.6.3 Error Code Handling

主要 Error Code：

| Error | Code | Severity | 中文含义 |
|---|---:|---|---|
| `ERR_CMD_SYNCH` | `0x00` | S0 | Command Processor 同步确认 |
| `ERR_CMD_BUSY` | `0x10` | S2 | Command 未执行，Slave Busy |
| `ERR_DAQ_ACTIVE` | `0x11` | S2 | 因 DAQ 正在运行而拒绝 |
| `ERR_PGM_ACTIVE` | `0x12` | S2 | 因 Programming 正在运行而拒绝 |
| `ERR_CMD_UNKNOWN` | `0x20` | S2 | 未知/未实现 Optional Command |
| `ERR_CMD_SYNTAX` | `0x21` | S2 | Command Syntax 无效 |
| `ERR_OUT_OF_RANGE` | `0x22` | S2 | Syntax 正确但参数越界 |
| `ERR_WRITE_PROTECTED` | `0x23` | S2 | 内存写保护 |
| `ERR_ACCESS_DENIED` | `0x24` | S2 | 地址不可访问 |
| `ERR_ACCESS_LOCKED` | `0x25` | S2 | 需要 Seed&Key 解锁 |
| `ERR_PAGE_NOT_VALID` | `0x26` | S2 | Page 无效 |
| `ERR_MODE_NOT_VALID` | `0x27` | S2 | Mode 不支持 |
| `ERR_SEGMENT_NOT_VALID` | `0x28` | S2 | Segment 无效 |
| `ERR_SEQUENCE` | `0x29` | S2 | 命令序列错误 |
| `ERR_DAQ_CONFIG` | `0x2A` | S2 | DAQ Configuration 无效 |
| `ERR_MEMORY_OVERFLOW` | `0x30` | S2 | 内存/配置资源不足 |
| `ERR_GENERIC` | `0x31` | S2 | 通用错误 |
| `ERR_VERIFY` | `0x32` | S3 | Slave 内部 Program Verify 失败 |
| `ERR_RESOURCE_TEMPORARY_NOT_ACCESSIBLE` | `0x33` | S2 | Resource 暂时不可访问 |
| `ERR_SUBCMD_UNKNOWN` | `0x34` | S2 | Sub-command 未知/未实现 |

规范后续的大型 Error Handling Matrix 给出每条 Command 对各种 Error 的推荐 Pre-Action/Action。实现 Master 时可以归纳成几类统一策略：`ERR_CMD_BUSY/PGM_ACTIVE` 通常等待 `t7` 后继续；`ERR_ACCESS_LOCKED` 触发 Seed&Key；`ERR_SEQUENCE` 重新建立 MTA/DAQ/Programming 序列；`ERR_OUT_OF_RANGE` 更换参数；`ERR_CMD_UNKNOWN` 对 Capability Query 可以退回 A2L，对可替代 Command 可以使用 Alternative；Timeout 多数先 `SYNCH` 再重复两次；Dynamic DAQ Allocation 出现 `ERR_MEMORY_OVERFLOW/ERR_SEQUENCE` 时应重新初始化 DAQ，而不是继续沿用部分配置。

## 7.7 Event Packet 描述

所有 Event Packet 的 PID 为 `0xFD`，Byte 1 是 Event Code。Event 中的 WORD/DWORD 同样遵循对齐和 Slave Byte Order。

### 7.7.1 `EV_RESUME_MODE` (`0x00`)

Slave 上电后直接进入 RESUME Mode 时发送。携带 `Session Configuration ID`；若支持 Timestamp，还携带当前 DAQ Clock Timestamp。Master 可用 ID 检查自动恢复的 DAQ Configuration 是否和自己预期的一致。

### 7.7.2 `EV_CLEAR_DAQ` (`0x01`)

通知 Master：非易失存储中的 DAQ Configuration 已清除完成。

### 7.7.3 `EV_STORE_DAQ` (`0x02`)

通知 Master：DAQ Configuration 已保存到 NVM。

### 7.7.4 `EV_STORE_CAL` (`0x03`)

通知 Master：Calibration Data 已保存到 NVM。

### 7.7.5 `EV_CMD_PENDING` (`0x05`)

要求 Master 重新启动当前 Command 的 Timeout Timer，而不是重发 Command。最终 Slave 仍必须给出 Positive/Negative Response。

### 7.7.6 `EV_DAQ_OVERLOAD` (`0x06`)

通知 DAQ Transfer 出现 Overload/Overrun。Master 应把该周期的数据完整性标记为可疑，并结合 DAQ List/Event Channel、总线负载和 ODT Packing 分析原因。

### 7.7.7 `EV_SESSION_TERMINATED` (`0x07`)

Slave 主动终止当前 XCP Session。收到后 Master 应把本地连接状态同步为 Disconnected，而不是继续发 Command。

### 7.7.8 `EV_TIME_SYNC` (`0x08`)

用于时间同步/相关。是否发送与 Timestamp 配置有关；`TIMESTAMP_FIXED=1` 时始终允许/要求相应时间同步机制，否则取决于 `SET_DAQ_LIST_MODE.TIMESTAMP`。

为兼容旧 Master，刚进入 CONNECTED 时使用 Legacy Format，直到 Master 通过 `TIME_CORRELATION_PROPERTIES` 开启 Extended Format。对于 `MAX_CTO=8`（例如传统 CAN），即使启用高级 Time Correlation，也继续使用精简格式。

Legacy Format 主要携带“DAQ Timestamp 所关联 Clock”的 32-bit Timestamp。若这个 Clock 无法被随机读取（例如 DAQ Timestamp 来自不可直接读取的 ECU Clock），Slave 不应发送虚假的 `EV_TIME_SYNC`。

Extended Format 通过 `TRIGGER_INFO` 标记触发来源：

```text
0  Hardware Trigger
1  XCP 外部时间同步事件（例如 PPS）
2  GET_DAQ_CLOCK_MULTICAST
3  通过 Time Sync Bridge 转发的 Multicast
4  Clock synchronization/syntonization 状态变化
5  Grandmaster Leap Second
6  ECU Reset Release
```

`TIME_OF_TS_SAMPLING` 说明 Timestamp 是在协议处理、High-priority Interrupt、物理发送还是物理接收时采样；对于 Multicast Clock Request，推荐在物理接收时立即采样，以降低不确定性。

`PAYLOAD_FMT` 指示 Event 中是否携带 XCP Slave Clock、Grandmaster Clock、ECU Clock，以及各自是 DWORD 还是 DLONG，同时可指示是否附带 Cluster ID/Counter。高级模式下 Slave 应尽量在 Trigger 后尽快发送 Event，避免 Clock Wrap-around 导致关联歧义。

### 7.7.9 `EV_STIM_TIMEOUT` (`0x09`)

当某次 Stimulation Cycle 无法成功执行时通知 Master。`Info Type` 表示问题定位到 Event Channel 还是 DAQ List；`Failure Type` 至少包括 Timeout 和 DTO Counter Check Failed。

### 7.7.10 `EV_SLEEP` (`0x0A`)

Slave 进入 Sleep Mode，但逻辑上仍保持 `CONNECTED`。此时 Slave 不处理 Command，也不会返回 `ERR_CMD_BUSY`/`EV_CMD_PENDING`。Master 收到该 Event 后必须停止发送 Command，并丢弃双方 Pending Command，等待 `EV_WAKE_UP`。

### 7.7.11 `EV_WAKE_UP` (`0x0B`)

Slave 离开 Sleep，恢复正常处理。

### 7.7.12 `EV_ECU_STATE_CHANGE` (`0x0C`)

通知 Master ECU State 已变化，并携带新的 `STATE_NUMBER`。

### 7.7.13 `EV_USER` (`0xFE`)

用户自定义 Event 的承载 Packet。

### 7.7.14 `EV_TRANSPORT` (`0xFF`)

Transport Layer Specific Event 的承载 Packet，具体内容由相应 Associated Standard 定义。

# 8 与 ASAM MCD-2 MC Description File 的接口

## 8.1 总览

XCP Protocol Layer 本身与 Transport Layer 无关，而 Slave 的 XCP Communication Stack 参数通过 A2L/AML 中的 `IF_DATA` 描述。典型文件关系可以理解为：

```text
main.a2l
  |
  +-- XCPplus_vX_Y_IF_DATA_example.aml
        |
        +-- XCP_vX_Y_definitions.aml
        |     +-- XCP_common_vX_Y.aml
        |     +-- XCP_on_CAN_vU_V.aml
        |     +-- XCP_on_UDP_IP_vU_V.aml
        |     +-- ...
        |
        +-- XCPplus_vX_Y.aml
```

`XCP_vX_Y_common.aml` 描述与 Transport 无关的 `Common_Parameters`；各个 Associated Standard 中的 `XCP_vX_Y_on_##.aml` 描述 CAN/Ethernet 等 Transport-specific Parameter。

一个 XCP `IF_DATA` 可以同时包含默认的 Common Parameter 和 Transport-specific Override。这里的 **override 是替换整个 Block，而不是把 Block 内 Element 合并**。Override 的粒度是诸如 `PROTOCOL_LAYER`、`DAQ` 这样的 Block 级别。

## 8.2 XCP AML 与 Communication Stack 组合

`XCP_definitions.aml` 必须组合一个 Protocol Layer Part 与 Slave 支持的一个或多个 Transport Layer Part。例如：

```text
/include XCP_common_v1_0.aml
/include XCP_on_UDP_IP_v1_0.aml
/include XCP_on_CAN_v1_1.aml
```

这表示同一个 Slave 以 XCP Protocol 1.0 运行，同时支持 UDP/IP Transport 1.0 与 CAN Transport 1.1。实际允许的版本组合应符合 Compatibility Matrix。

`IF_DATA XCP` 与 `IF_DATA XCPplus` 都可以定义默认 Common Parameter 和各 Transport 的 Specific Parameter，但二者有一个关键区别：

- `IF_DATA XCP`：同一种 Transport Layer 只能出现一个 Instance；
- `IF_DATA XCPplus`：允许同一种 Transport Layer 出现多个 Instance，例如 `private CAN` 与 `vehicle CAN`。

当 `XCPplus` 中存在多个相同 Transport 类型实例时，必须使用 `TRANSPORT_LAYER_INSTANCE` 区分。

Master 查某个 Transport Layer 的 Common Parameter 时，先检查该 `/begin XCP_on_##` 内是否存在 Override；若存在则使用它，否则回退到 `IF_DATA XCP/XCPplus` 的默认 Common Parameter。

如果一个 A2L 同时包含 `IF_DATA XCPplus` 和 `IF_DATA XCP`，Master **优先使用 XCPplus**；只有没有 XCPplus 时才查 XCP。

## 8.3 示例 A2L

规范随附的 Example 展示一个同时支持 UDP/IP 和两个 XCP on CAN Instance 的 Slave：UDP/IP 使用默认 Common Parameter；`private CAN` 覆盖 DAQ 和 PROTOCOL_LAYER；`vehicle CAN` 使用另一组 CAN-specific Parameter。

## 8.4 A2L 与 Slave 参数一致性

很多 XCP 参数既能写在 A2L `IF_DATA` 中，也能在连接后直接从 Slave 查询。**当同一参数同时存在两种来源时，Master 必须检查二者一致性。**

如果不一致，Master 应向用户报告，并让用户决定该 Parameter 最终使用 A2L 值还是 Slave 运行时返回值，而不是静默覆盖。这一点非常值得直接做进 CANape-like 工具的 Configuration Diagnostic。

# 9 外部 Seed&Key 函数接口

Seed→Key 算法不由 XCP 标准统一规定，而由 Slave/ECU Vendor 提供。A2L `PROTOCOL_LAYER` 中的 `SEED_AND_KEY_EXTERNAL_FUNCTION` 给出外部函数文件名；文件名包含扩展名但不包含 Path。

Windows 上通常是 DLL，UNIX/Linux 上通常是 Shared Object。一个外部文件可以提供 CAL/PAG、DAQ、STIM、PGM 中全部或部分 Resource 的解锁算法。

规范定义两个外部函数逻辑接口。

## 9.1 `XCP_GetAvailablePrivileges`

用于查询这个外部 Seed&Key 模块实际提供哪些 Resource 的解锁算法。返回值使用 XCP Resource Availability Mask 编码。

## 9.2 `XCP_ComputeKeyFromSeed`

逻辑原型参数：

```text
Requested Privilege
Seed Length
Seed Pointer
Key Length Pointer
Key Pointer
```

计算关系：

```text
Key = f(Seed, RequestedPrivilege)
```

每次只针对一个 Privilege。Key Buffer 建议预留 255 Byte，因为这是规范允许的最大 Key Length。

返回错误包括：Privilege 不可用、Seed Length 无效、Key Buffer 太小等。

特别注意跨平台 Byte Order：外部函数接收和返回的是**按 XCP Packet 实际传输顺序排列的 BYTE Array**。例如 Master 运行在 Intel 平台而 ECU 使用 Motorola Format，也不应由 Master 擅自重排 Seed/Key Byte。

# 10 外部 Checksum 函数接口

当 `BUILD_CHECKSUM` 返回/配置的类型为 `XCP_USER_DEFINED` 时，Master 应调用项目提供的外部 Checksum Algorithm。A2L 中 XCP Segment 的 `CHECKSUM` Block 通过 `EXTERNAL_FUNCTION` 指定函数文件名。

Windows 可通过 DLL、UNIX/Linux 可通过 `.so` 集成。加载机制由 Tool 自己设计，函数参数/API 则由配套标准约束。

# 11 外部 A2L 解压/解密接口

当 `GET_ID` 上传的 A2L Description Data 被压缩或加密时，Master 将数据交给 Slave Vendor 提供的外部函数解压/解密。

规范给出的概念接口：

```c
int XCP_DecompressA2L(
    unsigned int compressedLength,
    unsigned char* compressedData,
    unsigned int* decompressedLength,
    unsigned char** decompressedData);
```

典型返回值：

```text
0 成功
1 源数据损坏
2 解压/解密内存不足
3 内部错误
4 SmartCard 不可访问
```

外部函数自行分配输出 Buffer，Client 使用完成后必须调用释放接口：

```c
int XCP_ReleaseDecompressedData(unsigned char* decompressedData);
```

释放后不得继续访问该 Buffer。

# 12 示例

规范最后给出多组完整通信序列，目的是展示各条独立 Command 如何组成真实工作流。对于实现 XCP Master，这一章非常值得直接当成 Integration Test 的参考。

## 12.1 Configuration Example

给出不同 ODT Entry Size 与不同 Copy Routine Size 对 CPU Load 的计算示例，用于说明前文 ODT Optimization/性能参数如何影响 Slave 的数据复制开销。工程上应把这些参数理解为 Slave 的 Packing/Copy Constraint，而不是只追求“把 DTO 塞满”。

## 12.2 `GET_ID` 标识字符串示例

规范示例：

```text
Type 1: Test
Type 2: c:\database\test.a2l
Type 3: ftp://ttp.oem.com\data_repository\project_xcp\test.a2l
```

说明 `GET_ID` 可以返回短名称、本地/网络路径或 URL。

## 12.3 报文方向记号

```text
Master → Slave : CMD
Slave  → Master: RES
```

## 12.4 建立 Session

示例首先发送：

```text
CONNECT
→ FF 00
← FF 15 C0 08 08 00 10 10
```

示例响应的解释：

```text
RESOURCE = 0x15
  CAL/PAG + DAQ + PGM available

COMM_MODE_BASIC = 0xC0
  Intel Byte Order
  AG = BYTE
  Slave Block Mode supported

MAX_CTO = 8
MAX_DTO = 8
Protocol Version = 1.0
Transport Version = 1.0
```

随后：

```text
GET_COMM_MODE_INFO
GET_STATUS
```

示例 `GET_STATUS` 表示 Session 当前无 DAQ、无 RESUME，但 CAL/PAG、DAQ、PGM 都被 Seed&Key 保护。

### Seed&Key 解锁示例

规范分别对 CAL/PAG、DAQ、PGM 执行：

```text
GET_SEED(resource)
↓
外部算法计算 Key
↓
UNLOCK(key)
```

每次成功后 Resource Protection Mask 会相应清零，最后可达到 `0x00`，表示示例中的三类资源全部解锁。

### 获取 A2L 标识

示例：

```text
GET_ID(Type=1)
← Mode=0, Length=6
UPLOAD(6)
← 58 43 50 53 49 4D = "XCPSIM"
```

说明 `Mode=0` 时 `GET_ID` 只设置 MTA，实际字符串再用 `UPLOAD` 读取。

## 12.5 Calibration 示例

首先分别查询 ECU Access 与 XCP Master Access 当前激活的 Page。随后示例把 Page 0 同时设置为 ECU/XCP Access，并对一段 Calibration Memory 计算 Checksum。

读写参数示例：

```text
SET_MTA address=0x00000060
DOWNLOAD 00 00 80 3F
SHORT_UPLOAD address=0x00000060, size=4
← 00 00 80 3F
```

在 Intel Little-endian 的 IEEE-754 场景中，这组 Byte 可解释为 float `1.0`。这条序列非常适合作为你以后 C++ XCP Master 的最小“写后读回”测试。

Page Copy 示例：

```text
COPY_CAL_PAGE
source      = Segment 0 / Page 1
destination = Segment 2 / Page 3
```

## 12.6 Synchronous Data Transfer 示例

### 12.6.1 查询 DAQ 能力

示例先执行：

```text
GET_DAQ_PROCESSOR_INFO
GET_DAQ_RESOLUTION_INFO
GET_DAQ_EVENT_INFO
```

示例 Slave 为 Dynamic DAQ，支持 Timestamp；Event Channel 名称通过 `GET_DAQ_EVENT_INFO` 设置 MTA 后再 `UPLOAD` 得到 ASCII `"10 ms"`。

若是 Static DAQ，还应遍历 `GET_DAQ_LIST_INFO` 查询固定 DAQ List 的 ODT/Entry 结构。

### 12.6.2 准备 DAQ List

Static：对每个 configurable List 执行 `CLEAR_DAQ_LIST`。

Dynamic 示例：

```text
FREE_DAQ
ALLOC_DAQ(1)
ALLOC_ODT(DAQ0, 1)
ALLOC_ODT_ENTRY(DAQ0, ODT0, 2)
```

### 12.6.3 配置 ODT Entry

```text
SET_DAQ_PTR(DAQ0, ODT0, Entry0)
WRITE_DAQ(
    BIT_OFFSET = 0xFF,
    Size = 4,
    AddressExtension = 0,
    Address = 0x000C5508)
```

随后依次写剩余 Entry。Static/Dynamic 两种模式只是 Loop Upper Limit 的来源不同，真正写 Entry 的机制相同。

### 12.6.4 开始 DAQ

示例：

```text
SET_DAQ_LIST_MODE
  direction = DAQ
  timestamp = enabled
  Event = 0
  Prescaler = 1
  Priority = 0

START_STOP_DAQ_LIST(Select)
GET_DAQ_CLOCK
START_STOP_SYNCH(Start Selected)
```

这基本就是实现一个通用 XCP Measurement Engine 所需的标准启动序列。

### 12.6.5 停止 DAQ

先把要停止的 List `Select`，再：

```text
START_STOP_SYNCH(Stop Selected)
```

## 12.7 Reprogramming Slave 示例

标准流程：

```text
PROGRAM_START
  ↓
SET_MTA
PROGRAM_CLEAR
  ↓
SET_MTA
PROGRAM / PROGRAM_NEXT ...
  ↓
PROGRAM_RESET
```

示例 Programming Session 返回独立的 `MAX_CTO_PGM/MAX_BS_PGM/MIN_ST_PGM`；Master 必须按这些值发送 Programming Data。

## 12.8 关闭 Session

```text
DISCONNECT
→ FE
← FF
```

## 12.9 Time Correlation 示例

规范给出 Clock Scenario 1~5b 的多组 `EV_TIME_SYNC` Payload Example，包括：

- 只有一个 Free-running XCP Slave Clock；
- Slave Clock 与 Grandmaster Synchronize；
- Slave Clock 与 Grandmaster Syntonize；
- 同时存在 Slave Clock 与 Dedicated Grandmaster-related Clock；
- 同时存在 Slave Clock 与 ECU Clock；
- Multicast、PPS、ECU Reset Release 等不同 Trigger。

示例展示 `TRIGGER_INFO`、`PAYLOAD_FMT`、32/64-bit Timestamp、Cluster Identifier、Counter、`SYNC_STATE` 如何组合。对于普通 CANape-like 变量测量工具，第一版可以暂不实现高级 Time Correlation；当需要跨 ECU/跨 CAN-Ethernet 的高精度统一时间轴时再完整实现该部分。

# 13 符号与缩略语

| 缩写 | 含义 |
|---|---|
| A2L | ASAM MCD-2 MC Language File 扩展名 |
| AG | Address Granularity，地址粒度 |
| AML | ASAM 2 Meta Language |
| CAL | Calibration |
| CAN | Controller Area Network |
| CCP | CAN Calibration Protocol |
| CMD | Command |
| CTO | Command Transfer Object |
| DAQ | Data Acquisition / Data Acquisition Packet |
| DLL | Dynamically Linked Library |
| DTO | Data Transfer Object |
| ECU | Electronic Control Unit |
| ERR | Error Packet |
| EV | Event Packet |
| ID | Identifier |
| IF | Interface |
| IP | Internet Protocol |
| LSB | Least Significant Bit/Byte |
| MCD | Measurement, Calibration and Diagnostics |
| MSB | Most Significant Bit/Byte |
| MTA | Memory Transfer Address |
| ODT | Object Descriptor Table |
| PAG | Paging |
| PGM | Programming |
| PID | Packet Identifier |
| RAM | Random Access Memory |
| RES | Command Response Packet |
| ROM | Read-Only Memory |
| SCI | Serial Communication Interface |
| SERV | Service Request Packet |
| SiL | Software in the Loop |
| SPI | Serial Peripheral Interface |
| STIM | Data Stimulation Packet |
| TCP/IP | Transmission Control Protocol / Internet Protocol |
| TS | Timestamp |
| UDP | User Datagram Protocol |
| USB | Universal Serial Bus |
| XCP | Universal Measurement and Calibration Protocol |

# 14 参考文献

原规范引用的主要标准/资料包括：

1. ASAM MCD-2 MC / Measurement and Calibration Data Specification 1.7.x；
2. ISO 14229-1 Road vehicles - Diagnostic services；
3. ISO 15765 系列 CAN Diagnostics / DoCAN；
4. CRC Algorithm Reference；
5. ASAM MCD-1 XCP CAN Transport Layer 1.3.0；
6. ASAM MCD-1 XCP Ethernet Transport Layer 1.3.0；
7. ASAM MCD-1 XCP FlexRay Transport Layer 1.3.0；
8. ASAM MCD-1 XCP SxI Transport Layer 1.3.0；
9. ASAM MCD-1 XCP USB Transport Layer 1.3.0；
10. ASAM Common SeedKey and Checksum Calculation 1.0.0；
11. IEEE/IEC 61588 Precision Clock Synchronization Protocol（PTP 相关）。

原文后续 Figure Directory 与 Table Directory 主要用于索引前文 77 幅图和 288 个表。本译文已把实现相关的图表信息直接归入对应章节，因此不重复逐项翻译索引页。

---

# 附录 A：面向“CANape 类变量读取工具”的实现总结

如果你的目标不是完整实现 ASAM XCP 的所有能力，而是先实现 **A2L + XCP 读取 ECU 内部变量**，建议把协议裁剪成三阶段。

## A.1 第一阶段：Polling Measurement MVP

最低需要：

```text
Transport
  CAN / Ethernet

Session
  CONNECT
  GET_STATUS
  GET_COMM_MODE_INFO（如果支持）

Memory Read
  SHORT_UPLOAD
  或 SET_MTA + UPLOAD

Security
  GET_SEED + UNLOCK（仅当 DAQ/CAL 等目标资源确实被保护）

A2L
  MEASUREMENT
  ECU_ADDRESS / ADDRESS_EXTENSION
  datatype / byte order
  COMPU_METHOD
  unit
```

核心数据流：

```text
变量名 VehicleSpeed
        ↓
A2L Database
        ↓
Address + Extension + Type + Conversion
        ↓
SHORT_UPLOAD
        ↓
Raw Bytes
        ↓
Byte Order / Data Type Decode
        ↓
COMPU_METHOD
        ↓
12.34 km/h
```

此阶段完全可以不实现 DAQ、STIM、Page Switching、Programming 和高级 Time Correlation。

## A.2 第二阶段：高速 DAQ Measurement

增加能力查询：

```text
GET_DAQ_PROCESSOR_INFO
GET_DAQ_RESOLUTION_INFO
GET_DAQ_EVENT_INFO
GET_DAQ_LIST_INFO（Static DAQ）
```

Dynamic DAQ：

```text
FREE_DAQ
ALLOC_DAQ
ALLOC_ODT
ALLOC_ODT_ENTRY
SET_DAQ_PTR
WRITE_DAQ
SET_DAQ_LIST_MODE
START_STOP_DAQ_LIST(Select)
GET_DAQ_CLOCK
START_STOP_SYNCH(Start Selected)
```

DTO Receiver 需要根据 `DAQ_KEY_BYTE` 给出的 Identification Field Type 识别：

```text
DAQ List
ODT
Timestamp
Data Payload
```

然后按照本地 Measurement Layout 将 DTO Byte 解包到每个 Signal。

## A.3 第三阶段：Calibration/完整工具

再逐步增加：

```text
DOWNLOAD / DOWNLOAD_NEXT
Page Switching
Seed&Key DLL/SO
Checksum
RESUME DAQ
MDF Recorder
STIM / Bypassing
Flash Programming
Advanced Time Correlation
```

从工程风险看，Programming 与 Bypassing 应晚于 Measurement/Calibration：它们的错误代价更高，且依赖更多 ECU Vendor-specific Flow Control。

## A.4 推荐 C++ 模块划分

```text
XcpMaster
├── XcpSession
├── XcpCommandCodec
├── XcpResponseParser
├── XcpMemoryAccess
├── XcpSecurity
├── XcpDaqManager
│   ├── DaqAllocator
│   ├── OdtPacker
│   └── DtoDecoder
├── XcpCalibration
├── XcpProgramming
└── XcpTimeCorrelation

IXcpTransport
├── XcpCanTransport
├── XcpCanFdTransport
├── XcpUdpTransport
└── XcpTcpTransport

A2lDatabase
├── Measurement
├── Characteristic
├── CompuMethod
├── RecordLayout
├── EventChannel
└── XcpIfData
```

最关键的设计原则是：**Protocol Layer 不应直接依赖 Vector/PEAK/SocketCAN API**。Transport 只负责发送/接收 CTO/DTO Byte Stream；XCP Core 只处理 Protocol Semantic。这样同一套 Master 可以复用到 CAN、CAN FD 和 Ethernet。

## A.5 规范阅读优先级

对于你当前“像 CANape 一样读取变量”的目标，优先阅读顺序可以简化为：

```text
Chapter 5   XCP Protocol / State Machine
    ↓
Chapter 7.1 Packet / CTO / DTO
    ↓
CONNECT / GET_STATUS / GET_COMM_MODE_INFO
    ↓
SET_MTA / UPLOAD / SHORT_UPLOAD
    ↓
Chapter 8   A2L / IF_DATA Interface
    ↓
Chapter 4.2 Polling
    ↓
Chapter 4.1 + 7.5.4 DAQ
    ↓
Chapter 7.6 Error Handling
    ↓
Seed&Key / Calibration / Programming（按项目需要）
```

---

**译文结束。**
