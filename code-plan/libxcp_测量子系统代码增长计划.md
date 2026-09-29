# libxcp 测量子系统代码增长计划（v0.2 → v1.0 全路线）

> 本文档是**代码增长计划**，不是最终设计文档。它是
> `code-plan/XCP_1.3.0_最小协议核心实现计划.md`（最小协议核心，已完成）与
> `code-plan/libxcp_measurement_architecture.md`（目标建议，v0.2–v1.0 路线）的
> 衔接：把"建议"落实为"有明确新增/修改文件、接口签名、验收标准和测试安排的
> 分步执行计划"。
>
> **硬性前置约束（AGENTS.md）**：设计方案经用户批准之前，禁止写任何代码。本计划
> 每一里程碑均需逐节确认后方可进入实现。

---

## 1. 范围与不变量

### 1.1 范围

- 目标形态：**Measurement-only XCP Master SDK**（Polling + DAQ → 物理值，经 A2L 换算）。
- 版本路线（与 `libxcp_measurement_architecture.md` §26 一致）：

```text
v0.2  DTO Envelope Decoder（统一信封解码）
v0.3  DAQ Event Runtime Info（GET_DAQ_EVENT_INFO）
v0.4  MeasurementPlanner（变量名 → DaqListSpec[] + 路由表）
v0.5  MeasurementSession（最终用户 API）
v0.6  Timestamp + Statistics + Error 硬化
v0.9  XCPlite + 真 ECU 互操作（删除测试专用手工拆包）
v1.0  Measurement-only SDK 封板
```

### 1.2 硬性不变量（每步不得违反）

| # | 不变量 | 依据 |
|---|--------|------|
| I1 | 核心树 `include/`+`src/` 不引用 A2L 栈（`liba2l`/`a2lbridge`/`calmcar::xcp::a2l` 字符串零命中） | `tests/a2l_isolation_check.cmake` S2 |
| I2 | 桥接层 `a2lbridge/include`+`src` 不直接包含上游 `a2l/` 头（`#include <a2l/`/`"a2l/` 零命中） | `tests/a2l_isolation_check.cmake` S1 |
| I3 | **不能修改** `thirdparty/` 下任何文件（含编译逻辑），除非用户单独明确指示 | AGENTS.md |
| I4 | **不重构**已完成代码；对已完成文件的改动仅限最小必要的**增量追加**（新增枚举值/新函数/新成员），不改既有签名与行为 | AGENTS.md |
| I5 | `XcpMaster` 保持协议级 façade，**不提供** `master.AddMeasurement(...)` | 架构文档 §16 |
| I6 | DTO 解码布局唯一权威 = 本端 `DaqLedgerEntry` 账本（实际下发顺序），不用 A2L 的 PREDEFINED 顺序 | 架构文档 B-6 |
| I7 | 解码器必须用 `DaqConfigGeneration()` 世代守卫防陈旧账本；timestamp 字节宽只用 `DaqTimestampBytesCached()` 的缓存值，**不猜** | `include/libxcp/xcp_master.hpp:332-419` |
| I8 | 核心测量类型不 `#include` 任何 `libxcp/a2l/*`；核心自持 `MeasurementValue`（镜像 `variant<int64,uint64,double,string,bool>`） | 用户澄清 m00190 |
| I9 | 一切新公共接口/成员须 doxygen 中文注释；新测试放在 `tests/`；每次代码变更后写 MD 记录到 `code-plan/` | AGENTS.md |

---

## 2. 现状盘点（已具备，直接复用）

- **命令与执行器**（`include/libxcp/command_executor.hpp`）：已具备
  `ExecuteSetDaqPtr(uint16_t,uint8_t,uint8_t)`(:231)、
  `ExecuteWriteDaq(bit_offset,size,ext,addr)`(:247)、
  `ExecuteClearDaqList`(:258)、`ExecuteFreeDaq`(:271)、
  `ExecuteAllocDaq(count)`(:281)、`ExecuteAllocOdt(list,count)`(:290)、
  `ExecuteAllocOdtEntry(list,odt,count)`(:297)、
  `ExecuteSetDaqListMode(...)`(:310)、`ExecuteStartStopDaqList`(:322)、
  `ExecuteStartStopSynch`(:330)、`ExecuteGetDaqListInfo`(:339)。
  **没有** `ExecuteGetDaqEventInfo` —— v0.3 缺口。
- **XcpMaster DAQ API**（`include/libxcp/xcp_master.hpp`）：
  `ConfigureDaqList`(:243)、`ConfigureDaqListsDynamic`(:267)、
  `StartDaqSync`(:279)、`FetchA2lViaUpload`(:296)、`StartDaqList`(:308)、
  `StopDaqList`(:315)、`StopDaq`(:323)、`ClearDaqList`(:327)、
  `DaqLedger()`(:330)、`DaqConfigGeneration()`(:333)、
  `QueryDaqProcessorInfo()`(:347)、`QueryDaqResolutionInfo()`(:355)、
  `QueryDaqListInfo(uint16_t)`(:365)、`ReadDaqEntryAt(...)`(:377)、
  `GetSessionParameters()`(:216)、私有 `DaqTimestampBytesCached()`(:419)、
  `ClearDynamicTableBestEffort()`(:423)。**没有** `GetDaqEventInfo` —— v0.3 缺口。
- **账本/代际/时间戳预检**：`DaqLedgerEntry`（`include/libxcp/xcp_master.hpp:92-103`，
  含 `optional<uint8_t> pid = FIRST_PID + 相对ODT号`）；`Session::DaqConfigGeneration()`/
  `BumpDaqConfigGeneration()`（`include/libxcp/session.hpp:100-194`）；
  timestamp 字节计入信封预检（`ConfigureDaqListsDynamic` 对单 ODT 超 MAX_DTO 的 G3 修正）。
