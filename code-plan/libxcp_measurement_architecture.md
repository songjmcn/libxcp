# libxcp Measurement-Only 架构设计与后续演进建议

> 适用版本：基于当前 `libxcp` 更新版本的源码审核结果  
> 目标：将项目收敛为 **Measurement-only XCP Master SDK**，优先支持 XCP on UDP，后续扩展 CAN/CAN FD。  
> 非目标：Calibration、Programming、STIM 不属于第一阶段交付范围。

---

## 1. 文档目的

本文档用于指导 `libxcp` 从当前“协议核心 + 基础 Measurement 能力”继续演进为一个可实际使用的 **C++ XCP Measurement Master SDK**。

当前项目已经具备以下基础能力：

- XCP Session 基本状态机
- Command Codec / Response Parser
- UDP Transport
- CommandExecutor
- MemoryAccess
- XcpMaster
- Polling Measurement
- Seed & Key
- 基础 DAQ 配置与控制
- Dynamic DAQ
- DTO 异步接收
- A2L 基础解析、变量定位与物理值转换
- XCPlite 对手端 DAQ 测试

因此后续开发重点不再是“继续补齐所有 XCP 命令”，而应转向：

1. 统一 DTO 解析
2. 完善 DAQ Event 查询
3. A2L → DAQ 自动规划
4. 提供高层 MeasurementSession API
5. 完善时间戳、异常恢复与生产级测试

---

## 2. 产品边界

### 2.1 第一阶段目标

第一阶段将 `libxcp` 定位为：

> 一个支持 XCP on UDP 的 Measurement-only C++ Master SDK，能够通过 A2L 选择变量，并通过 Polling 或 DAQ 获取物理值。

目标用户代码最终应接近：

```cpp
XcpMaster master(transport);
master.Connect();

MeasurementSession session(master, a2l_database);

session.Add("EngineSpeed");
session.Add("VehicleSpeed");
session.Add("ThrottlePosition");

session.Start([](const MeasurementFrame& frame) {
    for (const auto& sample : frame.samples) {
        std::cout << sample.name
                  << " = " << sample.value
                  << " @ " << sample.timestamp.count()
                  << " ns\n";
    }
});
```

### 2.2 第一阶段不做

以下功能暂不作为第一版 Measurement SDK 的阻塞项：

- Calibration Page
- DOWNLOAD / SHORT_DOWNLOAD / MODIFY_BITS
- Programming (`PROGRAM_*`)
- STIM
- Flash programming
- 完整 ASAP2/A2L 标准覆盖
- MDF 写入
- GUI

如果仍希望保留 Calibration 相关代码，建议放到编译开关下：

```cmake
LIBXCP_ENABLE_CALIBRATION=OFF
```

默认关闭，避免 Measurement-only 主路径被无关功能复杂化。

---

## 3. 当前架构评价

当前主干结构基本合理，不建议推倒重构：

```text
                XcpMaster
                    │
       ┌────────────┼────────────┐
       │            │            │
    Session   CommandExecutor  MemoryAccess
                    │
              CommandCodec
                    │
             ResponseParser
                    │
              IXcpTransport
                    │
               UdpTransport
```

这套结构已经把：

- Session 管理
- 协议编码
- 响应解析
- 命令执行
- 内存读取
- 传输层

进行了合理解耦。

后续重点应在这套结构上增加 Measurement 子系统，而不是修改现有核心层职责。

---

## 4. 推荐的目标架构

建议最终形成如下结构：

```text
libxcp
│
├── core
│   ├── protocol_types
│   ├── command_codec
│   ├── response_parser
│   ├── session
│   ├── command_executor
│   └── xcp_error
│
├── transport
│   ├── ixcp_transport
│   ├── udp_transport
│   ├── tcp_transport              # 可选
│   ├── socketcan_transport        # 后续
│   ├── vector_xl_transport        # 后续
│   └── pcan_transport             # 后续
│
├── memory
│   └── memory_access
│
├── security
│   ├── seed_key
│   └── iseed_key_provider
│
├── daq
│   ├── daq_manager
│   ├── daq_list
│   ├── odt
│   ├── odt_entry
│   ├── daq_ledger
│   ├── dto_envelope_decoder       # 新增，优先级最高
│   ├── dto_payload_decoder
│   ├── daq_timestamp
│   └── event_channel
│
├── a2l
│   ├── ia2l_database
│   ├── a2l_bridge
│   ├── measurement_symbol
│   ├── compu_method
│   ├── if_data
│   └── physical_value
│
├── measurement
│   ├── measurement_planner        # 新增
│   ├── measurement_session        # 新增
│   ├── measurement_frame
│   ├── measurement_sample
│   └── polling_measurement
│
└── xcp_master
```

