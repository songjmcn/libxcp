# P1：Poll 会话骨架实施记录

> 对应计划：`code-plan/Poll模式多Event架构实施计划.md` §9 批次 P1。
> 本记录只覆盖 P1 范围内的真实改动与真实测试结果，不含预期或估算。
> 记录日期：2026-11-14。

---

## 1. 批次范围

| 项 | 内容 |
|----|------|
| 计划要求（§9 P1） | `PollEventSpec` / `PollSample` / `PollStatistics` 类型 + `PollSession` 骨架（`AddEvent`/`Validate`/`Start`/`Stop`）+ 单 event 调度 |
| 本次交付 | 上述全部；调度循环按"最早到期优先"顺序轮询，因此已可承载多 event（各自独立周期） |
| 不在本批 | 最小堆调度（P2）、地址合并（P3）、超期跳过统计的细化口径（P2/P3）、20 ms 容量标定（P4） |

---

## 2. 设计依据

- **轮询语义**：poll 不使用 DAQ event channel。从机侧无事件、无 DAQ List、无 DTO；
  主站按本地定时器主动 `SHORT_UPLOAD`（不支持时回退 `SET_MTA` + `UPLOAD`）读取变量内存。
  因此"event 数量"在 poll 下完全由主站定义，不受从机 `OPTION_DAQ_EVENT_COUNT` 限制。
- **命令串行**：`CommandExecutor::RunCommand` 在存在待响应命令时直接抛
  `InvalidState`（`src/command_executor.cpp:908-913`），故采集必须集中在**单线程**串行执行；
  本类因此只创建一个调度线程，不提供多线程读取接口。
- **与 DAQ 互斥**：DAQ 由从机驱动 DTO 流，poll 由主站主动 UPLOAD；同一 transport 上混用会让
  周期统计与错误归因失去意义（实施计划 §1.2-2），`Start` 时以 `DaqLedger()` 非空为拒绝条件。
- **时间戳口径**：取主站本地时钟（实施计划 §1.2-3），同时给 `steady_clock`（算周期偏差）与
  `system_clock`（与外部日志对齐）两个时刻。

---

## 3. 代码变更

| 文件 | 变更 |
|------|------|
| `include/libxcp/poll/poll_types.hpp` | **新增**。`PollEventSpec`（名字/周期/符号表，周期下限 `kMinPeriod=20ms`）、`PollVariableSample`（符号/物理值/`valid`/`error`）、`PollSample`（事件名 + 双时钟 + 变量结果数组）、`PollStatistics`（`ticks`/`skipped_overruns`/`read_errors`/`callback_errors`）、`PollCallback` |
| `include/libxcp/poll/poll_session.hpp` | **新增**。`PollSession(master, database)`、`AddEvent`/`RemoveEvent`/`Validate`/`Start`/`Stop`/`IsRunning`/`Statistics`/`EventCount`；私有 `ResolvedVariable`/`EventState`/`RunLoop`/`RunTick`/`ResolveEvent` |
| `src/poll/poll_session.cpp` | **新增**。事件表管理与校验、调度循环（绝对时刻 `wait_until` + 超期跳过）、单 tick 读取编排（`ReadMemoryBytes` → `ToPhysical`）、统计累计 |
| `CMakeLists.txt` | 库源文件列表新增 `src/poll/poll_session.cpp`（带批次注释） |
| `tests/CMakeLists.txt` | 测试清单新增 `poll_session_test.cpp` |
| `tests/poll_session_test.cpp` | **新增**。15 个用例（见 §5） |

---

## 4. 设计决策

1. **绝对时刻等待而非相对睡眠**：调度循环用 `m_cv_.wait_until(next)`，`next` 是"上次计划时刻 + 周期"，
   不用 `sleep_for(period)`——后者会把每轮的执行耗时累加进周期，20 ms 任务会持续漂移。