- **基础类型**（`include/libxcp/protocol_types.hpp`）：`Bytes=std::vector<uint8_t>`、
  `BytesView=std::span<const uint8_t>`(:23-48)；`CommandCode` 枚举(:63-102，
  注释 :97 明确指出 **0xD7=GET_DAQ_EVENT_INFO 刻意未收录**)；
  `PacketType{Res,Err,Ev,Serv}`(:110-115)、`ClassifyPacket`(:124)、
  `kDtoPidMax=0xFB`(:139)、`kDaqBitOffsetNone=0xFF`(:144)、
  `DaqListModeBit`(:165-172)、`ByteOrder{Intel=0,Motorola=1}`(:342)、
  `AddressGranularity{Byte=1,Word=2,DWord=4}`(:352)、`AgToBytes`(:359)、
  `SessionParameters`（含 `ByteOrder byte_order`、`AddressGranularity address_granularity`、
  `max_dto`）。注意：核心字节序枚举值是 **Intel/Motorola**，A2L 侧是
  **MsbLast/MsbFirst**，二者是不同命名空间的独立类型，仅在适配器内显式映射。
- **XcpMaster 事件入口**：`IEventListener::OnDto`（`include/libxcp/command_executor.hpp:50-67`）
  接收**完整 DTO 帧**；`DtoPacket.data[0]==pid`（`include/libxcp/response_parser.hpp:67-70`）。
  测量会话经此入口拿 DTO，不另开 Transport 监听。
- **A2L 栈（只读消费，不改）**：
  `calmcar::xcp::a2l::IA2lDatabase`（`Find/Search/Count/ByteSizeOf/ToPhysical/FromPhysical`，
  `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/ia2l_database.hpp:24-80`）；
  `PhysicalValue = variant<int64_t,uint64_t,double,string,bool>`（`a2l_types.hpp:111-112`）；
  `IDaqLayout::Decode(const DtoFrameLayout&, BytesView) → Result<...>`+
  `Generation() const noexcept`（`daq_layout.hpp:289-327`）；
  `A2lBridge::Load(...)` 与 `Database()/XcpInfo()/CompareWithRuntime(...)`
  （`a2l_bridge.hpp`）。
- **测试基建**：GTest v1.15.2（FetchContent / `.deps-cache/googletest`）；
  `tests/CMakeLists.txt` 的 `libxcp_tests` 与 `XcpliteIntegration`；ctest
  `A2lIsolation`（`tests/CMakeLists.txt:220-224` 调 `a2l_isolation_check.cmake`）。
- **测试专用遗留（v0.9 删除）**：`tests/xcplite_daq_test.cpp:135-166` 的
  `EnvelopeView` + `SplitEnvelope(const Bytes&)` 手工拆 `[ODTrel][0xAA][DAQ16]`(+首 ODT 4B ts)，
  是 v0.2 要统一掉的"测试专用手工拆包"。

---

## 3. 目标数据通路（本计划收敛后）

```text
A2L 变量名（"EngineSpeed"...）
        ↓ IMeasurementDatabase 查询（地址/宽度/换算）
MeasurementPlanner
        ↓ 按 Event 分组 + ODT 装箱（尊重 MAX_DTO/AG/时间戳头）
MeasurementPlan{ DaqListSpec[] + MeasurementRoute[] }
        ↓
XcpMaster::ConfigureDaqListsDynamic + StartDaqSync
        ↓
ECU DTO（IEventListener::OnDto 完整帧）
        ↓
DtoEnvelopeDecoder（v0.2，A2L-free；布局来自账本+代际）
        ↓
ODT Router（账本 (daq_list,odt) → 有序 Entry）
        ↓
净荷切片（按 Entry.size 与 AG；v0.4 路由表绑定 name）
        ↓
IMeasurementDatabase::ToPhysical(name, raw)（适配器内桥接 a2l）
        ↓
MeasurementSample → MeasurementFrame → MeasurementSession callback
```

---

## 4. 关键架构决策

### 4.1 依赖方向：核心窄接口 + 桥接适配器（用户已确认 m00190）

- 测量代码落在 **libxcp 核心** `include/libxcp/measurement/*` + `src/measurement/*`，
  命名空间仍为 `calmcar::xcp`。
- 核心只依赖一个**窄接口** `calmcar::xcp::IMeasurementDatabase`（见 §5.1）与核心自有
  值类型 `MeasurementValue`，**绝不**使用 `calmcar::xcp::a2l::PhysicalValue` /
  `calmcar::xcp::a2l::Bytes`。
- 桥接侧新增**适配器**，把 `calmcar::xcp::a2l::IA2lDatabase` 包装成
  `calmcar::xcp::IMeasurementDatabase`。

### 4.2 适配器物理落点（开放决策，需确认）

约束冲突：适配器语义上属于"桥接侧"（必须知道 A2L 类型），但 AGENTS.md 禁止改动
`thirdparty/`，而 S2 门禁又扫描主树 `include/`+`src/` 中 `calmcar::xcp::a2l` 等字符串，
因此**适配器不能放进 `include/`/`src/`，也不能改 `thirdparty/a2l-sdk` 的构建脚本**。

推荐方案（本文档默认）：

- 新建**独立顶层目录** `adapter/a2l/`（在 D:\project\libxcp 下，**不在** `include/` 与
  `src/` 内，S2 的 `GLOB_RECURSE "${LIBXCP_ROOT_DIR}/include/*"`/`src/*` 扫不到它）：
  - `adapter/a2l/a2l_measurement_database.hpp`
  - `adapter/a2l/a2l_measurement_database.cpp`
- 新增**独立 CMake target** `libxcp_measurement_adapter`（STATIC），仅在
  `LIBXCP_BUILD_A2L=ON` 时构建；`target_link_libraries(libxcp_measurement_adapter
  PUBLIC libxcp::libxcp INTERFACE libxcp::a2lbridge)`。副产物不并入 `libxcp` 静态库，
  保证 `libxcp` 目标自身零 A2L 耦合。
- 包含方向：`a2l_measurement_database.cpp` 仅包含
  `<libxcp/measurement/*.hpp>`（核心）与 `<libxcp/a2l/ia2l_database.hpp>`（桥接）；
  **核心 `include/libxcp/*` 与 `src/measurement/*` 永不包含 `libxcp/a2l/*`**。
  S1/S2 均保持 0 命中（S1 只禁上游 `a2l/` 直含；S2 不扫 `adapter/`）。

