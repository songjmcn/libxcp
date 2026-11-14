# XCP 多 Event（周期任务）支持能力分析

> **归属说明（2026-11-14 补充）**：本文分析的是 **DAQ 语义**下的多 event
> （从机 Event Channel 触发、DTO 数据流）。经用户澄清，"poll 模式"实指
> **主机端主动建周期任务、周期性读取 ECU 变量**（非 DAQ），该语义的分析
> 见 `code-plan/Poll模式多Event架构分析.md`。两份文档互补：
> 本文回答"DAQ 能挂几个周期任务"，后者回答"主站轮询能挂几个采集任务"。

> 目的：回答"当前 XCP 在 poll 下支持几个 event"（一个固定周期的 task 算一个 event），
> 并评估现有架构支撑"不同周期下变量读取"的差距。
> 范围：**只读分析，未修改任何源码、测试或配置**。
> 依据：本仓库代码实测（行号来自本轮 `grep -n`/`sed -n`）+ `thirdparty/XCPlite` 只读核证。
> 分析日期：2026-11-14　分支：`feate_daq_all`

---

## 1. 结论

| 问题 | 答案 |
|---|---|
| 一个会话最多能用几个 event？ | **主站侧无硬编码上限**；实际由从机的 event 容量、DAQ List 数、`MAX_DTO`、DAQ 表内存共同决定 |
| XCPlite（本仓库测试对手端）能给几个？ | **16**（`OPTION_DAQ_EVENT_COUNT = 16`），可改但受内存与 `<= 1024` 硬上限约束 |
| 从机运行时"声明"几个？ | `GET_DAQ_PROCESSOR_INFO.MAX_EVENT_CHANNEL` 返回的是**已注册事件数**，不是容量上限 —— 注册 3 个任务就报 3 |
| 现有架构能做"不同周期各读一批变量"吗？ | **能，且已有测试证明**：按 `event_channel` 分组 → 每个 event 一个 DAQ List → `START_STOP_SYNCH` 同时启动 |
| 差什么？ | 同一 event 内**无法降频**（`prescaler` 硬编码 1）、**无法分批启停**、无 per-event 统计/分流、`MeasurementFrame` 不带 `event_channel`、List 编号从 0 起与 `MIN_DAQ>0` 的从机冲突 |

一句话：**多 event 的"骨架"已经在了，缺的是"每个 event 可独立调参、独立观测、独立启停"的控制面。**

---

## 2. 术语澄清（重要，先对齐再谈数量）

"poll" 在 XCP 规范里**不是**一个正式模式名。本仓库与 `thirdparty/XCPlite` 里也没有 `poll mode` 开关。它可能指两件完全不同的事：

| 解读 | 含义 | 本库现状 |
|---|---|---|
| **(a) ECU 周期任务触发 event channel**（用户括号里的定义） | 每个固定周期任务调一次 `XcpEvent(id)`，DAQ List 绑到该 event，按该周期发 DTO | **已支持**，见 §4 |
| (b) 主站主动轮询读取 | 完全不用 DAQ，主站按自己的节拍 `SHORT_UPLOAD`/`UPLOAD` 逐个读变量 | 只有 `MemoryAccess` 的**单次**读写，**没有任何调度器**；要靠调用方自己起定时器 |

本文按 (a) 分析。若实际需求是 (b)，结论要改：那需要的是主站侧采集调度器（定时器 + 变量→周期表），与 event channel 无关——`MemoryAccess` 现有接口够用但缺调度层。

另外澄清两个容易混淆的 API：
- `thirdparty/XCPlite/src/xcpappl.c:113` `ApplXcpBackgroundTasks()`、旧版 `XCP_BackgroundTasks()`、新版 `ApplXcpRegisterIdleCallback()` —— 这些是 **XCP 服务器后台任务**（校准发布、非阻塞 socket 轮询），**与测量 event 无关**，不要把它当成 "poll 模式"。
- `src/event_poller.*`、`tests/event_poller_test.cpp` —— 是 **socket 层多路复用封装**，同样与 XCP event channel 无关。