---

## 5. 核心原则

### 5.1 Transport 与 XCP 协议彻底解耦

`IXcpTransport` 只负责：

- Open / Close
- Send packet
- Receive packet
- Transport error / warning

不得承担：

- XCP 命令语义
- DAQ 路由
- A2L 解析
- Measurement 变量映射

这样后续扩展 CAN/CAN FD 时，不需要修改 XCP Core。

### 5.2 CTO 与 DTO 处理路径分离

必须继续保持：

```text
                         CommandExecutor
                               │
               ┌───────────────┴───────────────┐
               │                               │
             CTO                            DTO/EV/SERV
          RES / ERR                            │
               │                               │
      synchronous command                asynchronous path
```

目的：

- CTO 请求/响应严格保持单 Outstanding Command
- DTO 高速数据不能阻塞命令执行
- DAQ 数据可以独立交给采集线程或回调线程

---

# 6. 第一优先级：统一 DTO Envelope Decoder

这是当前最重要的架构缺口。

目前不同测试路径中仍存在：

- Absolute PID 路由
- Relative ODT
- Relative Word identification
- XCPlite 测试中的手工 `SplitEnvelope()`

这意味着当前 DTO 处理逻辑尚未真正统一。

## 6.1 新增 DtoEnvelopeDecoder

建议定义：

```cpp
struct DtoIdentity {
    std::uint16_t daq_list = 0;
    std::uint8_t odt = 0;
};

struct DtoEnvelope {
    DtoIdentity identity;

    std::optional<std::uint8_t> counter;
    std::optional<std::uint64_t> raw_timestamp;

    BytesView payload;
};
```

接口：

```cpp
class DtoEnvelopeDecoder {
public:
    DtoEnvelope Decode(
        BytesView dto,
        const DtoFrameLayout& frame_layout) const;
};
```

## 6.2 支持四种 PID/Identification 模式

至少覆盖：

```text
Absolute
RelativeByte
RelativeWord
RelativeWordAligned
```

统一输出：

```text
DAQ List ID
ODT ID
Counter(optional)
Timestamp(optional)
Payload
```

## 6.3 解耦 Envelope 和 Payload

不要让 `IDaqLayout::Decode()` 同时负责：

- 识别 DAQ/ODT
- 去掉 header
- 解析 timestamp
- 解 ODT Entry
- 做 A2L 物理值换算

建议拆成：

```text
Raw DTO
   │
   ▼
DtoEnvelopeDecoder
   │
   ▼
DtoEnvelope
   │
   ▼
DaqLayout / ODT Router
   │
   ▼
DtoPayloadDecoder
   │
   ▼
MeasurementSample[]
```

这样更容易支持不同 ECU 和不同 DAQ identification 模式。

---

# 7. 第二优先级：补 GET_DAQ_EVENT_INFO

目前已有：

```text
GET_DAQ_PROCESSOR_INFO
GET_DAQ_RESOLUTION_INFO
GET_DAQ_LIST_INFO
```

建议补：

```text
GET_DAQ_EVENT_INFO
```

原因：

- A2L Event Channel 信息可能陈旧
- ECU runtime 能力应支持校验
- Measurement Planner 需要 event 的周期和属性

建议结构：

```cpp
struct DaqEventInfo {
    std::uint16_t event_channel = 0;
    std::uint8_t properties = 0;
    std::uint8_t max_daq_lists = 0;
    std::uint8_t time_cycle = 0;
    std::uint8_t time_unit = 0;
    std::uint8_t priority = 0;
};
```