备选方案（需用户单独许可改 `thirdparty`）：把适配器加入
`thirdparty/a2l-sdk/a2lbridge` 的源码与 `build-sdk.ps1`。**本文档默认不选**。

> ⚠️ 在进入 v0.5 前，必须向用户确认 §4.2 的推荐落点，否则不实现适配器。

### 4.3 净荷解码：账本切片 + 适配器换算（核心不进 IDaqLayout）

- 动态测量路径的 ODT/Entry 边界来自实时账本 + 路由表，核心无需桥接层的
  `IDaqLayout`（后者面向 PREDEFINED/结构体按 A2L 记录布局解码）。
- 核心 `DtoPayloadDecoder` 只做字节级切片，物理换算统一走 `IMeasurementDatabase`。
- 这样 v0.2 的核心解码器完全 A2L-free，v0.4 引入 `IMeasurementDatabase` 后仍不接触
  A2L 头文件。

### 4.4 错误模型：测量层独立错误码，不重构 `ErrorCategory`

- 现役 `ErrorCategory`（`include/libxcp/xcp_error.hpp:26-35`）只到
  `TransportError/ProtocolError/...RecoveryFailed`，**不新增成员**（I4）。
- 测量层新增 `include/libxcp/measurement/measurement_result.hpp`：
  - `enum class MeasurementErrorCode`：`NotFound / AmbiguousName /
    RawSizeMismatch / UnsupportedDataType / UnsupportedConversion /
    InvalidLayout / DaqConfigurationError / DtoDecodeError / Busy / NotRunning / Fatal`。
  - `template<class T> class MeasurementResult`（与 `a2l_result.hpp` 的 Result 同风格，
    但核心自持，不依赖 A2L）。
- 分类原则（架构文档 §18）：单帧 DTO 解码失败 → drop 该帧 + 统计；Event 不一致 →
  warning；fatal（断开/配置失败/代际失配）→ 走 `XcpException` 上抛并 `Stop()`。

---

## 5. 拟新增公共接口（汇总，签名即验收依据）

### 5.1 核心类型与窄接口（v0.2–v0.4 逐步落地）

```cpp
// include/libxcp/measurement/measurement_types.hpp —— 核心测量值类型
namespace calmcar::xcp {

/// @brief 测量物理值（镜像 A2L PhysicalValue 的五选一；核心不依赖 a2l 类型）
using MeasurementValue =
    std::variant<std::int64_t, std::uint64_t, double, std::string, bool>;

}  // namespace calmcar::xcp
```

```cpp
// include/libxcp/daq/dto_envelope_types.hpp —— 统一信封（v0.2，A2L-free）
namespace calmcar::xcp {

/// @brief 识别字段类型（四种 PID/Identification 模式，核心自持枚举）
enum class IdentificationFieldType : std::uint8_t {
    Absolute,             ///< 绝对 ODT 号（PID = FIRST_PID + 相对 ODT）
    RelativeByte,         ///< 相对 ODT 号 + 扩展 DAQ 字节(0xAA)
    RelativeWord,         ///< 相对 ODT 号 + 扩展 DAQ WORD(0xAA03 LE)
    RelativeWordAligned,  ///< 同上但 4 字节对齐
};

/// @brief 运行时信封布局（v0.2 只依赖账本/会话参数，不依赖 A2L）
struct DtoFrameLayout {
    IdentificationFieldType identification_field_type = IdentificationFieldType::Absolute;
    std::uint8_t first_odt = 0;   ///< Absolute 模式 FIRST_PID 回填换算用
    bool counter_enabled = false; ///< DTO 计数器位（DaqListModeBit::kDtoCounter）
    bool timestamp_enabled = false;          ///< 时间戳位（kTimestamp）
    std::uint8_t timestamp_size_bits = 0;    ///< 时间戳位宽（bit，来自分辨率取证，不猜）
    bool overflow_indicator = false;         ///< OVERLOAD_INDICATOR
    bool pid_off = false;                    ///< 无识别字段（解码侧拒绝，B-7）
    std::size_t header_bytes = 1;            ///< 含时间戳在内的信封头长（预检口径）
};

struct DtoIdentity {
    std::uint16_t daq_list = 0;
    std::uint8_t odt = 0;
};

struct DtoEnvelope {
    DtoIdentity identity;
    std::optional<std::uint8_t> counter;
    std::optional<std::uint64_t> raw_timestamp;
    BytesView payload;  ///< 指向 dto 内的净荷段
};

}  // namespace calmcar::xcp
```

```cpp
// include/libxcp/daq/dto_envelope_decoder.hpp —— 统一信封解码（v0.2）
namespace calmcar::xcp {

class DtoEnvelopeDecoder {
public:
    /// @brief 按运行时布局切出信封；布局与帧不符或识别字段非法时抛
    ///        XcpException(MalformedPacket)（B-7 pid_off 直接拒绝）
    [[nodiscard]] DtoEnvelope Decode(BytesView dto,
                                     const DtoFrameLayout& frame_layout) const;
};

}  // namespace calmcar::xcp
```

```cpp
// include/libxcp/measurement/measurement_database.hpp —— 核心窄接口（v0.4）
namespace calmcar::xcp {

/// @brief 核心可见的只读测量符号视图（窄：只取测量路径需要的最小集）
struct MeasurementSymbolInfo {
    Address address{0};
    AddressExtension extension{0};
    std::uint16_t event_channel{0};  ///< 来自 A2L Event（仅用于规划分组）
    std::uint8_t element_size_bytes{0};  ///< 单元素实占字节（B-1 口径，不除 AG）
    std::uint8_t element_count{1};       ///< 元素数（规划用；首版仅标量=1）
};

class IMeasurementDatabase {
public:
    virtual ~IMeasurementDatabase() = default;
    IMeasurementDatabase(const IMeasurementDatabase&) = delete;
    IMeasurementDatabase& operator=(const IMeasurementDatabase&) = delete;

    /// @brief name 可为全库唯一裸名或规范键（歧义→AmbiguousName）
    [[nodiscard]] virtual MeasurementResult<MeasurementSymbolInfo> Find(
        std::string_view name) const = 0;

    /// @brief raw 字节 → 物理值（长度不符 → RawSizeMismatch）
    [[nodiscard]] virtual MeasurementResult<MeasurementValue> ToPhysical(
        std::string_view name, BytesView raw) const = 0;
};

}  // namespace calmcar::xcp
```