---

## 3. 从机侧：event 容量与"声明数"是两件事

### 3.1 容量上限

| 项 | 值 | 证据 |
|---|---|---|
| 本仓库链接的 XCPlite（新版 `xcplib`） | `OPTION_DAQ_EVENT_COUNT (16)` | `thirdparty/XCPlite/src/xcplib_cfg.h:132` |
| 同上，RTOS 变体 | `OPTION_DAQ_EVENT_COUNT 16` | `thirdparty/XCPlite/src/xcplib_rtos_cfg.h:124` |
| 硬上限（编译期检查） | `> 0 && <= 1024` | `thirdparty/XCPlite/src/xcplite.h:344-348` |
| 旧版 `XCPliteV1.2` 示例配置 | `OPTION_DAQ_EVENT_COUNT 256` | `thirdparty/XCPliteV1.2/src/main_cfg.h:106` |
| 超限时的行为 | `XcpCreateEvent` 返回 `XCP_UNDEFINED_EVENT_ID` | `thirdparty/XCPlite/src/xcplite.c:902`（`if (e >= XCP_MAX_EVENT_COUNT)`） |

> 注意：`CMakeLists.txt:414` 只 `add_subdirectory(thirdparty/XCPlite)`，即本仓库实际用的是 **16**，不是旧版的 256。

### 3.2 报给主站的 `MAX_EVENT_CHANNEL` 是"当前已注册数"

```c
// thirdparty/XCPlite/src/xcplite.c:2479-2483
#if defined(XCP_ENABLE_DAQ_EVENT_INFO)
    CRM_GET_DAQ_PROCESSOR_INFO_MAX_EVENT = XcpGetEventCount(); // 已注册事件数
#else
    CRM_GET_DAQ_PROCESSOR_INFO_MAX_EVENT = 0;                  // 未知
#endif
```

含义：**ECU 里实际起了几个周期任务、注册了几个 event，主站就只能看到几个**。`MAX_EVENT_CHANNEL` 不是"这个 ECU 支持几种周期"的能力声明，而是"现在有几种可用"的枚举上限（合法通道号区间 `[0, MAX_EVENT_CHANNEL-1]`）。

本仓库测试 Slave 目前只注册 **1 个** event（`tests/xcp_test_slave/main.cpp:232` `DaqCreateEvent(testev);`），所以端到端测试实际只跑过单 event。

### 3.3 另一个隐藏约束：DAQ 表内存

| 项 | 值 | 证据 |
|---|---|---|
| DAQ 表内存 | `OPTION_DAQ_MEM_SIZE (512 * 6)` = **3072 字节** | `thirdparty/XCPlite/src/xcplib_cfg.h:136` |
| 每个测量信号（ODT Entry）约需 | 6 字节 | 同上注释 |

也就是说：**event 数量不是真正的瓶颈，Entry 总量才是**。3072 字节大约只够 512 个测量信号（跨所有 event、所有 List、所有 ODT 合计）。配 16 个 event 却每个挂 100 个变量，会先撞 `CRC_MEMORY_OVERFLOW`。

---

## 4. 主站侧：多 event 现状（每条都有代码/测试证据）

### 4.1 分组：一个 event → 一个 DAQ List

`src/measurement/measurement_planner.cpp:215-243` 按 `MeasurementSymbolInfo::event_channel` 做**稳定分组**（`event_channel==0` 单独成组），`:289-300` 跨组即开新 List，`cur.spec.daq_list = daq_list_counter++;` 从 0 连续编号。

### 4.2 下发：动态整表一次编排

`MeasurementSession::Prepare()`（`src/measurement/measurement_session.cpp:268`）把 `plan.Value().daq_lists` 整体交给 `XcpMaster::ConfigureDaqListsDynamic`，后者执行
`FREE_DAQ → ALLOC_DAQ(n) → ALLOC_ODT* → ALLOC_ODT_ENTRY* → SET_DAQ_PTR+WRITE_DAQ* → SET_DAQ_LIST_MODE*`（`src/xcp_master.cpp:1226-1261`）。