2. **超期跳过判定为"迟到至少一个完整周期"**：`now >= next + period` 时丢弃全部已过期 tick，
   把 `next` 直接推进到下一个未过期时刻，并把丢弃数量计入 `skipped_overruns`；**不补跑**。
   迟到一个周期以内仍按原计划采集，避免把抖动误判为超期。
3. **错误隔离**：单个变量的读取/换算失败只让该变量 `valid=false` 并累加 `read_errors`，
   不丢弃整个 tick、不影响同 tick 的其他变量。异常统一在 `RunTick` 内按变量粒度捕获。
4. **回调异常不终止调度**：`catch (...)` 后累加 `callback_errors` 并继续，否则一次用户代码异常会
   静默终止整条采集链路。
5. **运行期事件表冻结**：`AddEvent`/`RemoveEvent` 在运行中抛 `InvalidState`。运行期修改事件表会让
   调度线程持有的工作副本与外部视图不一致，且无法给出确定的生效时刻；需要改配置就 `Stop` → 改 → `Start`。
6. **`Start` 内部复制并解析事件表**：调度线程使用独占的工作副本（`m_running_events_`），
   外部事件表在运行期不被读取，避免跨线程读写同一容器。
7. **统计用原子计数器**：`Statistics()` 是高频旁路查询，不应与调度线程争 `m_mutex_`；
   四个计数各自 `std::atomic<std::uint64_t>`，读取时合成快照。
8. **复用既有结果类型**：`Validate()` 返回 `MeasurementResult<void>`（`measurement_result.hpp` 已提供
   `void` 特化），不新造 `PollResult`，减少与测量子系统的类型分叉。
9. **只支持标量**：`ResolveEvent` 对 `element_count != 1` 直接报错。数组读取需要
   `ToPhysicalElement` 逐元素换算，属后续批次（计划 §10 已列为边界）。
10. **DAQ 互斥判定用账本而非标志位**：`DaqLedger()` 非空即视为"已配置 DAQ"，不引入新状态位，
    因为账本本身就是本端对从机 DAQ 配置的唯一权威记录。

---

## 5. 测试（`tests/poll_session_test.cpp`，套件 `PollSessionSkeleton`）

| # | 用例 | 断言要点 |
|---|------|----------|
| 1 | `AddEventRejectsDuplicateName` | 同名注册抛异常，事件数不变 |
| 2 | `AddEventRejectsPeriodBelowMinimum` | 19 ms 被拒（下限 20 ms） |
| 3 | `AddEventRejectsEmptySymbolList` | 空符号表被拒 |
| 4 | `RemoveEventRejectsUnknownName` | 移除成功；再次移除未注册名抛异常 |
| 5 | `ValidateReportsUnresolvedSymbol` | 返回 `NotFound` 并带出失败符号名 |
| 6 | `ValidateReportsArraySymbolAsUnsupported` | 数组符号返回 `UnsupportedDataType` |
| 7 | `ValidateSucceedsForScalarSymbols` | 标量符号校验通过 |
| 8 | `StartRejectsWhenNotConnected` | 未连接主站时拒启动 |
| 9 | `StartRejectsWithoutEvents` | 无事件时拒启动 |
| 10 | `StartRejectsUnresolvedSymbol` | 符号不可解析时拒启动，且状态仍为未运行 |
| 11 | `SingleEventProducesPeriodicSamples` | 20 ms 事件在 2 s 内收到 ≥3 帧；`event_name`/变量顺序/物理值正确；`ticks ≥ 3` |
| 12 | `ReadFailureMarksVariableInvalidWithoutStopping` | 从机回 `ERR_OUT_OF_RANGE` 时该变量 `valid=false` 且 `error` 非空，`read_errors ≥ 1`，调度继续 |
| 13 | `CallbackExceptionDoesNotStopScheduling` | 回调首帧抛异常后仍收到后续帧，`callback_errors ≥ 1` |
| 14 | `StopFlipsRunningAndIsIdempotent` | `Start`/`Stop` 正确翻转状态，`Stop` 可重复调用 |
| 15 | `AddEventRejectedWhileRunning` | 运行中 `AddEvent`/`RemoveEvent` 均抛异常 |