### 5.2 GET_DAQ_EVENT_INFO（v0.3）

```cpp
// include/libxcp/protocol_types.hpp —— 增量追加枚举值（I4：只加不改）
// CommandCode::GetDaqEventInfo = 0xD7   （现注释 :97 的"刻意未收录"改为已收录说明）
```

```cpp
// include/libxcp/response_parser.hpp —— 增量追加
struct GetDaqEventInfoResponse {
    std::uint16_t event_channel{0};
    std::uint8_t properties{0};
    std::uint8_t max_daq_lists{0};
    std::uint8_t time_cycle{0};
    std::uint8_t time_unit{0};
    std::uint8_t priority{0};
};
```

```cpp
// include/libxcp/command_executor.hpp —— 增量追加（与 ExecuteGetDaqListInfo 对齐）
[[nodiscard]] GetDaqEventInfoResponse ExecuteGetDaqEventInfo(
    std::uint16_t event_channel);
```

```cpp
// include/libxcp/xcp_master.hpp —— 增量追加（薄转发，零策略，同 QueryDaq* 口径）
[[nodiscard]] std::optional<GetDaqEventInfoResponse> GetDaqEventInfo(
    std::uint16_t event_channel);
// Slave 回 ERR_CMD_UNKNOWN 时返回 nullopt；其余错误如实抛出
```

> A2L-vs-runtime 比对（v0.3 验收）：`GetDaqEventInfo()` 结果与桥接侧
> `IfDataXcpInfo.event_channels`（`if_data_xcp.hpp` 内的 `EventChannelInfo`）比对；
> 不一致输出 **warning**（复用 `ParamDiscrepancy` 展示），**不阻断**。

### 5.3 规划器与路由（v0.4）

```cpp
// include/libxcp/measurement/measurement_planner.hpp
namespace calmcar::xcp {

struct MeasurementRoute {
    std::string name;                 ///< 测量名（回调样本用的键）
    std::uint16_t daq_list;           ///< DAQ List 号
    std::uint8_t odt;                 ///< ODT 号（0 基）
    std::size_t payload_offset;       ///< 净荷内字节偏移
    std::size_t size;                 ///< 实占字节数
};

class MeasurementPlanner {
public:
    /// @param database 核心窄接口视图（适配器提供）
    /// @param max_dto  会话 MAX_DTO（来自 GetSessionParameters().max_dto）
    /// @param address_granularity 会话 AG（来自 GetSessionParameters()）
    /// @param timestamp_bytes DaqTimestampBytesCached() 缓存（0=无/未知）
    explicit MeasurementPlanner(const IMeasurementDatabase& database,
                                std::uint16_t max_dto,
                                AddressGranularity address_granularity,
                                std::size_t timestamp_bytes = 0);
    ~MeasurementPlanner();

    /// @brief 变量名列表 → 完整规划（分组/装箱/路由一次成型）
    [[nodiscard]] MeasurementResult<MeasurementPlan> Build(
        const std::vector<std::string>& names) const;
};

struct MeasurementPlan {
    std::vector<DaqListSpec> daq_lists;   // 直接可喂 ConfigureDaqListsDynamic
    std::vector<MeasurementRoute> routes; // ODT Router 绑定 name 用
};
}  // namespace calmcar::xcp
```

装箱策略（架构文档 §9，确定性、不最优）：

1. 按 `event_channel` 分组（`0=不由通道触发` 单独成组）；
2. 保持用户变量顺序；
3. 顺序填充当前 ODT；单条目入门即超 `max_dto - timestamp_bytes` 时报 `DaqConfigurationError`；
4. 超限则开新 ODT；一个变量不跨 ODT；一个 Entry = 一个 Measurement；
5. 需要多 ODT/多 List 时校验 Slave 容量（`QueryDaqProcessorInfo`/`QueryDaqListInfo` 结果）。

### 5.4 数据模型与时间戳（v0.5/v0.6）

```cpp
// include/libxcp/daq/daq_timestamp.hpp —— v0.6（先以 v0.5 直传 raw 起步）
namespace calmcar::xcp {

struct DaqTimestamp {
    std::uint64_t raw = 0;               ///< 原始计数（取自信封 raw_timestamp）
    std::chrono::nanoseconds value{0};   ///< 换算后的单调纳秒
    bool valid = false;                  ///< 无时间戳位或换算失败为 false
};

/// @brief 原始计数 → 纳秒；处理 1/2/4B 位宽、tick、unit 与环绕回卷
class DaqTimestampConverter {
public:
    /// @param unit_ns          时间单位（纳秒，来自 A2L/运行时取证）
    /// @param bits             位宽（1/2/4，来自 DtoFrameLayout.timestamp_size_bits）
    /// @param dto_counter_ns   无时间戳位时按 DTO 计数器/事件周期兜底（可选）
    [[nodiscard]] DaqTimestamp Convert(std::uint64_t raw, std::uint64_t prev_raw) const;
};

}  // namespace calmcar::xcp
```

```cpp
// include/libxcp/measurement/measurement_sample.hpp —— v0.5
namespace calmcar::xcp {

struct MeasurementSample {
    std::string name;
    std::chrono::nanoseconds timestamp;
    MeasurementValue value;
    Bytes raw;
    bool valid = true;  ///< 该样本换算/解码失败时为 false，不整体丢弃帧
};

struct MeasurementFrame {
    std::uint16_t daq_list = 0;
    std::uint8_t odt = 0;
    std::chrono::nanoseconds timestamp;
    std::vector<MeasurementSample> samples;
};

using MeasurementCallback = std::function<void(const MeasurementFrame&)>;

}  // namespace calmcar::xcp
```

### 5.5 MeasurementSession（v0.5）