### 4.3 启动：一次拉起全部 List

`XcpMaster::StartDaqSync()`（`src/xcp_master.cpp:1285-1327`）：从账本取全部 List → 逐个 `START_STOP_DAQ_LIST(SELECT)` → 一次 `START_STOP_SYNCH(START_SELECTED)`。
`MeasurementSession::Start()`（`src/measurement/measurement_session.cpp:432`）在非 Absolute 识别模式下就走这条路。

### 4.4 解码：按 `(daq_list, odt)` 精确路由，时间戳按 List 独立延展

- 路由：`MeasurementSession::Prepare()` 建立 `(daq_list, odt) → OdtSlice[]`（`measurement_session.cpp:368-396`）。
- 时间戳：worker 里 `std::map<std::uint16_t, DaqTimestampConverter> converters;`（`measurement_session.cpp:549`），**每个 DAQ List 一条独立换算流**，注释明确"不同通道的 raw 计数互不相干，混用会把两路差值误判为回卷"。
- 计数器：`std::map<std::uint16_t, std::uint8_t> last_counters;`，同样按 List 独立。

### 4.5 已有测试

| 测试 | 证明什么 |
|---|---|
| `tests/measurement_planner_test.cpp:290` `MultiEventMultiList` | 输入 4 个变量跨 2 个 event → 产出 2 个 List，`daq_list` 0..N-1，分组按首现顺序、组内保序 |
| `tests/measurement_session_test.cpp:447` `MultiEventRoutesToSeparateLists` | 2 个 event 的变量同会话测量；`ALLOC_DAQ` 一次给 2 个 List；`START_STOP_SYNCH` 只发 1 次；两路 DTO 分别路由到各自 List 的切片，帧带各自时间戳 |
| A2L 侧 `thirdparty/a2l-sdk/adapter/src/measurement_runtime_profile.cpp:176-206` | `BuildMeasurementRuntimeProfile` 遍历 A2L 全部 EVENT，逐个 `QueryDaqEventInfo` 交叉核对周期/单位；同通道周期冲突报错；通道号超 `max_event_channel` 报错 |

**结论：≥2 个 event 的规划、下发、启动、路由、解码全链路已被测试锁定。** 不是"只支持 1 个"。

---

## 5. 能力边界清单（"不同周期读取"真正缺的东西）

### 5.1 高影响：同一 event 内无法降频

```cpp
// src/measurement/measurement_planner.cpp:301
cur.spec.prescaler = 1;
```

`DaqListSpec::prescaler`（`include/libxcp/xcp_master.hpp:80`）**硬编码为 1**，且 `MeasurementSession` 没有任何设置入口。

后果：想要"10ms 任务里的 A 变量按 10ms 发、B 变量按 100ms 发"，只能：
- 在 A2L/ECU 里为 B 单开一个 100ms 的 event（需要改 ECU 侧），或
- 拆成两个会话分别跑（但两个会话会争同一个 `XcpMaster` 与同一张 DAQ 表），或
- 不用 DAQ，改用主站轮询读 B。

这正是"多 event 架构"最该补的一环：`SET_DAQ_LIST_MODE` 的 `PRESCALER` 字段本来就是干这个的，协议支持、`XcpMaster` 也透传（`xcp_master.hpp:426`），只是规划层写死了。

### 5.2 高影响：无法分批启停特定 event

`StartDaqSync()` 一次启动**全部**已配置 List；`StopDaq()` 一次停**全部**（`src/xcp_master.cpp:959-964`，`STOP_ALL`）。虽然 `StopDaqList(list)` 存在（`:952`），但会话层没暴露。

后果：做不到"先开 10ms 那路、观察后再开 100ms 那路"，也做不到"临时关掉某一路降负载"。

### 5.3 中影响：单队列混合，高频 event 挤掉低频 event

`MeasurementSession` 只有一个 `std::deque<Bytes> m_queue_`，容量 `kMaxQueue = 256`（`measurement_session.hpp:261`），溢出策略是 `m_queue_.pop_front()`（`measurement_session.cpp:498`，丢**最老**的帧）。