### 5.1 实现期抓到的三个真实缺陷

1. **响应报文缺 PID 前缀**：测试 helper `Res()` 初版直接返回 `Bytes{...}`，忘了在首字节填 `0xFF`
   （`Err()` 同理缺 `0xFE`）。`MockTransport` 把响应原样投递给 `OnPacketReceived`，首字节不是
   `0xFF/0xFE/0xFD` 的报文不被识别为响应，导致 `CONNECT` 无响应超时（221 ms）并报
   `当前状态不允许进入恢复: Connecting`——14/15 用例连锁失败。修正后 helper 统一补前缀。
2. **`MockTransport` 的命名空间**：它定义在 `calmcar::xcp::test` 而非 `calmcar::xcp`，
   测试里必须 `using test::MockTransport;`，否则 `std::unique_ptr<MockTransport>` 直接编译失败（C2065）。
3. **测试装置地址布局自相矛盾**：`Rig` 初版把符号地址写成 `0x1000 + i`，却把内存填到
   `0x1000 + i * scale + b`。`scale=1` 时两者巧合相等，一旦用 2/4 字节宽度就会读到相邻变量的
   字节。改为 `0x1000 + i * scale`，让宽度参与地址计算。

---

## 6. 实测结果（本机 Release）

| 项 | 结果 |
|----|------|
| 构建 | `cmake --build cmake-build-release --target libxcp_tests` → `EXIT=0` |
| 本仓库源码告警 | **0**（`p1_build.log` 中 `warning` 计数为 0） |
| 新增用例 | `--gtest_filter='PollSessionSkeleton*'` → **15 tests / 15 passed / 139 ms** |
| 全量回归 | `libxcp_tests.exe` → **993 tests / 992 passed / 1 skipped / 0 failed / 46905 ms** |
| 跳过项 | `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`（P0 之前即存在） |

用例数由 978（P0 后）增至 993，增量 15 与新增用例数一致。

---

## 7. 边界与遗留

1. **数组不支持**：`Validate`/`Start` 对 `element_count != 1` 直接拒绝（见 §4-9）。
2. **无地址合并**：每个变量一次读取往返。P3 才做"相邻地址合并成一次 UPLOAD"，这是 poll 唯一的
   性能杠杆；当前 N 个变量 = N 次往返。
3. **调度为线性扫描**：每轮 `min_element` 找最早到期事件。事件数少时可忽略，P2 换最小堆。
4. **未实现 DAQ 互斥的自动化用例**：`StartRejectsWhenDaqConfigured` 需要模拟从机完整应答
   FREE_DAQ/ALLOC_* 等命令，P3 一并补（本批已实现拒绝逻辑，仅缺用例）。
5. **统计无时间维度**：只给累计值，不给周期偏差的分布；P4 做容量标定时再补。
6. **`IsRunning()` 在 `noexcept` 内加锁**：若互斥量加锁抛 `std::system_error` 会 terminate；
   与既有同类访问器口径一致，未额外处理。
7. **回调阻塞会拖慢调度**：文档已声明回调不得阻塞，但未做运行期检测。

---

## 8. 自审

| 检查项 | 结论 |
|--------|------|
| 是否有占位符/TODO | 无 |
| 是否与计划签名一致 | 一致；`PollResult<void>` 落地为既有 `MeasurementResult<void>`（§4-8） |
| 是否只改必要文件 | 是：2 新增头 + 1 新增实现 + 1 测试 + 2 处注册 |
| 中文 Doxygen | 公开类型与全部公开方法均有 |
| 是否误用绝对路径 | 无 |
| 是否改 thirdparty | 否 |
| 是否有未验证的结论 | 实测数字全部来自本机运行结果（§6） |
| 已知限制是否写明 | 见 §7，逐条对应计划 §10 |