```cpp
// include/libxcp/measurement/measurement_session.hpp
namespace calmcar::xcp {

class MeasurementSession {
public:
    /// @param master 已有 XcpMaster（须已 Connect；生命周期须长于本对象）
    /// @param database IMeasurementDatabase 视图（适配器提供；生命周期须长于本对象）
    MeasurementSession(XcpMaster& master, IMeasurementDatabase& database);
    ~MeasurementSession();  // 尽力 Stop + 撤销配置（复用 StopDaq/Clear 收尾）

    MeasurementSession(const MeasurementSession&) = delete;
    MeasurementSession& operator=(const MeasurementSession&) = delete;

    void Add(std::string_view name);       // 追加待测变量（Prepare 前可改）
    void Remove(std::string_view name);
    void Clear();

    /// @brief 规划 + ConfigureDaqListsDynamic（配置期记 DaqConfigGeneration）
    void Prepare();

    /// @brief 启动 DAQ 并注册回调；DTO 在内部 worker 解码后回调（非 RX 线程）
    void Start(MeasurementCallback callback);
    void Stop();

    [[nodiscard]] bool Running() const;

    [[nodiscard]] const MeasurementStatistics& Statistics() const;
};
}  // namespace calmcar::xcp
```

> 说明：`MeasurementSession(XcpMaster&, IMeasurementDatabase&)` 是架构文档 §10.2 的
> `IA2lDatabase&` 按 m00190 决策**改名**后的形态，其余语义不变。最终应用码不变：

```cpp
MeasurementSession session(master, a2l_adapter);
session.Add("EngineSpeed");
session.Add("VehicleSpeed");
session.Prepare();
session.Start([](const MeasurementFrame& frame) { /* consume */ });
```

### 5.6 统计（v0.5 起步、v0.6 硬化）

```cpp
// include/libxcp/measurement/measurement_session.hpp
struct MeasurementStatistics {
    std::uint64_t dto_received{0};
    std::uint64_t dto_dropped{0};      // 有界队列溢出
    std::uint64_t decode_errors{0};    // 单帧解码失败（drop 计数）
    std::uint64_t timestamp_wraps{0};  // 时间戳回卷次数
};
```

---

## 6. 分步执行计划

> 每步格式：目标 → 新增/修改文件（精确路径）→ 接口动作 → 验收标准 → 测试（L1-L4）→
> 记录要求。每步完成后按 AGENTS.md 在 `code-plan/` 写 `libxcp_测量子系统_<版本>_记录.md`。

---

### v0.2 —— 统一 DTO Envelope Decoder

**目标**：把四种识别模式统一成一个 A2L-free 内核解码器；XCPlite 手工拆包必须与其
等价（正式的删除动作放到 v0.9 收口）。

**新增文件**：

| 路径 | 内容 |
|------|------|
| `include/libxcp/daq/dto_envelope_types.hpp` | `IdentificationFieldType` / `DtoFrameLayout` / `DtoIdentity` / `DtoEnvelope`（§5.1） |
| `include/libxcp/daq/dto_envelope_decoder.hpp` | `DtoEnvelopeDecoder` 声明（§5.1） |
| `src/daq/dto_envelope_decoder.cpp` | 四种模式解析 + 时间戳/计数器/溢出提取 + 边界校验 |
| `tests/dto_envelope_decoder_test.cpp` | L1 单测 |

**修改文件**（增量，不重构）：

- `CMakeLists.txt`：`target_sources(libxcp PRIVATE ... src/daq/dto_envelope_decoder.cpp)`；
- `tests/CMakeLists.txt`：`libxcp_tests` 源列表追加 `dto_envelope_decoder_test.cpp`。

**接口动作**：纯新增（`calmcar::xcp` 内新头/新类），不改任何既有签名。

**核心规则（实现时的硬核对项）**：

- 布局不来自 A2L —— 来自会话参数（`IdentificationFieldType` 的原码取
  `QueryDaqProcessorInfo()` 的 DAQ_KEY_BYTE bit6-7 所对应运行时真值）与账本回填的
  `pid`（Absolute 模式）；`timestamp_size_bits` 用 `DaqTimestampBytesCached()`。
- `pid_off=true` 直接抛 `XcpException(MalformedPacket)`（B-7）。
- 首 ODT 时间戳字节计入 `header_bytes`（XCPlite 实然：每事件首 ODT 带 4B ts）。
- 帧短于最小头/识别字段不合法 → `MalformedPacket`，不允许部分解释。

**验收标准**：

```text
Raw DTO → DtoEnvelopeDecoder → DtoEnvelope{identity, counter?, raw_timestamp?, payload}
```
- 四种模式 Absolute / RelativeByte / RelativeWord / RelativeWordAligned 各有 golden 字节序列通过；
- 与 `tests/xcplite_daq_test.cpp` 既有 XCPlite 帧口径一致（`[ODTrel][0xAA][DAQ16 LE]` + 首 ODT 4B ts）。

**测试**：

- L1 `dto_envelope_decoder_test.cpp`：四模式、带/不带 counter、带/不带 timestamp、
  pid_off 拒绝、帧过短、payload 截取正确、`header_bytes` 计算正确。
- L2 门禁：`A2lIsolation` 仍 S1=0/S2=0（本次新增核心文件不得含 a2l 字符串）。

**记录**：`code-plan/libxcp_测量子系统_v0.2_记录.md`。

---

### v0.3 —— DAQ Event Runtime Info（GET_DAQ_EVENT_INFO）

**目标**：补齐第四类 DAQ 信息查询，支撑 A2L Event vs ECU 运行时一致性校验
（warning 不 fail）。

**新增文件**：

| 路径 | 内容 |
|------|------|
| `tests/get_daq_event_info_test.cpp` | L2 mock-transport 单测 |

**修改文件**（增量追加）：

| 路径 | 动作 |
|------|------|
| `include/libxcp/protocol_types.hpp` | `CommandCode` 追加 `GetDaqEventInfo = 0xD7`，更新 :97 注释 |
| `include/libxcp/response_parser.hpp` | 追加 `GetDaqEventInfoResponse` + `ParseGetDaqEventInfoResponse` |
| `include/libxcp/command_codec.hpp/.cpp`（`src/command_codec.cpp`） | CRO 编码（`[D7][event_channel WORD LE]`） |
| `include/libxcp/command_executor.hpp` + `src/command_executor.cpp` | 追加 `ExecuteGetDaqEventInfo(uint16_t) → GetDaqEventInfoResponse` |
| `include/libxcp/xcp_master.hpp` + `src/xcp_master.cpp` | 追加 `GetDaqEventInfo(uint16_t) → optional<GetDaqEventInfoResponse>`（薄转发；按 `QueryDaqListInfo` 口径 ERR_CMD_UNKNOWN→nullopt） |
| `include/libxcp/response_parser.hpp` + `src/response_parser.cpp` |（同上合并项）|