后果：100Hz + 1Hz 两个 event 同时跑，队列满时被丢的一定是更老的帧——低频 event 的帧**几乎必然**先被挤掉。低频变量的采样完整性得不到保障。

### 5.4 中影响：统计不按 event 分开

`MeasurementStatistics`（`measurement_session.hpp:51-58`）只有全局 `dto_received/dto_dropped/decode_errors/timestamp_wraps/counter_gaps/counter_lost`。多 event 下无法回答"是哪一路在丢帧"。

### 5.5 中影响：`MeasurementFrame` 不带 `event_channel`

```cpp
// include/libxcp/measurement/measurement_sample.hpp:51-61
struct MeasurementFrame {
    std::uint16_t daq_list;      // 有
    std::uint8_t odt;            // 有
    ...                          // 没有 event_channel
};
```

上层拿到帧后要知道"它属于哪个周期"，目前只能自己维护 `daq_list → event_channel` 映射（而该映射是规划器内部产物，`MeasurementPlan` 不对外暴露生命周期）。这是多周期场景下很实际的接口短板。

### 5.6 中影响：List 编号从 0 起，与 `MIN_DAQ > 0` 的从机冲突

`planner.cpp:298` 注释写着"序号从 0..N-1（ConfigureDaqListsDynamic 硬门槛）"，但 master 侧 B0/F-05 之后已经改成：

```cpp
// src/xcp_master.cpp:1153-1168
const std::uint16_t min_daq = ... m_daq_processor_info_->min_daq ...;
const std::uint16_t expected = static_cast<std::uint16_t>(min_daq + i);
if (specs[i].daq_list != expected) { throw ... "ALLOC_DAQ 的列表编号须为连续的 [MIN_DAQ, MIN_DAQ+n-1]" }
```

**规划器硬编码 0、master 要求 `MIN_DAQ+i`，两者只在 `MIN_DAQ==0` 时一致**。XCPlite 的 `MIN_DAQ` 恒为 0（`thirdparty/XCPlite/src/xcplite.c:2477`），所以现有测试全绿；换一个有预定义 List 的从机，多 event 场景会**直接抛异常**。单 event 也一样，只是多 event 更容易撞上。

### 5.7 低影响：未校验 `event_channel <= max_event_channel`

A2L 侧校验了（`measurement_runtime_profile.cpp:180`），但 `MeasurementPlanner`/`MeasurementSession`/`ConfigureDaqListsDynamicImpl` 都没校验。如果调用方绕过 A2L 适配层自己实现 `IMeasurementDatabase`，越界通道号会一路下发到 Slave 才报错。

### 5.8 低影响：`event_channel == 0` 的语义二义

`MeasurementSymbolInfo::event_channel` 的注释是"0=不由通道触发"（`measurement_database.hpp:34`），但 XCPlite 里 **event 0 在合法通道区间 `[0, MAX_EVENT_CHANNEL-1]` 内**，且 `xcplib_cfg.h:135-138` 还提供了 `OPTION_DAQ_ASYNC_EVENT` 来专门创建一个 id=0 的周期默认事件。

后果：A2L 里真有一个 event 通道号为 0 的测量时，规划器会把它当"无事件"单独成一组，虽然 `SET_DAQ_LIST_MODE` 仍写 0 而"碰巧"对——但这是**巧合正确**，不是设计保证。

### 5.9 低影响：Absolute PID 模式天然只支持单 event

```cpp
// src/measurement/measurement_session.cpp:264-267
if (m_id_field_type_ == IdentificationFieldType::Absolute &&
    (plan.Value().daq_lists.size() != 1U || ...)) {
    ThrowInvalid("Absolute PID DTO 当前仅支持单个 DAQ List（DAQ_LIST 0）");
}
```

这是现有实现明确拒绝的路径，不算缺陷（Absolute PID 的帧里没有 List 号，多 List 无法区分），但意味着**选 Absolute 识别模式就等于放弃多 event**。