接口：

```cpp
DaqEventInfo XcpMaster::GetDaqEventInfo(
    std::uint16_t event_channel);
```

建议同时支持：

```text
A2L 声明值
    ↓ compare
ECU runtime 查询值
```

出现差异时给出 warning，而不是直接失败。

---

# 8. 第三优先级：MeasurementPlanner

当前底层已经有：

- A2L Symbol
- 地址
- 数据类型
- 字节序
- Event Channel
- DaqListSpec
- Dynamic DAQ

但中间缺少自动规划层。

## 8.1 Planner 输入

```cpp
std::vector<std::string> measurement_names;
```

例如：

```cpp
{
    "EngineSpeed",
    "VehicleSpeed",
    "ThrottlePosition"
}
```

## 8.2 Planner 输出

```cpp
struct MeasurementPlan {
    std::vector<DaqListSpec> daq_lists;
    std::vector<MeasurementRoute> routes;
};
```

其中：

```cpp
struct MeasurementRoute {
    std::string name;
    std::uint16_t daq_list;
    std::uint8_t odt;
    std::size_t payload_offset;
    std::size_t size;
};
```

## 8.3 Planner 需要考虑

- Address
- Address Extension
- Element size
- Data type
- Byte order
- Event Channel
- MAX_DTO
- DAQ Address Granularity
- Timestamp header
- PID/DAQ identification field
- ODT 数量
- ECU Dynamic DAQ 能力

## 8.4 推荐数据流

```text
A2L symbols
    │
    ▼
MeasurementPlanner
    │
    ├── group by Event Channel
    ├── calculate payload size
    ├── pack ODT entries
    ├── respect MAX_DTO
    └── generate routing table
    │
    ▼
MeasurementPlan
    │
    ├── DaqListSpec[]
    └── MeasurementRoute[]
```

---

# 9. ODT Packing 策略

第一版建议采用简单确定性算法，不需要追求最优装箱。

推荐：

1. 按 Event Channel 分组
2. 保持用户变量顺序
3. 尽量顺序填充当前 ODT
4. 超过 `MAX_DTO` 时创建新 ODT
5. 不允许一个变量跨 ODT
6. 一个 ODT Entry 对应一个 Measurement

伪代码：

```text
for each event:
    create DAQ list

    for each measurement:
        if current ODT has enough space:
            append ODT entry
        else:
            create new ODT
            append ODT entry
```

后续可再做：

- 地址连续变量合并
- Entry packing 优化
- 按数据宽度排序

第一版不需要。

---

# 10. 第四优先级：MeasurementSession

这是面向最终用户的高层 API。

## 10.1 职责

`MeasurementSession` 负责：

- 添加/删除变量
- 构建 MeasurementPlan
- 自动配置 DAQ
- 启动/停止 DAQ
- 接收 DTO
- 解码 MeasurementSample
- 执行 A2L conversion
- 管理时间戳
- 输出 MeasurementFrame

它不直接编码 XCP 命令。

## 10.2 推荐接口

```cpp
class MeasurementSession {
public:
    MeasurementSession(
        XcpMaster& master,
        IA2lDatabase& a2l);

    void Add(std::string_view name);
    void Remove(std::string_view name);
    void Clear();

    void Prepare();

    void Start(MeasurementCallback callback);
    void Stop();

    bool Running() const;
};
```

最终应用代码：

```cpp
MeasurementSession session(master, a2l);

session.Add("EngineSpeed");
session.Add("VehicleSpeed");
session.Add("Throttle");

session.Prepare();

session.Start([](const MeasurementFrame& frame) {
    // consume values
});
```

---

# 11. Measurement 数据模型

推荐统一数据模型：

```cpp
struct MeasurementSample {
    std::string name;

    std::chrono::nanoseconds timestamp;

    PhysicalValue value;

    Bytes raw;

    bool valid = true;
};
```

```cpp
struct MeasurementFrame {
    std::uint16_t daq_list = 0;
    std::uint8_t odt = 0;

    std::chrono::nanoseconds timestamp;

    std::vector<MeasurementSample> samples;
};
```