**响应编码硬核对项**（写前读 `command_executor.hpp` 现役 Execute 模式再照抄）：

- 按规范 GET_DAQ_EVENT_INFO RES：`[D7][event_channel(2)][properties][max_daq_lists][time_cycle][time_unit]`，
  可选 `priority` 仅在 EXTENDED 事件信息存在；`time_unit`/`time_cycle` 单位语义写入 doxygen。
- XCPlite 实然行为以 `xcp.c` 实现为准（实现前 grep 取证；若 XCPlite 回
  `CRC_CMD_SYNTAX/CMD_UNKNOWN`，本方法仍按 nullopt 返回，测试据此断言）。

**验收标准**：

```text
GetDaqEventInfo(ev) 成功 → 六字段结构；Slave 无此命令 → nullopt（不抛）
A2L if_data(EventChannelInfo) vs 该结果 → 有差异输出 warning（不 fail）
```

**测试**：

- L2 `get_daq_event_info_test.cpp`：mock transport 回 `[D7]...` 正常解析；回 ERR→nullopt；
  CRO 字节序列断言；`GetDaqEventInfo(ev)` 透传参数正确。
- L3（若 `LIBXCP_BUILD_XCPLITE_SLAVE=ON`）：`xcplite_daq_test.cpp` 增补事件信息查询段。

**记录**：`code-plan/libxcp_测量子系统_v0.3_记录.md`。

---

### v0.4 —— MeasurementPlanner

**目标**：`names[] → MeasurementPlan{DaqListSpec[]+MeasurementRoute[]}`，自动规划 DAQ 配置。

**新增文件**：

| 路径 | 内容 |
|------|------|
| `include/libxcp/measurement/measurement_types.hpp` | `MeasurementValue`（§5.1） |
| `include/libxcp/measurement/measurement_result.hpp` | `MeasurementErrorCode` + `MeasurementResult<T>`（§4.4） |
| `include/libxcp/measurement/measurement_database.hpp` | `MeasurementSymbolInfo` + `IMeasurementDatabase`（§5.1） |
| `include/libxcp/measurement/measurement_planner.hpp` | `MeasurementRoute` / `MeasurementPlan` / `MeasurementPlanner`（§5.3） |
| `src/measurement/measurement_planner.cpp` | 分组/装箱/路由实现（§5.3 策略） |
| `tests/measurement_planner_test.cpp` | L1 fake-database 单测 |

**修改文件**：

- `CMakeLists.txt`：`target_sources(libxcp PRIVATE ... src/measurement/measurement_planner.cpp)`；
- `tests/CMakeLists.txt`：`libxcp_tests` 追加 `measurement_planner_test.cpp`。

**实现硬核对项**：

- MAX_DTO 上限取 `XcpMaster::GetSessionParameters().max_dto`（`SessionParameters`，
  `include/libxcp/protocol_types.hpp:492` 起）；AG 取 `address_granularity`
  （核心 `AddressGranularity`，`AgToBytes`）；timestamp 头字节取
  `DaqTimestampBytesCached()`（0=无/未知，不猜）。
- `DaqEntrySpec.size` 是**以 AG 为单位的元素数**；`IO` 字节→AG 换算必须在规划器内做，
  且单个变量跨 ODT/非标量/`ElementSizeOf==0` 类型一律 `DaqConfigurationError`（核心
  与 a2l 的 `ElementSizeOf` 同规则：无固定宽度→0→报错，B-3）。
- 输出 `daq_lists` 的 `daq_list` 按 `0..N-1` 顺序（`ConfigureDaqListsDynamic` 硬门槛，
  `xcp_master.hpp:249-266`）；识别字段按运行时取证选型（RelativeWord 优先，据
  `QueryDaqProcessorInfo` 的 DAQ_KEY_BYTE）。

**验收标准**：

```cpp
planner.Build({"EngineSpeed", "VehicleSpeed"})  // ok → MeasurementPlan
```
- 生成的 `DaqListSpec[]` 可直接喂 `ConfigureDaqListsDynamic` 且不触 max_dto/timestamp 预检失败；
- `routes[]` 与 `daq_lists` 的 (daq_list, odt) 及净荷偏移一一对应、可回查 name。

**测试**：

- L1 `measurement_planner_test.cpp`：分组正确、顺序保持、恰好超限开新 ODT、单变量
  超 MAX_DTO 报错、AG 换算、timestamp 头扣除、跨 ODT 变量拒绝、空名/未知名/
  歧义名报错、多 Event 多 List。
- L2 门禁 `A2lIsolation`：新增核心文件零 a2l 字符串（S2=0）。

**记录**：`code-plan/libxcp_测量子系统_v0.4_记录.md`。

---

### v0.5 —— MeasurementSession（含桥接适配器）

**目标**：落地最终用户 API（Add/Prepare/Start(cb)/Stop）；打通"信封→切片→A2L 换算→
帧回调"全链路。**执行本步前必须先与用户确认 §4.2 适配器落点。**

**新增文件**：

| 路径 | 内容 |
|------|------|
| `include/libxcp/measurement/measurement_sample.hpp` | `MeasurementSample`/`MeasurementFrame`/`MeasurementCallback`（§5.4，v0.5 先直传 raw 时间戳） |
| `include/libxcp/measurement/measurement_session.hpp` | `MeasurementSession` + `MeasurementStatistics`（§5.5/§5.6） |
| `src/measurement/measurement_session.cpp` | 内部 DTO 队列/worker/路由/切片/换算/回调 |
| `src/measurement/dto_payload_decoder.cpp` + `include/libxcp/daq/dto_payload_decoder.hpp` | 账本切片解码（核心，不碰 A2L） |
| `adapter/a2l/a2l_measurement_database.hpp` | `A2lMeasurementDatabase : calmcar::xcp::IMeasurementDatabase`（包装 `calmcar::xcp::a2l::IA2lDatabase`） |
| `adapter/a2l/a2l_measurement_database.cpp` | `Find`/`ToPhysical` 的 a2l→core 映射（含字节序枚举显式映射：`a2l::ByteOrder::MsbLast ↔ core::ByteOrder::Intel`） |
| `tests/measurement_session_test.cpp` | L2 mock-transport 集成测试 |
| `tests/measurement_adapter_test.cpp` | 适配器单测（`LIBXCP_BUILD_A2L=ON` 时，fake `IA2lDatabase`） |

