# Poll（主站主动轮询）模式多 Event 架构分析

> 分析对象：`D:\project\libxcp` 当前实现（分支 `feate_daq_all`，B0–B4 已完成）。
> 分析性质：**只读分析，未修改任何源码**。文中所有结论均附代码/规范出处，可复核。
> 前置澄清：本文的 "event" 指**主机端自建的周期采集任务**（`{周期, 变量集合}`），
> 与 XCP 协议的 DAQ Event Channel 是两件事。DAQ 语义下的多 event 分析见
> `code-plan/多Event架构支持分析.md`。

---

## 1. 结论

### 1.1 直接回答"poll 下支持几个 event"

**当前库里 poll 模式的 event 数量是 0 —— 不是容量为 0，而是没有这个概念。**

库里既没有"周期任务"抽象，也没有定时器、调度器或采集线程。poll 通路只提供
**单次同步读原语**，由调用方每次自己发起：

| 层次 | 现状 |
|------|------|
| `XcpMaster::ReadMemory/ReadMemoryBytes` | 单次同步读，一次调用一次往返，无周期语义 |
| `MemoryAccess::ReadElements` | 自动选 SHORT_UPLOAD 或 SET_MTA+UPLOAD 分块 |
| 采集调度（定时、分组、批合并） | **不存在** |
| 时间戳 / 丢样 / 超期统计 | **不存在**（poll 路径无任何时间元数据） |

证据：`include/libxcp/xcp_master.hpp:325,334` 只有 `ReadMemoryBytes` / `ReadMemory`
两个同步入口；`src/xcp_master.cpp:703-711` 直接转发给 `m_memory_access_`，
没有加锁、没有调度、也没有时间戳回填。库内线程只有 transport 的 RX 线程
（`src/tcp_transport.cpp:289`、`src/udp_transport.cpp:373`）与 DAQ 的 worker
线程（`src/measurement/measurement_session.cpp:435`），**没有定时器线程**
（全库 `std::chrono::steady_clock::now() +` 只出现在超时/恢复判定里）。

### 1.2 若由调用方自行搭多 event，实际能支撑几个

调用方当然可以在外部写 N 个线程/定时器，但会撞上一条硬约束：

> **同一时刻只允许一条 Outstanding Command。**

`src/command_executor.cpp:908-913` 的 `RunCommand` 首行：

```cpp
if (m_session_.HasPendingCommand()) {
    throw detail::MakeInvalidState("已有待响应的命令，拒绝并发发送");
}
```

`Session::MarkCommandSent` 在锁内**再判一次**（`src/session.cpp:202-209`），
所以不会破坏状态一致性 —— 但**后到的线程拿到的是异常，不是排队等待**。
结论：多 event 并发轮询会大量抛 `InvalidState`，必须由调用方自行串行化。

因此实际能力取决于 **串行调度**，而不是并发度：

```
一个轮询周期的总耗时 ≈ Σ(每变量往返时间) + 调度开销
可达最小周期     ≈ 该 event 变量数 × 单次往返时间
```

### 1.3 poll 相比 DAQ 的一个天然优势

poll **不注册任何 DAQ event**，所以**不受从机 event 数量限制**。
XCPlite 的 `OPTION_DAQ_EVENT_COUNT` 是 16（`thirdparty/XCPlite/src/xcplib_cfg.h:132`），
但那是 DAQ 通道数；poll 走命令通道，从机侧对 poll "event" 没有计数概念。
代价是每次读都要一次命令往返，吞吐远低于 DAQ。

---

## 2. 术语对齐：poll event 与 DAQ event 的差别

| 维度 | poll（本文） | DAQ（另一份文档） |
|------|--------------|-------------------|
| 谁驱动 | **主机**定时器 | **从机** Event Channel 触发 |
| 传输通路 | 命令通道（SHORT_UPLOAD/UPLOAD） | DTO 数据流 |
| 数据量/次 | 通常 1 个变量（可合并连续地址） | 一次可带整个 ODT 的多个变量 |
| 周期精度 | 受往返时间与调度抖动影响 | 由从机事件周期决定，抖动小 |
| 从机资源 | 不占用 DAQ 表 | 占用 DAQ List / ODT / DAQ 表内存 |
| event 数量上限 | 无协议上限（受带宽限制） | 受 `OPTION_DAQ_EVENT_COUNT`(16) 与 DAQ 表内存限制 |
| 丢样语义 | 无（主机自己取，慢了就超期） | 有（DTO 丢帧，可用 DTO counter 检测） |
| 时间戳 | 只有主机本地时刻 | 可带 ECU 时间戳，可做时间关联 |