第一版建议每个 DTO 生成一个 `MeasurementFrame`。

以后再考虑：

- 按 Event Channel 聚合
- 按时间窗口聚合
- 多 DAQ List 合并

---

# 12. Timestamp 设计

Measurement-only 模式下，timestamp 是核心能力，不建议当成附属功能。

建议引入：

```cpp
struct DaqTimestamp {
    std::uint64_t raw = 0;
    std::chrono::nanoseconds value{0};
    bool valid = false;
};
```

必须考虑：

- 1-byte timestamp
- 2-byte timestamp
- 4-byte timestamp
- timestamp tick
- time unit
- wraparound

统一输出：

```text
std::chrono::nanoseconds
```

内部保留 raw timestamp，便于调试。

---

# 13. Polling Measurement 保留为第二路径

DAQ 应作为主路径，但 Polling 仍建议保留。

用途：

- ECU 不支持 DAQ
- Debug
- 少量低频变量
- DAQ 配置失败后的 fallback

架构：

```text
MeasurementSession
      │
      ├── DAQ Measurement
      │
      └── Polling Measurement
             │
             └── MemoryAccess
```

可提供：

```cpp
enum class MeasurementMode {
    Auto,
    Daq,
    Polling
};
```

`Auto` 策略：

1. 优先 DAQ
2. DAQ 不可用时 fallback 到 Polling

---

# 14. Seed & Key 保留最小接口

即使只做 Measurement，也不能删除 Seed & Key。

原因：ECU 可能锁定 DAQ Resource。

建议维持插件式设计：

```cpp
class ISeedKeyProvider {
public:
    virtual ~ISeedKeyProvider() = default;

    virtual Bytes CalculateKey(
        Resource resource,
        BytesView seed) = 0;
};
```

libxcp 本身不实现 OEM 算法。

---

# 15. A2L 第一阶段支持范围

Measurement-only 模式不需要完整 ASAP2。

建议优先保证：

```text
MEASUREMENT
COMPU_METHOD
COMPU_TAB
COMPU_VTAB
UNIT
MOD_COMMON
MOD_PAR
IF_DATA / XCP_ON_*
```

需要稳定获得：

- Name
- ECU Address
- Address Extension
- Data Type
- Byte Order
- Dimension
- Bit Mask
- Conversion
- Unit
- Event Channel / DAQ metadata

不属于第一阶段阻塞项：

- CHARACTERISTIC
- AXIS_PTS
- RECORD_LAYOUT 的完整 Calibration 语义
- FLASH layout

---

# 16. XcpMaster 职责边界

`XcpMaster` 应保持协议级 façade，而不是把 Measurement Planner 塞进去。

建议职责：

```text
XcpMaster
├── Connect / Disconnect
├── GetStatus
├── Unlock
├── ReadMemory
├── Query DAQ capability
├── Configure DAQ
├── Start / Stop DAQ
└── Event/DTO listener registration
```

不要让它直接提供：

```cpp
master.AddMeasurement("EngineSpeed");
```

这种 API 应属于 `MeasurementSession`。

---

# 17. 线程模型建议

建议采用至少三类逻辑线程：

```text
Transport RX Thread
       │
       ▼
CommandExecutor::OnPacketReceived
       │
       ├── RES/ERR → command wait queue
       │
       └── DTO     → DAQ queue
                         │
                         ▼
                  Measurement Worker
                         │
                         ▼
                  user callback
```

重要原则：

- Transport 接收线程禁止执行复杂 A2L conversion
- 用户 callback 不要运行在 transport RX thread
- DTO queue 必须有容量限制
- 队列溢出要统计 dropped DTO

建议增加：

```cpp
struct MeasurementStatistics {
    std::uint64_t dto_received;
    std::uint64_t dto_dropped;
    std::uint64_t decode_errors;
    std::uint64_t timestamp_wraps;
};
```

---

# 18. 错误模型

建议区分：

```text
TransportError
ProtocolError
XcpNegativeResponse
SessionError
DaqConfigurationError
DtoDecodeError
A2lError
MeasurementError
```