**修改文件**：

- `CMakeLists.txt`：追加 `src/measurement/measurement_session.cpp`、
  `src/measurement/dto_payload_decoder.cpp`；新增 `if(LIBXCP_BUILD_A2L)` 段定义
  `libxcp_measurement_adapter`（源于 `adapter/a2l/*.cpp`，链 `libxcp::libxcp`+
  `libxcp::a2lbridge`；不并入 `libxcp` 目标）；
- `tests/CMakeLists.txt`：追加两个测试源；适配器测试仅在 `LIBXCP_BUILD_A2L` 下加入。

**实现硬核对项**：

- DTO 入口复用 `IEventListener::OnDto`（完整帧）；`MeasurementSession` 内部实现该监听，
  RX 线程只入队（有界，溢出计数 `dto_dropped`）；worker 线程解码+换算+回调
  （架构文档 §17：回调不在 RX 线程）。
- 解码布局只取账本：`Start(Prepare)` 时快照 `DaqLedger()` + 记
  `DaqConfigGeneration()`；每帧先核对代际，失配 → 重建路由/解码器（B-6/F4 同口径，
  `daq_layout.hpp:316-326` 的"一行可写检查"移入核心 ODT Router）。
- 换算走 `IMeasurementDatabase::ToPhysical(name, raw)`（适配器内转
  `IA2lDatabase::ToPhysical`），失败仅该样本 `valid=false` + `decode_errors++`。
- `Prepare()` 内部：`MeasurementPlanner::Build` → `ConfigureDaqListsDynamic`；
  `Start()`：`StartDaqSync()`；`Stop()`/析构：`StopDaq()` + `ClearDaqList`/复位路由。
- `Running()` 与既有 `Session` 运行态不冲突；不新增 `XcpMaster` 成员/方法（I5）。

**验收标准**：

```text
session.Add → Prepare → Start(cb) → 收 DTO → cb(MeasurementFrame{...samples 含物理值})
应用层不再直接操作 ALLOC_DAQ/ALLOC_ODT/SET_DAQ_PTR/WRITE_DAQ/PID/ODT index（§M4）
```

**测试**：

- L2 `measurement_session_test.cpp`（mock transport 回放 DTO 序列）：Add/Remove/Clear、
  Prepare 失败回滚、Start 后帧回调、样本物理值正确、`valid=false` 单样本降级、
  队列溢出统计、Stop/析构收尾、代际失配重建。
- L2 `measurement_adapter_test.cpp`（A2L 开时）：`Find`/`ToPhysical` 映射正确、
  字节序/AG 显式映射表正确、歧义名与尺寸不符错误透传。

**记录**：`code-plan/libxcp_测量子系统_v0.5_记录.md`。

---

### v0.6 —— Timestamp + Statistics + Error 硬化

**目标**：把 v0.5 的直传时间戳升级为回卷安全的纳秒时间；补齐统计与 malformed DTO 鲁棒性。

**新增文件**：

| 路径 | 内容 |
|------|------|
| `include/libxcp/daq/daq_timestamp.hpp` | `DaqTimestamp` + `DaqTimestampConverter`（§5.4） |
| `src/daq/daq_timestamp.cpp` | 1/2/4B 位宽 + tick/unit + 回卷换算实现 |
| `tests/daq_timestamp_test.cpp` | L1 时间戳单测 |
| `tests/measurement_session_hardening_test.cpp` | L2 硬化测试 |

**修改文件**：

- `src/measurement/measurement_session.cpp`：接入 `DaqTimestampConverter`、统一统计埋点；
- `CMakeLists.txt` 与 `tests/CMakeLists.txt`：追加上述源。

**实现硬核对项**：

- 位宽来源 `DtoFrameLayout.timestamp_size_bits`（v0.2 解析；XCPlite 实然 4B）；
  unit 取值来自 A2L/运行时取证，0/未知 → `valid=false` 但保留 raw（不猜）。
- 回卷判定：raw < prev_raw 且差值越界 → `timestamp_wraps++`，值按位宽模 2^bits 连续延展。
- malformed 帧：短帧/信封布局不符 → drop 该帧 + `decode_errors++`，不中断会话；
  fatal（传输/协议异常）→ `XcpException` + `Stop()`。

**验收标准**：

- 带时间戳与不带时间戳两种 ECU 下回调均给出合法的 `std::chrono::nanoseconds`（或
  `raw` 保留 + `valid=false`）；
- 长时间运行（≥10 万帧，mock 回放）统计无泄漏、无越界、回调无重复计数。

**测试**：

- L1 `daq_timestamp_test.cpp`：1/2/4B 位宽、小端/大端 unit 换算、回卷延伸、越界、
  `valid=false` 路径；
- L2 `measurement_session_hardening_test.cpp`：malformed/短帧、连续错帧后仍恢复、
  drop 统计、回卷统计、Stop 后再 Start 计数归位。

**记录**：`code-plan/libxcp_测量子系统_v0.6_记录.md`。

---

### v0.9 —— XCPlite + 真 ECU 互操作

**目标**：删除测试专用手工拆包，用测量栈跑通 XCPlite 端到端，并为真 ECU 留插拔口。

**修改/删除**：

| 路径 | 动作 |
|------|------|
| `tests/xcplite_daq_test.cpp` | 删除 `EnvelopeView`(:135-141) 与 `SplitEnvelope`(:144-166)，测试改用 `DtoEnvelopeDecoder` |
| `tests/xcplite_measurement_test.cpp`（新增） | XCPlite 全链路：`FetchA2lViaUpload`→`A2lBridge::Load`→适配器→`MeasurementSession`→ 有物理值帧 |
| `examples/measurement_demo.cpp`（新增，`LIBXCP_BUILD_EXAMPLES` 下） | 最小示例：Connect→Add→Start→打印帧 |

