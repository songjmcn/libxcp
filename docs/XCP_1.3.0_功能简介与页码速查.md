# ASAM MCD-1 XCP 1.3.0 功能简介与页码速查

> 基于文档：**ASAM MCD-1 (XCP) – Universal Measurement and Calibration Protocol – Protocol Layer Specification – Version 1.3.0 – 2015-05-01 – Base Standard**
>
> 本文按原规范结构整理，重点面向 XCP Master / CANape 类工具开发、ECU 测量、标定、DAQ、刷写、A2L、Seed&Key、时间同步和错误处理。
>
> **注意**：本规范是 Base Standard / Protocol Layer。XCP on CAN、XCP on Ethernet、XCP on USB、XCP on FlexRay、XCP on SxI 的具体传输封装由独立 Transport Layer 标准定义。

---

# 1. XCP 是什么

XCP（Universal Measurement and Calibration Protocol）是一套面向 ECU 开发、测试、标定和刷写的通用协议。

主要特性：

- Single Master / Multi Slave
- 协议层与传输层分离
- 地址化 ECU 内存访问
- A2L 描述变量、标定量和通信参数

主要应用包括：

- ECU 开发与测试
- ECU 在线测量
- ECU 在线标定
- DAQ 高速采集
- STIM 数据刺激
- Rapid Control Prototyping / Bypass
- Flash Programming
- 时间同步与多设备时间关联