---

## 3. 现有 poll 能力盘点（可复用的原语）

### 3.1 读通路与往返成本

`src/memory_access.cpp:135-155` 的 `ReadElements` 决策顺序：

1. 若 `short_upload_available` 且元素数不超上限 → **SHORT_UPLOAD（1 次往返）**
2. 仅当异常满足 `ErrorCode::CmdUnknown && CommandCode::ShortUpload` 才降级
3. 否则 → `UploadChunked`：**每块 `SET_MTA` + `UPLOAD`（2 次往返/块）**
   （`src/memory_access.cpp:101-107`）

这意味着 **poll 的每次读取成本取决于从机是否支持 SHORT_UPLOAD**，
差一倍往返。

### 3.2 单次读取容量

`src/memory_access.cpp:21-34`：

```cpp
ElementCount MaxShortUploadElements() const { return max_cto / ag; }
ElementCount MaxUploadElements()      const { return limit - 1; }
```

`MaxUploadElements`（用于 UPLOAD 分块）与规范一致；`MaxShortUploadElements`
存在 off-by-one 偏差，详见 **§8.1**。

### 3.3 变量元数据可以复用 A2L 数据库

poll 需要知道变量的地址、扩展、宽度与物理换算规则，这套东西 DAQ 侧已经具备：

`include/libxcp/measurement/measurement_database.hpp:31-38`

```cpp
struct MeasurementSymbolInfo {
    Address address;                ///< 32 位地址
    AddressExtension extension;     ///< 8 位地址扩展
    std::uint16_t event_channel;    ///< 来自 A2L Event（poll 模式下不使用）
    std::uint8_t element_size_bytes;
    std::uint8_t element_count;
    bool is_array;
};
```

配合 `ToPhysical(name, raw)`（`:60-61`）即可把原始字节换算成物理值。
**结论：poll 架构不需要重造变量元数据，直接复用 `IMeasurementDatabase`。**

### 3.4 与 DAQ 共存是可行的

`src/command_executor.cpp:347-430` 的 `OnPacketReceived` 分流规则：

- `RES` / `ERR` → 存入唯一响应槽位（无 pending 时按 stale 丢弃）
- `EV_CMD_PENDING` → 只置标记，不占槽位
- `DTO` → `async_dto`，直接投递给 listener，**不占响应槽位**

所以 **DAQ 跑着的时候同样可以发 poll 命令**，两者不会抢响应槽位。
但两者共享同一条物理链路带宽，DTO 高频会挤压命令响应延迟。

另外 `Session::MarkCommandSent`（`src/session.cpp:210-216`）只在
`Failed`/`Disconnected` 时拒发命令，**DAQ 运行中并未禁止命令**，
所以协议层也不拦。

---

## 4. 三个硬约束（多 event 架构必须先解决的前置条件）

### 4.1 单 Outstanding Command，且不是排队式

见 §1.2。这是 **XCP Standard Communication Model 的语义**，不是实现偷懒：
`src/session.cpp:206-208` 的报错文案明确写了这一点。

对多 event 的含义：
- 不能"一个 event 一个线程各自读"
- 必须有一个**串行执行点**（单线程调度，或外部互斥）
- 若用互斥锁保护，锁等待会直接变成周期抖动

### 4.2 MTA 是全局共享状态

MTA 在从机侧只有一份。`SET_MTA + UPLOAD` 是两段式操作，
若被其他命令插入，第二次 `UPLOAD` 会读到错误位置。

更隐蔽的是**超时恢复**：`CommandExecutor` 缓存了 `m_last_mta_`，
超时/恢复后会重放 `SET_MTA`（`src/command_executor.cpp:711` 定义
`RestoreUploadMta()`，`:1077` 在恢复路径中调用）。如果两个轮询任务交错，恢复重放可能把 MTA 指向
另一任务的地址。

调研中还发现一个相关事实：**SHORT_UPLOAD 也会改写从机 MTA**。规范 §7.5.1.13
说明"the MTA pointer is set to the first data byte behind the uploaded data block"，
XCPlite 侧对应实现见 `thirdparty/XCPlite/src/xcplite.c:2341-2361`
（`if (!async) { XcpSetMta(...); ... } XcpReadMta(...)`）。