**验收标准**：

```text
XCPlite raw DTO → DtoEnvelopeDecoder → ODT Router → DtoPayloadDecoder → adapter
               → MeasurementSample（全程无测试专用手工拆包）
```

**测试**：

- L3 `xcplite_measurement_test.cpp`（`LIBXCP_BUILD_XCPLITE_SLAVE=ON`+
  `LIBXCP_BUILD_A2L=ON`）：真实 UDP 回环读到含时间戳与换算物理值的 `MeasurementFrame`；
  `SplitEnvelope` 删除后既有 `xcplite_daq_test.cpp` 用解码器替代并保持全绿。
- L4（手工验收口径，不标 CI）：真 ECU 冒烟清单——Connect/GetDaqEventInfo/
  Configure/Start/收帧/Stop，记录到 v0.9 MD。

**记录**：`code-plan/libxcp_测量子系统_v0.9_记录.md`。

---

### v1.0 —— Measurement-only SDK 封板

**目标**：公开 API 冻结 + 文档 + 打包 + 封板清单闭环。

**动作**：

- API 审查：核心新增头全部在 `include/libxcp/{daq,measurement}/*`，namespace `calmcar::xcp`；
  `IMeasurementDatabase`/`MeasurementSession`/`DtoEnvelopeDecoder` 签名冻结；
  `adapter/a2l` 作为独立可选组件交付（README 说明构建开关）。
- 文档：README 增补"测量快速上手"（变量名→帧回调 10 行内示例）；更新 `code-plan/` 路线图。
- 测试回归：`ctest --test-dir cmake-build-release -C Release --output-on-failure`
  （含 `A2lIsolation`）全绿；Release + Windows/Linux/macOS 三平台均编译通过。
- 封板 checklist：逐项核对 `libxcp_measurement_architecture.md` §25 的未核对项
  （CAN/CAN FD Transport、大数据量/多 List 压力、真 ECU 长稳等），未闭环项如实登记为
  "v1.0 已知限制"或后置路线（架构文档 §25：下一步是 CAN 传输扩展，而非继续
  Calibration/Programming）。

**记录**：`code-plan/libxcp_测量子系统_v1.0_封板记录.md`。

---

## 7. 测试策略矩阵（L1–L4）

| 层 | 目标 | 载体 | 覆盖里程碑 |
|----|------|------|-----------|
| L1 单元 | 解码器/规划/时间戳纯逻辑 | GoogleTest（无 transport） | v0.2/v0.4/v0.6 |
| L2 mock transport | 命令交互 + 会话状态机 + 帧回放 | `tests/mock_transport.hpp` | v0.3/v0.5/v0.6 |
| L3 XCPlite 集成 | 真实 UDP 回环 + 真实 A2L | `XcpliteIntegration`（`LIBXCP_BUILD_XCPLITE_SLAVE`+`LIBXCP_BUILD_A2L`） | v0.9 |
| L4 真 ECU | 手工冒烟/长稳 | 手工清单 + MD 记录 | v0.9/v1.0 |
| 隔离门禁 | A2L 栈互不污染 | ctest `A2lIsolation` | 每步回归 |

构建回归命令（相对路径，Release，本机沙箱注意项见 `CMakeLists.txt:15-17` 注释）：
单测试 `ctest -R A2lIsolation --test-dir cmake-build-release -C Release --output-on-failure`；
全量 `ctest --test-dir cmake-build-release -C Release --output-on-failure`。

---

## 8. 风险与开放决策

| # | 事项 | 状态 | 处置 |
|---|------|------|------|
| D1 | 适配器物理落点（`adapter/a2l/` 新顶层目录 vs 改 `thirdparty` 构建） | **待确认（v0.5 前）** | 默认 §4.2 推荐方案；改 thirdparty 需用户单独许可 |
| D2 | GET_DAQ_EVENT_INFO 在 XCPlite 的支持度（可能回 CMD_UNKNOWN） | 待取证 | nullopt 兜底；测试按实然断言，不改 XCPlite |
| D3 | 真 ECU 的识别字段类型/时间戳宽度差异 | 待 L4 取证 | 全部走运行时取证（`QueryDaqProcessorInfo`/`QueryDaqResolutionInfo`），不硬编码 |
| D4 | 多 List/大 ODT 的 Slave 容量上限 | 待 L3/L4 取证 | 规划期查 `QueryDaqListInfo` 容量；超限 `DaqConfigurationError` |
| R1 | Polling 第二路径（架构文档 §13） | 本计划**不纳入 v0.2–v1.0 主线** | 封板后作为后置项；`MeasurementMode` 枚举暂不实现 |
| R2 | CAN/CAN FD Transport | 同上 | 架构文档 §25 定位为 v1.0 之后下一步，不在本计划主线 |

---

## 9. AGENTS.md 合规自查

- ✅ 中文思考/中文文档；本计划为纯文档，不写任何实现代码。
- ✅ 澄清性问题已逐一提问并闭环（m00083 范围、m00190 依赖方向）。
- ✅ 不重构已完成代码：已完成文件仅"追加枚举值/函数/成员"，不改签名与行为（I4）。
- ✅ thirdparty 零修改（I3）；`adapter/a2l/` 为主仓新增（D1 待确认）。
- ✅ 新接口全部 doxygen 中文注释；新测试全部放 `tests/`。
- ✅ 每次代码变更后写 MD 记录到 `code-plan/`。
- ✅ 路径规范：计划中一律相对路径（`include/...`、`src/...`、`tests/...`），不写绝对路径。
- ✅ 术语基线来自逐字核对的头文件（协议核心 + 桥接层），未凭记忆编造签名：
  `CommandCode`(:63-102)、`DtoPacket`(:67-70)、`DaqLedgerEntry`(:92-103)、
  `ExecuteGetDaqListInfo`(:339)、`QueryDaq*`(:347-379)、`DaqConfigGeneration`(:333)、
  `DaqTimestampBytesCached`(:419) 等均已实读确认。