**原文参考：[p.9–10](XCP_1.3.0_document.md#1-前言)**

---

# 2. XCP 功能总览

| 功能类别 | 核心用途 | 原文页码 |
|---|---|---:|
| 同步数据传输 DAQ/STIM | 周期采集 ECU 变量、向 ECU 注入刺激数据 | [13–27](XCP_1.3.0_document.md#41-同步数据传输) |
| Measurement Modes | Polling 与 DAQ 测量模式 | [28–31](XCP_1.3.0_document.md#42-测量模式) |
| Bypassing | 快速控制原型、外部算法旁路 ECU 功能 | [32–41](XCP_1.3.0_document.md#43-bypassing旁路rcp) |
| Online Calibration | 在线读写标定量、Page/Segment 切换 | [42–46](XCP_1.3.0_document.md#44-在线标定) |
| Flash Programming | 擦除、写入、校验 ECU 非易失存储器 | [46–50](XCP_1.3.0_document.md#45-flash-programming) |
| Time Correlation | ECU、XCP Slave、外部时钟之间时间同步 | [50–61](XCP_1.3.0_document.md#46-时间相关time-correlation) |
| ECU States | 向 Master 传递 ECU 状态 | [61–63](XCP_1.3.0_document.md#47-ecu-states) |
| XCP Communication Model | 标准、Block、Interleaved 通信模型 | [64–72](XCP_1.3.0_document.md#52-xcp-通信模型) |
| Performance Limits | DAQ/STIM 带宽、CPU/RAM 开销约束 | [73–82](XCP_1.3.0_document.md#6-性能极限) |
| XCP Packet / Protocol Layer | CTO/DTO、PID、计数器、时间戳、数据域 | [83–96](XCP_1.3.0_document.md#71-xcp-packet) |
| Standard Commands | 连接、状态、ID、内存读取、Checksum 等 | [101–129](XCP_1.3.0_document.md#751-标准命令standard-commands) |
| Calibration Commands | DOWNLOAD / SHORT_DOWNLOAD / MODIFY_BITS | [130–136](XCP_1.3.0_document.md#752-calibration-command) |
| Page Switching | 标定页读取、切换、复制 | [137–149](XCP_1.3.0_document.md#753-page-switching-command) |
| DAQ/STIM Commands | ODT 配置、DAQ 动态分配、启动/停止 | [150–188](XCP_1.3.0_document.md#754-data-acquisition-and-stimulation-command) |
| Programming Commands | PROGRAM_START/CLEAR/PROGRAM/VERIFY/RESET | [189–206](XCP_1.3.0_document.md#755-non-volatile-memory-programming) |
| Time Correlation Commands | 时间同步属性与扩展同步 | [207–222](XCP_1.3.0_document.md#756-time-correlation) |
| Error Handling | Timeout、错误码、恢复策略 | [223–241](XCP_1.3.0_document.md#76-communication-error-handling) |
| Event Handling | Resume、DAQ Overload、ECU State Change 等 | [242–254](XCP_1.3.0_document.md#77-event-packet-描述) |
| A2L / MCD-2 MC 接口 | IF_DATA XCP/XCPplus、协议参数描述 | [255–259](XCP_1.3.0_document.md#8-与-asam-mcd-2-mc-description-file-的接口) |
| Seed&Key 接口 | 受保护资源解锁 | [260–261](XCP_1.3.0_document.md#9-外部-seedkey-函数接口) |
| External Checksum | 用户自定义校验算法 | [262](XCP_1.3.0_document.md#10-外部-checksum-函数接口) |
| A2L 解压/解密接口 | ECU 提供压缩/加密 A2L 时的处理接口 | [263](XCP_1.3.0_document.md#11-外部-a2l-解压解密接口) |
| 官方通信示例 | Session、Calibration、DAQ、Programming | [264–283](XCP_1.3.0_document.md#12-示例) |

---

# 3. 同步数据传输：DAQ / STIM / ODT

## 3.1 DAQ

DAQ（Data Acquisition）用于 Slave → Master 的高速同步数据采集。

```text
ECU Internal Variables
        ↓
    ODT Entry
        ↓
       ODT
        ↓
    DAQ List
        ↓
  Event Channel
        ↓
      DTO
        ↓
      Master
```

适合 1 ms / 5 ms / 10 ms 等周期变量采集、多变量同步采集，以及比 Polling 更高吞吐率的测量。

**原文参考：[p.13–27](XCP_1.3.0_document.md#411-daqstim-与-odt)**

## 3.2 STIM

STIM（Data Stimulation）是 DAQ 的反方向，用于 Master → Slave 的同步数据刺激，可用于外部算法输入、Rapid Control Prototyping 和 Bypass。

**原文参考：[p.13–27](XCP_1.3.0_document.md#411-daqstim-与-odt)**

## 3.3 ODT Entry / ODT / DAQ List

ODT Entry 描述 ECU 内存中的一个数据元素，包含 Address、Address Extension、Size，位变量还可包含 Bit Offset。多个 ODT Entry 构成 ODT，多个 ODT 构成 DAQ List。DAQ List 可以绑定 Event Channel、设置优先级、Prescaler 和 Timestamp。

**原文参考：[p.13–16](XCP_1.3.0_document.md#412-odt-entry)**

---

# 4. Dynamic DAQ

XCP 支持动态创建 DAQ List、ODT 和 ODT Entry。标准配置顺序是：

```text
FREE_DAQ
   ↓
ALLOC_DAQ
   ↓
ALLOC_ODT
   ↓
ALLOC_ODT_ENTRY
   ↓
SET_DAQ_PTR
   ↓
WRITE_DAQ
```

顺序错误可能返回 `ERR_SEQUENCE`，内存不足返回 `ERR_MEMORY_OVERFLOW`。

**原文参考：[p.16–20](XCP_1.3.0_document.md#416-动态-daq-配置)**

| 命令 | 功能 | 页码 |
|---|---|---:|
| FREE_DAQ | 清除动态 DAQ 配置 | [185](XCP_1.3.0_document.md#75416-free_daq释放-dynamic-daq-配置) |
| ALLOC_DAQ | 分配 DAQ List | [186](XCP_1.3.0_document.md#75417-alloc_daq分配-daq-lists) |
| ALLOC_ODT | 为 DAQ List 分配 ODT | [187](XCP_1.3.0_document.md#75418-alloc_odt给-daq-list-分配-odt) |
| ALLOC_ODT_ENTRY | 为 ODT 分配 Entry | [188](XCP_1.3.0_document.md#75419-alloc_odt_entry给-odt-分配-entry) |

---

# 5. DAQ 配置保存与 RESUME

XCP 可以将 DAQ 配置保存到非易失存储器，用于避免重复配置，或者在 Slave 上电后自动启动 DAQ/STIM。Master 可通过 Session Configuration ID 判断保存配置是否匹配。

**原文参考：[p.21–24](XCP_1.3.0_document.md#417-daq-配置保存与上电数据传输)**（RESUME Mode 见[对应小节](XCP_1.3.0_document.md#4172-保存-daq-配置并进入-resume-mode)）

---

# 6. Measurement Modes

## 6.1 Polling

Master 主动读取 ECU 内存。规范推荐 `SHORT_UPLOAD`，也可使用 `SET_MTA + UPLOAD`。

优点：实现简单，无需配置 DAQ。缺点：请求负载较高、同步性和实时性不如 DAQ。

**原文参考：[p.28–29](XCP_1.3.0_document.md#421-polling轮询)**

## 6.2 DAQ Burst Measurement

Master 预先配置 DAQ，之后 Slave 根据内部 Event 自动发送 DTO。规范还描述 Standard Burst、Improved Burst、Alternating 等模式。

**原文参考：[p.29–31](XCP_1.3.0_document.md#42-测量模式)**

---

# 7. Bypassing

Bypass 用于 Rapid Control Prototyping。典型链路：

```text
ECU Internal Function
       ↓
DAQ → External Controller
       ↓
External Algorithm
       ↓
STIM → ECU
```

规范涉及 Delayed Bypassing、Bypass Activation、Startup、Plausibility Checks、Consistency、Event Channel Relations 和 Minimum Separation Time。

**原文参考：[p.32–41](XCP_1.3.0_document.md#43-bypassing旁路rcp)**

---

# 8. Online Calibration

XCP 支持 ECU 在线标定。核心概念：SECTOR、SEGMENT、PAGE。

- SECTOR：Flash 物理区域
- SEGMENT：逻辑标定区域
- PAGE：同一标定数据的不同版本

支持在线读写标定值、Calibration Page Switching、Page Copy 和 Calibration Data Freezing。

**原文参考：[p.42–46](XCP_1.3.0_document.md#44-在线标定)**

---

# 9. 标定数据写入

| 命令 | 功能 | 页码 |
|---|---|---:|
| DOWNLOAD | Master → Slave 写内存 | [130](XCP_1.3.0_document.md#7521-downloadmaster-slave-写内存) |
| DOWNLOAD_NEXT | Block Mode 连续下载 | [132](XCP_1.3.0_document.md#7522-download_nextblock-mode-后续数据) |
| DOWNLOAD_MAX | 固定最大长度写入 | [134](XCP_1.3.0_document.md#7523-download_max固定最大长度写入) |
| SHORT_DOWNLOAD | 短格式直接写指定地址 | [135](XCP_1.3.0_document.md#7524-short_download带地址的短写入) |
| MODIFY_BITS | 原子位修改 | [136](XCP_1.3.0_document.md#7525-modify_bits原子位修改) |

典型流程：`SET_MTA → DOWNLOAD`，也可直接使用 `SHORT_DOWNLOAD`。

---

# 10. Calibration Page Switching

| 命令 | 功能 | 页码 |
|---|---|---:|
| SET_CAL_PAGE | 设置激活 Page | [137](XCP_1.3.0_document.md#7531-set_cal_page) |
| GET_CAL_PAGE | 获取当前 Page | [138](XCP_1.3.0_document.md#7532-get_cal_page) |
| GET_PAG_PROCESSOR_INFO | 获取 PAG Processor 信息 | [139](XCP_1.3.0_document.md#7533-get_pag_processor_info) |
| GET_SEGMENT_INFO | 获取 Segment 信息 | [140](XCP_1.3.0_document.md#7534-get_segment_info) |
| GET_PAGE_INFO | 获取 Page 信息 | [143](XCP_1.3.0_document.md#7535-get_page_info) |
| SET_SEGMENT_MODE | 设置 Segment Mode | [147](XCP_1.3.0_document.md#7536-set_segment_mode) |
| GET_SEGMENT_MODE | 获取 Segment Mode | [148](XCP_1.3.0_document.md#7537-get_segment_mode) |
| COPY_CAL_PAGE | Page 间复制 | [149](XCP_1.3.0_document.md#7538-copy_cal_page) |

---

# 11. Flash Programming

XCP 支持 ECU 非易失存储器刷写。

```text
PROGRAM_START
    ↓
PROGRAM_CLEAR
    ↓
SET_MTA
    ↓
PROGRAM / PROGRAM_NEXT
    ↓
PROGRAM_VERIFY
    ↓
PROGRAM_RESET
```

**原文参考：[p.46–50](XCP_1.3.0_document.md#45-flash-programming)、[p.189–206](XCP_1.3.0_document.md#755-non-volatile-memory-programming)**

| 命令 | Code | 功能 | 页码 |
|---|---:|---|---:|
| PROGRAM_START | 0xD2 | 开始编程序列 | [189](XCP_1.3.0_document.md#7551-program_start) |
| PROGRAM_CLEAR | 0xD1 | 擦除非易失存储区域 | [191](XCP_1.3.0_document.md#7552-program_clear) |
| PROGRAM | 0xD0 | 写入 Flash | [193](XCP_1.3.0_document.md#7553-program) |
| PROGRAM_RESET | 0xCF | 结束编程并复位 | [195](XCP_1.3.0_document.md#7554-program_reset) |
| GET_PGM_PROCESSOR_INFO | 0xCE | 获取编程能力 | [196](XCP_1.3.0_document.md#7555-get_pgm_processor_info) |
| GET_SECTOR_INFO | 0xCD | 获取 Sector 信息 | [199](XCP_1.3.0_document.md#7556-get_sector_info) |
| PROGRAM_PREPARE | 0xCC | 编程准备 | [201](XCP_1.3.0_document.md#7557-program_prepare) |
| PROGRAM_FORMAT | 0xCB | 设置 Programming 数据格式 | [202](XCP_1.3.0_document.md#7558-program_format) |
| PROGRAM_NEXT | 0xCA | Block Mode 连续编程 | [204](XCP_1.3.0_document.md#7559-program_next) |
| PROGRAM_MAX | 0xC9 | 最大长度 Program | [205](XCP_1.3.0_document.md#75510-program_max) |
| PROGRAM_VERIFY | 0xC8 | 编程结果验证 | [206](XCP_1.3.0_document.md#75511-program_verify) |

XCP 重点标准化真正的 Programming 动作；版本检查、OEM 安全流程、项目特定 Bootloader 管理通常仍由项目定义。

---

# 12. Time Correlation

XCP 支持 DAQ Timestamp、XCP Slave Clock、ECU Clock、Global Clock、Grandmaster Clock，以及多设备时间关联，可用于多 ECU 和多总线测量统一时间轴。

**原文参考：[p.50–61](XCP_1.3.0_document.md#46-时间相关time-correlation)**

关键命令：

- [GET_DAQ_CLOCK：p.160](XCP_1.3.0_document.md#7548-get_daq_clock读取-slave-daq-clock)
- [TIME_CORRELATION_PROPERTIES：p.207](XCP_1.3.0_document.md#7561-time_correlation_properties)

---

# 13. ECU States

XCP 可以向 Master 暴露 ECU 当前状态，并支持 ECU 状态变化事件，使工具能够根据 ECU 状态控制测量/标定，同时维持 A2L 语义一致性。

**原文参考：[p.61–63](XCP_1.3.0_document.md#47-ecu-states)**

---

# 14. XCP 通信模型与状态机

XCP 是 Single Master / Multi Slave 架构，支持：

- Standard Communication Model
- Block Transfer Communication Model
- Interleaved Communication Model

还定义连接状态、DAQ 运行状态、Programming Session、Resume 等状态约束。

**原文参考：[p.64–72](XCP_1.3.0_document.md#52-xcp-通信模型)**

---

# 15. Resource Protection

XCP 资源包括：

```text
CAL/PAG
DAQ
STIM
PGM
```

资源可被保护。典型解锁流程：

```text
GET_SEED
   ↓
Vendor Algorithm
   ↓
KEY
   ↓
UNLOCK
```

**原文参考：[p.71](XCP_1.3.0_document.md#54-protection-handling)、[p.116–120](XCP_1.3.0_document.md#7518-get_seed读取解锁-seed)、[p.260–261](XCP_1.3.0_document.md#9-外部-seedkey-函数接口)**

---

# 16. XCP Packet、CTO 与 DTO

协议层定义 Identification Field、Counter Field、Timestamp Field 和 Data Field。

CTO（Command Transfer Object）用于 CMD、RES、ERR、EV、SERV；DTO（Data Transfer Object）用于 DAQ/STIM。

**原文参考：[p.83–95](XCP_1.3.0_document.md#71-xcp-packet)**

---

# 17. Standard Commands

| 命令 | Code | 功能 | 页码 |
|---|---:|---|---:|
| CONNECT | 0xFF | 建立连接 | [101](XCP_1.3.0_document.md#7511-connect与-slave-建立连接) |
| DISCONNECT | 0xFE | 断开连接 | [105](XCP_1.3.0_document.md#7512-disconnect断开连接) |
| GET_STATUS | 0xFD | 获取当前 Session 状态 | [106](XCP_1.3.0_document.md#7513-get_status获取当前会话状态) |
| SYNCH | 0xFC | Timeout 后同步命令执行 | [109](XCP_1.3.0_document.md#7514-synch超时后的命令执行同步) |
| GET_COMM_MODE_INFO | 0xFB | 获取通信模式能力 | [110](XCP_1.3.0_document.md#7515-get_comm_mode_info读取扩展通信模式信息) |
| GET_ID | 0xFA | 获取 Slave 标识信息 | [112](XCP_1.3.0_document.md#7516-get_id获取-slave-标识或-a2l-信息) |
| SET_REQUEST | 0xF9 | 请求保存等异步操作 | [114](XCP_1.3.0_document.md#7517-set_request请求保存清除非易失数据) |
| GET_SEED | 0xF8 | 获取 Seed | [116](XCP_1.3.0_document.md#7518-get_seed读取解锁-seed) |
| UNLOCK | 0xF7 | 解锁资源 | [118](XCP_1.3.0_document.md#7519-unlock发送-key-并解锁资源) |
| SET_MTA | 0xF6 | 设置 Memory Transfer Address | [121](XCP_1.3.0_document.md#75110-set_mta设置-memory-transfer-address) |
| UPLOAD | 0xF5 | Slave → Master 读取内存 | [122](XCP_1.3.0_document.md#75111-uploadslave-master-内存上传) |
| SHORT_UPLOAD | 0xF4 | 直接地址短读取 | [124](XCP_1.3.0_document.md#75112-short_upload带地址的一次性读取) |
| BUILD_CHECKSUM | 0xF3 | 计算内存 Checksum | [125](XCP_1.3.0_document.md#75113-build_checksum计算内存校验和) |
| TRANSPORT_LAYER_CMD | 0xF2 | Transport Layer 特定命令 | [128](XCP_1.3.0_document.md#75114-transport_layer_cmd) |
| USER_CMD | 0xF1 | 用户自定义命令 | [129](XCP_1.3.0_document.md#75115-user_cmd) |

---

# 18. Memory Transfer Address（MTA）

MTA 是 XCP 的核心内存访问机制之一。

```text
SET_MTA(address)
UPLOAD(size)
```

或：

```text
SET_MTA(address)
DOWNLOAD(data)
```

MTA 可用于变量读取、标定写入、Checksum 和 Flash Programming。

**原文参考：[p.121](XCP_1.3.0_document.md#75110-set_mta设置-memory-transfer-address)**

---

# 19. DAQ/STIM 命令速查

| 命令 | 功能 | 页码 |
|---|---|---:|
| SET_DAQ_PTR | 设置当前 ODT Entry 指针 | [150](XCP_1.3.0_document.md#7541-set_daq_ptr设置-odt-entry-配置指针) |
| WRITE_DAQ | 配置一个 ODT Entry | [151](XCP_1.3.0_document.md#7542-write_daq写一个-odt-entry) |
| SET_DAQ_LIST_MODE | 配置 DAQ List 模式 | [152](XCP_1.3.0_document.md#7543-set_daq_list_mode设置-daq-list-工作模式) |
| START_STOP_DAQ_LIST | 单个 DAQ List 启停/选择 | [154](XCP_1.3.0_document.md#7544-start_stop_daq_list启动停止选择一个-daq-list) |
| START_STOP_SYNCH | 同步启动/停止多个 DAQ List | [156](XCP_1.3.0_document.md#7545-start_stop_synch同步启动停止多个-daq-list) |
| WRITE_DAQ_MULTIPLE | 一次写多个 ODT Entry | [157](XCP_1.3.0_document.md#7546-write_daq_multiple一次写多个-odt-entry) |
| READ_DAQ | 读取 ODT Entry 配置 | [159](XCP_1.3.0_document.md#7547-read_daq读取一个-odt-entry-配置) |
| GET_DAQ_CLOCK | 获取 DAQ Clock | [160](XCP_1.3.0_document.md#7548-get_daq_clock读取-slave-daq-clock) |
| GET_DAQ_PROCESSOR_INFO | 获取 DAQ 总体能力 | [162](XCP_1.3.0_document.md#7549-get_daq_processor_info查询-daq-processor-总体能力) |
| GET_DAQ_RESOLUTION_INFO | 获取 DAQ 粒度/时间戳信息 | [168](XCP_1.3.0_document.md#75410-get_daq_resolution_info查询-odt-entry-与-timestamp-分辨率) |
| GET_DAQ_LIST_MODE | 获取 DAQ List Mode | [171](XCP_1.3.0_document.md#75411-get_daq_list_mode查询-daq-list-当前状态) |
| GET_DAQ_EVENT_INFO | 获取 Event Channel 信息 | [173](XCP_1.3.0_document.md#75412-get_daq_event_info查询-event-channel) |
| DTO_CTR_PROPERTIES | DTO Counter 能力 | [177](XCP_1.3.0_document.md#75413-dto_ctr_propertiesdto-counter-属性) |
| CLEAR_DAQ_LIST | 清空 DAQ List | [182](XCP_1.3.0_document.md#75414-clear_daq_list清除一个-staticconfigurable-daq-list) |
| GET_DAQ_LIST_INFO | 获取 DAQ List 信息 | [183](XCP_1.3.0_document.md#75415-get_daq_list_info查询-static-daq-list-信息) |
| FREE_DAQ | 清除 Dynamic DAQ | [185](XCP_1.3.0_document.md#75416-free_daq释放-dynamic-daq-配置) |
| ALLOC_DAQ | 动态分配 DAQ List | [186](XCP_1.3.0_document.md#75417-alloc_daq分配-daq-lists) |
| ALLOC_ODT | 动态分配 ODT | [187](XCP_1.3.0_document.md#75418-alloc_odt给-daq-list-分配-odt) |
| ALLOC_ODT_ENTRY | 动态分配 ODT Entry | [188](XCP_1.3.0_document.md#75419-alloc_odt_entry给-odt-分配-entry) |

---

# 20. Error Handling

XCP 定义 Error、Pre-Action、Action、Error Severity、Timeout Handling 和 Error Code Handling，适用于 Standard、Block 和 Interleaved 通信模式。

**原文参考：[p.223–241](XCP_1.3.0_document.md#76-communication-error-handling)**

---

# 21. Event Handling

Slave 可以主动向 Master 发送事件，包括 Resume Mode、DAQ Clear/Store、Calibration Store、DAQ Overload、Autonomous Disconnect、Time Sync、STIM Timeout、Sleep、ECU State Changed、User Defined Event、Transport Layer Event。

**原文参考：[p.242–254](XCP_1.3.0_document.md#77-event-packet-描述)**

---

# 22. A2L / ASAM MCD-2 MC 接口

XCP 本身通过地址访问 ECU；A2L 提供变量名、Address、Address Extension、Datatype、Dimension、Conversion、Unit、Event 和 XCP 参数。

```text
A2L
 ↓
Measurement / Characteristic
 ↓
Address + Type
 ↓
XCP
 ↓
ECU
```

**原文参考：[p.255–259](XCP_1.3.0_document.md#8-与-asam-mcd-2-mc-description-file-的接口)**

---

# 23. IF_DATA XCP / XCPplus

A2L 的 IF_DATA 可以描述 Protocol Layer、Transport Layer、DAQ 参数以及多个 XCP Transport 实例。`XCPplus` 可描述多个同类型 Transport Layer 实例。

如果 A2L 参数和 Slave 实际查询参数不一致，Master 应检测不一致并提示用户。

**原文参考：[p.255–259](XCP_1.3.0_document.md#8-与-asam-mcd-2-mc-description-file-的接口)**

---

# 24. Seed & Key 外部接口

受保护资源可通过 Vendor 自定义 Seed&Key 算法解锁。Windows 可使用 DLL，Unix/Linux 可使用 SO。标准定义外部函数接口：

```text
XCP_GetAvailablePrivileges
XCP_ComputeKeyFromSeed
```

**原文参考：[p.260–261](XCP_1.3.0_document.md#9-外部-seedkey-函数接口)**

---

# 25. External Checksum

如果 ECU 使用 `XCP_USER_DEFINED` Checksum，Master 可调用 Vendor 提供的 DLL/SO 实现自定义校验算法。

**原文参考：[p.262](XCP_1.3.0_document.md#10-外部-checksum-函数接口)**

---

# 26. A2L Decompression / Decryption

Slave 可返回压缩或加密的 A2L 数据，Master 可调用 Vendor 外部函数完成解压/解密。标准定义：

```text
XCP_DecompressA2L(...)
XCP_ReleaseDecompressedData(...)
```

**原文参考：[p.263](XCP_1.3.0_document.md#11-外部-a2l-解压解密接口)**

---

# 27. 官方完整流程示例

| 示例 | 页码 |
|---|---:|
| Configuration Examples | [264](XCP_1.3.0_document.md#121-configuration-example) |
| GET_ID Identification | [264](XCP_1.3.0_document.md#122-get_id-标识字符串示例) |
| Setting up a Session | [265](XCP_1.3.0_document.md#124-建立-session) |
| Seed&Key Unlock | [266–267](XCP_1.3.0_document.md#seedkey-解锁示例) |
| Calibration | [267–269](XCP_1.3.0_document.md#125-calibration-示例) |
| DAQ Processor 查询 | [270–271](XCP_1.3.0_document.md#1261-查询-daq-能力) |
| Dynamic DAQ Allocation | [272](XCP_1.3.0_document.md#1262-准备-daq-list) |
| ODT 配置 | [273](XCP_1.3.0_document.md#1263-配置-odt-entry) |
| DAQ Start | [274](XCP_1.3.0_document.md#1264-开始-daq) |
| DAQ Stop | [275](XCP_1.3.0_document.md#1265-停止-daq) |
| Flash Programming | [275–276](XCP_1.3.0_document.md#127-reprogramming-slave-示例) |
| DISCONNECT | [276](XCP_1.3.0_document.md#128-关闭-session) |
| Time Correlation | [277+](XCP_1.3.0_document.md#129-time-correlation-示例) |

对于实现 XCP Master，第 [12 章](XCP_1.3.0_document.md#12-示例)是最值得优先阅读的工程示例部分。

---

# 28. 面向 CANape 类工具的功能映射

```text
XCP Master
│
├── Session
│   ├── CONNECT
│   ├── DISCONNECT
│   └── GET_STATUS
│
├── Memory Access
│   ├── SET_MTA
│   ├── UPLOAD
│   ├── SHORT_UPLOAD
│   ├── DOWNLOAD
│   └── SHORT_DOWNLOAD
│
├── Measurement
│   ├── Polling
│   ├── DAQ
│   ├── Event Channel
│   ├── ODT
│   └── Timestamp
│
├── Calibration
│   ├── Characteristics
│   ├── PAGE
│   ├── SEGMENT
│   └── Page Switching
│
├── Programming
│   ├── PROGRAM_START
│   ├── PROGRAM_CLEAR
│   ├── PROGRAM
│   ├── PROGRAM_VERIFY
│   └── PROGRAM_RESET
│
├── Security
│   └── Seed & Key
│
├── A2L
│   ├── MEASUREMENT
│   ├── CHARACTERISTIC
│   ├── COMPU_METHOD
│   └── IF_DATA
│
└── Time
    ├── DAQ Clock
    └── Time Correlation
```

---

# 29. 推荐实现优先级

## Phase 1：最小可用 Measurement

实现 CONNECT、GET_STATUS、GET_COMM_MODE_INFO、GET_ID、SET_MTA、UPLOAD、SHORT_UPLOAD 和 A2L Parsing。

目标：`变量名 → ECU 实时值`

重点页：`101, 106, 110, 112, 121, 122, 124, 255–259`

## Phase 2：DAQ

实现 GET_DAQ_PROCESSOR_INFO、GET_DAQ_RESOLUTION_INFO、GET_DAQ_EVENT_INFO、FREE_DAQ、ALLOC_DAQ、ALLOC_ODT、ALLOC_ODT_ENTRY、SET_DAQ_PTR、WRITE_DAQ、SET_DAQ_LIST_MODE、START_STOP_DAQ_LIST、START_STOP_SYNCH。

重点页：`150–188, 270–275`

## Phase 3：Calibration

实现 DOWNLOAD、SHORT_DOWNLOAD、GET_CAL_PAGE、SET_CAL_PAGE、COPY_CAL_PAGE。

重点页：`130–149, 267–269`

## Phase 4：Seed & Key

实现 GET_SEED、UNLOCK 和外部 DLL/SO 接口。

重点页：`116–120, 260–261`

## Phase 5：Flash Programming

实现 PROGRAM_START、PROGRAM_CLEAR、PROGRAM、PROGRAM_NEXT、PROGRAM_VERIFY、PROGRAM_RESET。

重点页：`189–206, 275–276`

---

# 30. 一页式功能速查

```text
┌───────────────────────────────────────────────────────────────┐
│                       XCP 1.3.0                              │
├───────────────────────────────────────────────────────────────┤
│ Session        CONNECT / DISCONNECT / STATUS      101–110    │
│ Identification GET_ID                             112         │
│ Security       GET_SEED / UNLOCK                  116–120    │
│ Memory Read    SET_MTA / UPLOAD / SHORT_UPLOAD    121–124    │
│ Checksum       BUILD_CHECKSUM                     125–127    │
│ Calibration    DOWNLOAD / SHORT_DOWNLOAD          130–136    │
│ Page Switch    CAL PAGE / SEGMENT                 137–149    │
│ DAQ/STIM       DAQ / ODT / Event                  150–188    │
│ Programming    Flash                              189–206    │
│ Time Sync      Time Correlation                   207–222    │
│ Error          Timeout / Error Code               223–241    │
│ Event          EV_*                               242–254    │
│ A2L            IF_DATA XCP/XCPplus                255–259    │
│ Seed&Key DLL   External Security Interface        260–261    │
│ Checksum DLL   External Checksum                  262         │
│ A2L Crypto     Decompress / Decrypt               263         │
│ Examples       Full Communication Sequences       264–283    │
└───────────────────────────────────────────────────────────────┘
```

---

# 31. 总结

XCP 不只是“读取 ECU 变量”的协议，而是一套覆盖测量、标定、数据刺激、快速原型、Flash Programming、安全解锁和时间同步的 ECU 开发期协议栈。

对于 CANape 类工具：

- A2L 负责回答“变量是什么”
- XCP Memory Access 负责“如何读取/修改”
- DAQ 负责“如何高速同步采集”
- Calibration 负责“如何在线标定”
- Programming 负责“如何刷写 ECU”
- Seed&Key 负责“如何访问受保护资源”
- Time Correlation 负责“如何把多个设备数据放到同一时间轴”

因此，XCP 可以作为一个完整的 ECU 开发期测量、标定与编程协议栈，而不仅仅是变量读取协议。