对多 event 的含义：
- 单帧 SHORT_UPLOAD 本身是原子的，不会出现"两段式被插入"
- 但 **分块 UPLOAD 路径必须与其它读严格串行**
- 混合使用两种路径时，恢复逻辑的 MTA 重放必须谨慎

### 4.3 无定时器、无线程池、无时间戳

库内没有可供复用的调度设施（§1.1）。poll 路径也**完全不产生时间元数据**：
`MeasurementFrame` 的 `timestamp`/`timestamp_source` 只服务于 DAQ 解码
（`include/libxcp/measurement/measurement_sample.hpp`）。

对多 event 的含义：
- 定时必须由调用方（或新组件）提供
- poll 样本的时间戳只能是**主机本地时刻**，且带往返不确定性
- 若需要与 ECU 时钟对齐，得另用 `GET_DAQ_CLOCK`（B3 已实现，
  `include/libxcp/xcp_master.hpp:688` `ReadDaqClock`）

---

## 5. 一次 tick 的成本模型

设某 event 有 `k` 个变量，地址不连续（保守假设）：

| 路径 | 往返次数 | 说明 |
|------|----------|------|
| 从机支持 SHORT_UPLOAD | `k` | 每变量 1 次请求 + 1 次响应 |
| 从机不支持 SHORT_UPLOAD | `2k` | 每变量 2 次（SET_MTA + UPLOAD） |
| 变量地址连续且可合并（总量 ≤ 单帧容量） | `1` | 一次读一段，按偏移切分后各自换算 |
| 变量地址连续但超单帧容量 | `2 × 块数` | 分块 UPLOAD |

所以"不同周期下读变量"的真实约束是：

```
最小可达周期(event) ≈ k × 单次往返时间
```

单次往返时间由链路（CAN/UDP/TCP）、从机处理和主站线程唤醒共同决定，
本库不提供测量手段，也不应写死一个数字。可参考的只有兜底超时
`CommandTimeouts::command_timeout = 1000 ms`（`include/libxcp/command_executor.hpp:36`），
那是**故障判定阈值，不是周期能力**。

**工程含义**：poll 适合"变量少、周期慢（几十 ms 以上）"的场景；
变量多、周期快（1–10 ms）的场景必须用 DAQ。

---

## 6. 若要支持多 event，需要补齐的组件

以下为设计建议（**未实施**），按依赖顺序排列：

### 6.1 采集任务抽象

```
PollEvent {
    name             // 任务名
    period           // 周期
    symbols[]        // 变量名列表（经 IMeasurementDatabase::Find 解析）
    on_sample        // 回调
}
```

### 6.2 单线程调度器

- 一个调度线程维护"下一个到点的 event"（最小堆或按周期排序的队列）
- **所有 poll 读取在该线程串行执行** —— 这是由 §4.1 决定的，无可回避
- 同一 tick 内到点的多个 event 按优先级/固定顺序合并执行

### 6.3 地址合并优化

同一 tick 内：把地址相邻、且总宽度不超过单帧容量的变量合并成一次读取，
读回后按各自偏移切分再 `ToPhysical`。这是 poll 唯一能显著降低往返数的手段。

### 6.4 超期与失败语义（必须显式定义）

- **超期**：一个 tick 没读完就跨入下一个周期时，是跳过、还是顺延？
- **单变量失败**：整 tick 失败，还是跳过该变量继续？
  （建议：单变量失败不拖垮整 tick，但要上报，不能静默）
- **通信故障**：连续失败应触发降级/重连，而不是无限重试拖死调度线程

### 6.5 时间戳来源

- 仅主机：记录读请求发出/响应到达的时刻，标注不确定度
- 与 ECU 对齐：需要 `ReadDaqClock` 配对采样（B3 能力），
  且必须区分"ECU 时钟"与"主机本地时钟"，不能混用

### 6.6 与 DAQ 共存策略

- 两者共享链路带宽，需要限流（否则 DTO 洪峰会拉长 poll 往返）
- DAQ 的 `StopDaq`/`StartDaqSync` 与 poll 调度必须互斥操作配置阶段

---

## 7. poll 与 DAQ 的选型建议