不要全部转换为一个通用 `runtime_error`。

MeasurementSession 可以选择：

- Fatal error → Stop
- Single DTO decode error → drop frame + statistics
- Event mismatch → warning

---

# 19. 建议新增目录

```text
include/libxcp/daq/
    dto_envelope_decoder.hpp
    dto_payload_decoder.hpp
    daq_timestamp.hpp

src/daq/
    dto_envelope_decoder.cpp
    dto_payload_decoder.cpp
    daq_timestamp.cpp

include/libxcp/measurement/
    measurement_planner.hpp
    measurement_session.hpp
    measurement_types.hpp

src/measurement/
    measurement_planner.cpp
    measurement_session.cpp
```

---

# 20. 推荐开发顺序

## Milestone M1：统一 DTO Decode

目标：删除 XCPlite 测试中的手工 `SplitEnvelope()`。

需要完成：

- DtoEnvelopeDecoder
- Absolute
- RelativeByte
- RelativeWord
- RelativeWordAligned
- Timestamp extraction
- DAQ/ODT routing

验收：

```text
XCPlite raw DTO
    ↓
DtoEnvelopeDecoder
    ↓
DaqLayout
    ↓
Decoded Measurement
```

全程无测试专用手工拆包。

---

## Milestone M2：DAQ Runtime Metadata

增加：

- GET_DAQ_EVENT_INFO
- Event runtime validation
- DAQ capability normalization

验收：

- 能查询 ECU Event Channel
- 能与 A2L Event Channel 比对
- 不一致时明确 warning

---

## Milestone M3：MeasurementPlanner

实现：

```text
变量名列表
    ↓
A2L lookup
    ↓
Event group
    ↓
ODT packing
    ↓
DaqListSpec[]
```

验收：

```cpp
planner.Build({"EngineSpeed", "VehicleSpeed"});
```

可以直接生成有效 DAQ 配置。

---

## Milestone M4：MeasurementSession

实现最终用户 API：

```cpp
session.Add(...);
session.Prepare();
session.Start(...);
session.Stop();
```

验收：

应用层不需要直接操作：

- ALLOC_DAQ
- ALLOC_ODT
- SET_DAQ_PTR
- WRITE_DAQ
- PID
- ODT index

---

## Milestone M5：Production Hardening

增加：

- Long-running DAQ test
- reconnect
- timeout recovery
- DTO overflow handling
- dropped packet statistics
- timestamp wraparound
- malformed DTO robustness
- sanitizer
- thread sanitizer

---

# 21. 最小 Measurement 命令集

建议第一版正式支持以下命令：

## Session

```text
CONNECT
DISCONNECT
GET_STATUS
SYNCH
GET_COMM_MODE_INFO
GET_ID
```

## Security

```text
GET_SEED
UNLOCK
```

## Polling

```text
SET_MTA
UPLOAD
SHORT_UPLOAD
```

## DAQ Information

```text
GET_DAQ_PROCESSOR_INFO
GET_DAQ_RESOLUTION_INFO
GET_DAQ_LIST_INFO
GET_DAQ_EVENT_INFO
```

## Dynamic DAQ

```text
FREE_DAQ
ALLOC_DAQ
ALLOC_ODT
ALLOC_ODT_ENTRY
SET_DAQ_PTR
WRITE_DAQ
```

## DAQ Control

```text
SET_DAQ_LIST_MODE
START_STOP_DAQ_LIST
START_STOP_SYNCH
```

可以暂不实现：

```text
DOWNLOAD
SHORT_DOWNLOAD
MODIFY_BITS
SET_CAL_PAGE
GET_CAL_PAGE
PROGRAM_*
STIM
```

---

# 22. 测试架构建议

建议形成四层测试：

## L1：Unit Test

```text
Codec
Parser
Session
DtoEnvelopeDecoder
Timestamp
MeasurementPlanner
A2L conversion
```

## L2：Mock Transport Test

测试：

- CTO timeout
- ERR response
- RES mismatch
- DTO interleaving
- reconnect

## L3：XCPlite E2E

重点测试：