### 5.10 低影响：`SetDaqPackedMode` / `SetDtoCounterEnabled` 是全局开关

B2/B4 加的几个开关都作用在**全部** List 上，不能 per-list。多 event 下若想"10ms 那路打包、100ms 那路不打包"，目前做不到（packed 本身还要求周期事件，见 B4 记录 §6.1）。

---

## 6. 若要支撑"不同周期读取"的建议路线（仅建议，未实施）

按"改动面 / 收益"排序，供后续决策：

| 阶段 | 内容 | 触及层 | 备注 |
|---|---|---|---|
| P1 | `prescaler` 可配置（规划参数 + `MeasurementSession` 入口） | Planner → Session | 最小改动、收益最大；`XcpMaster` 已透传，只需规划层不再写死 |
| P2 | 暴露 per-list 启停（`StartDaqList`/`StopDaqList` 的会话层封装） | Session | 注意 XCPlite 拒绝单 List START，只能走 SELECT+SYNCH；需明确"部分启动"的语义 |
| P3 | `MeasurementFrame` 补 `event_channel` | 输出接口 | 破坏性小（加字段），但要在 Prepare 时把 `daq_list→event_channel` 落在会话内 |
| P4 | 队列按 List 分流或至少按 List 记账丢弃 | Session | 高频挤低频是真问题，但分流会改变回调时序保证 |
| P5 | `MeasurementStatistics` 增加 per-List 维度 | Session | 与 P4 一起做更自然 |
| P6 | 规划器按 `MIN_DAQ` 起编 List 号 | Planner | 修 5.6；需要 Planner 拿到 `min_daq`（现在构造参数里没有） |
| P7 | 补 `event_channel <= max_event_channel` 校验 | Planner/Session | 防御性 |

**不建议**做的：
- 不要为了"多周期"去扩展 `MeasurementSession` 支持多实例共享一个 `XcpMaster`——`XcpMaster` 的 DAQ 账本是单份全局状态，多会话会互相 `FREE_DAQ` 掀桌。
- 不要用 `START_STOP_DAQ_LIST(START)` 单表启动去"绕开"同步启动——XCPlite 会回 `CRC_MODE_NOT_VALID`（`tests/xcplite_daq_test.cpp` 文件头 D12 已记录）。

---

## 7. 本轮如何验证（只读命令，未执行任何修改）

```bash
# 从机容量
grep -n "OPTION_DAQ_EVENT_COUNT" thirdparty/XCPlite/src/xcplib_cfg.h
grep -n "OPTION_DAQ_MEM_SIZE" thirdparty/XCPlite/src/xcplib_cfg.h

# MAX_EVENT_CHANNEL 的运行时含义
grep -n "CRM_GET_DAQ_PROCESSOR_INFO_MAX_EVENT" -A 5 thirdparty/XCPlite/src/xcplite.c

# 主站多 event 分组与编号
sed -n '215,310p' src/measurement/measurement_planner.cpp

# 规划器写死 prescaler=1
grep -n "prescaler = 1" src/measurement/measurement_planner.cpp

# 同步启动 / 全停
sed -n '1285,1327p;952,964p' src/xcp_master.cpp

# MIN_DAQ 编号硬门槛
sed -n '1140,1175p' src/xcp_master.cpp

# 现有双 event 测试
sed -n '447,500p' tests/measurement_session_test.cpp
```

---

## 8. 需要你确认的点

1. **"poll" 到底指 §2 的 (a) 还是 (b)**？(a) 才是 event channel；(b) 需要的是主站采集调度器，方案完全不同。
2. **"不同周期"的来源**：是 ECU 已有多个周期任务（那就只需 P1 的 prescaler 做降频），还是要求主站自己定义周期（那可能该走 (b) 轮询）？
3. **是否需要 per-event 回调分流**（每个周期一个 callback），还是保持单一回调 + 帧内区分？
4. 若推进，优先做 P1/P2 即可覆盖大部分需求，是否按此范围出设计方案？