| 场景特征 | 推荐 |
|----------|------|
| 变量少（<10）、周期 ≥ 20 ms、需要动态增删变量 | **poll** |
| 变量多、周期 1–10 ms、要求低抖动 | **DAQ** |
| 从机 event 数不够（XCPlite 只有 16）但需要多周期 | **poll**（不受 16 限制） |
| 需要 ECU 时间戳精度 | **DAQ**（poll 只有主机时刻） |
| 从机不支持动态 DAQ 分配 | **poll** |

实际上两者可以并存：慢速、临时的变量用 poll；高速、固定的变量用 DAQ。

---

## 8. 附带发现（本次只读分析中顺带确认的问题，未修改）

### 8.1 `MaxShortUploadElements()` 比规范多算 1 个元素（已决定修复，待实施）

规范 §7.5.1.13 Table 72（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md:3636-3650`）：

> n = Number of data elements [AG] **[1..MAX_CTO/AG -1]**
> The error handling and the response structure is identical to the UPLOAD command.

而 `src/memory_access.cpp:21-29` 返回的是 `max_cto / ag`。

**后果**：当 `MAX_CTO=8`、`AG=1` 时，代码认为可以 SHORT_UPLOAD 8 个元素，
但响应帧需要 `1 (PID) + 8 = 9` 字节，超过 `MAX_CTO`。真实从机会回
`ERR_OUT_OF_RANGE`，而 `ReadElements` 的回退条件只认 `CmdUnknown`
（`src/memory_access.cpp:144-152`），因此**异常会直接上抛，不会降级到 UPLOAD**。

**对 poll 的直接影响**：CAN 经典帧（8 字节）下读取 **8 字节变量（如 double）**
正好撞在这个边界上。4 字节变量（`1+4=5 ≤ 8`）不受影响。

**为什么测试没抓到**：`tests/memory_access_test.cpp:558-572`
`SingleShortUploadWithinLimit` 用 `n = MAX_CTO/AG` 断言成功，但 mock slave
不校验响应长度，所以 mock 比真实协议宽松。

### 8.2 poll 路径完全没有时间/质量元数据

`ReadMemory` 只返回 `Bytes`，调用方拿不到"这次读是在什么时刻完成的"。
做多周期采集时，无法计算样本时间戳，也无法判断是否超期。这是新架构必须补的。

### 8.3 无"读多个不连续变量"的批量命令

XCP 协议本身没有多地址读命令，所以 poll 想降低往返只能靠**地址合并**（§6.3）。
这不是实现缺陷，是协议限制，但会直接影响多 event 架构的周期下限。

---

## 9. 本轮如何验证（只读命令，均未产生修改）

```bash
# 1. poll 通路只有同步单次读，无调度
grep -n "Bytes XcpMaster::ReadMemoryBytes" -A 4 src/xcp_master.cpp

# 2. 库内无定时器线程（只有 transport RX 与 DAQ worker）
grep -rn "std::thread\|std::jthread" src/ include/libxcp/

# 3. 单 Outstanding Command 约束
grep -n "拒绝并发发送" -B 4 -A 2 src/command_executor.cpp
grep -n "只允许一条" -B 6 -A 4 src/session.cpp

# 4. SHORT_UPLOAD 单次往返 / 分块两段式
sed -n '135,155p' src/memory_access.cpp

# 5. DTO 与响应分流（DAQ 与 poll 可共存）
sed -n '347,430p' src/command_executor.cpp

# 6. 规范 SHORT_UPLOAD 的 n 上限
sed -n '3636,3650p' docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md
```

---

## 10. 已确认的约束（2026-11-14 用户答复）

| # | 决策 | 对架构的含义 |
|---|------|--------------|
| 1 | 最快周期 **20 ms** | 单 event 周期下限按 20 ms 设计；不做 1–10 ms 级优化 |
| 2 | **不与 DAQ 并存** | poll 与 DAQ 互斥；库内入口校验拒绝，不做并发协调 |
| 3 | **本地时间戳** | 不做 ECU 时钟对齐，不引入 `GET_DAQ_CLOCK` 配对 |
| 4 | **超期跳过**（保实时性） | 错过下一触发点即丢本次，不补读；大规模读取交 DAQ |
| 5 | **修复 §8.1 off-by-one** | `MaxShortUploadElements()` 改为 `MAX_CTO/AG - 1` |

后续实施计划见 `code-plan/Poll模式多Event架构实施计划.md`。
§8.1 的修复方案、连锁影响与测试矩阵均在该文档的 §7–§8，**尚未实施**。