- Connect
- Unlock（如可配置）
- Dynamic DAQ
- Relative PID mode
- timestamp
- variable changes
- Start/Stop

要求所有 XCPlite DTO 都走正式 decoder，禁止测试专用 `SplitEnvelope()`。

## L4：Real ECU Interop

最终至少覆盖：

- 一个 XCP on UDP ECU
- 不同 MAX_DTO
- 不同 Address Granularity
- 一个需要 Seed&Key 的 ECU
- 一个 Relative DAQ identification ECU

---

# 23. 建议增加的 CI 检查

```text
Linux GCC
Linux Clang
Windows MSVC
ASan
UBSan
TSan（至少 nightly）
```

后续增加：

```text
XCPlite E2E
```

可以作为可选 integration job。

---

# 24. 对当前架构的最终建议

当前代码已经从：

```text
Protocol Core
```

进入：

```text
可工作的 XCP Measurement Master 基础层
```

下一阶段不应该继续扩大命令覆盖，而应该收紧为：

```text
统一 DTO 解码
    ↓
DAQ Metadata
    ↓
MeasurementPlanner
    ↓
MeasurementSession
    ↓
生产级稳定性
```

最终形成清晰的层次：

```text
Application
    │
    ▼
MeasurementSession
    │
    ▼
MeasurementPlanner + A2L
    │
    ▼
DAQ Manager / Polling
    │
    ▼
XcpMaster
    │
    ▼
CommandExecutor
    │
    ▼
IXcpTransport
```

---

# 25. 第一版封板标准

建议满足下面条件后，将版本定义为：

> `libxcp Measurement v1`

必须满足：

- [x] UDP Transport
- [x] Connect / Disconnect
- [x] Polling Read
- [x] Seed & Key
- [x] Dynamic DAQ
- [x] DTO receive
- [x] A2L symbol lookup
- [x] Physical conversion
- [x] 通用 DTO Envelope Decoder（v0.2，`include/libxcp/daq/dto_envelope_*`）
- [x] GET_DAQ_EVENT_INFO（v0.3，CMD_UNKNOWN → nullopt 兜底）
- [x] MeasurementPlanner（v0.4，`include/libxcp/measurement/measurement_planner.hpp`）
- [x] MeasurementSession（v0.5，`include/libxcp/measurement/measurement_session.hpp`）
- [x] Timestamp 正式输出（v0.6，`DaqTimestampConverter` + 回卷外推 + 统计）
- [x] XCPlite E2E 使用正式 DTO decoder（v0.9，测试内手工拆包已全部删除）
- [x] 长时间 DAQ 稳定性测试（v1.0：UDP 回环 60s 长稳——≈5.7 万 DTO 零丢弃
      零解码错误，32 位时间戳 14 次回绕外推生效；真 ECU 长稳归 L4 手工清单，
      见 `libxcp_测量子系统_v0.9_记录.md` §6）

当上述剩余项完成后，可以认为：

> **Measurement-only 最小功能集已经真正闭环。**

此时才建议开始第二阶段：

```text
CAN / CAN FD Transport
```

而不是继续扩展 Calibration / Programming。

---

## 26. 推荐版本路线

```text
v0.2
DTO Envelope Decoder

v0.3
DAQ Event Runtime Info

v0.4
MeasurementPlanner

v0.5
MeasurementSession

v0.6
Timestamp + Statistics + Error hardening

v0.9
XCPlite + Real ECU interoperability

v1.0
Measurement-only XCP Master SDK
```

---

## 27. 总结

当前 `libxcp` 已经不需要大规模重构。

下一步最重要的是把已有的协议能力收敛为一个真正完整的 Measurement 数据通路：

```text
A2L Variable Name
        ↓
MeasurementPlanner
        ↓
Dynamic DAQ Configuration
        ↓
ECU DTO
        ↓
DtoEnvelopeDecoder
        ↓
ODT / Entry Decode
        ↓
A2L Conversion
        ↓
MeasurementSample
        ↓
MeasurementSession Callback
```

当这条链路完全成立后，`libxcp` 就已经具备一个实用的、可扩展的 C++ XCP Measurement Master SDK 核心能力。
