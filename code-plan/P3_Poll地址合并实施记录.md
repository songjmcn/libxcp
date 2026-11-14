# P3：Poll 地址合并与 DAQ 互斥补强实施记录

> 对应计划：`code-plan/Poll模式多Event架构实施计划.md` §9 批次 P3（含并入的 P2 技术判定）。
> 本记录只写真实改动与真实测试结果。记录日期：2026-11-14。

---

## 1. 批次范围

| 项 | 内容 |
|----|------|
| 计划要求（§9 P3） | 地址合并 + 错误隔离 + 统计 + 与 DAQ 的互斥校验 |
| 本批交付 | **地址合并**（新增）、DAQ 互斥补强（新增 `GET_STATUS.daq_running` 校验）；错误隔离与统计已在 P1 完成，本批回归验证 |
| 计划要求（§9 P2） | 多 event 调度（最小堆）+ 超期跳过 + 独立周期 |
| P2 处置 | 多 event 独立周期与超期跳过已在 P1 实现；**最小堆不实现**，理由见 §4-5 |

---

## 2. 设计依据

- **为什么合并**：poll 每个变量一次读取就是一次 UPLOAD 往返（`src/memory_access.cpp` 的
  `ReadElements` 优先 `SHORT_UPLOAD`，1 次往返）。N 个地址互不相邻的变量 = N 次往返；
  而 20 ms 周期内可用的往返预算由链路决定，因此**减少往返次数是 poll 唯一的性能杠杆**
  （实施计划 §4.1）。相邻变量合并成一次读取，可把 N 次降到 1 次。
- **为什么只在"首尾相接"时合并**：XCP 的 UPLOAD 是按地址的连续读。跨越空隙读会读到
  未声明的内存（可能是别的信号、也可能是受保护/非法区域），一旦越界还会触发从机错误。
  因此只在 `下一变量地址 == 当前组起始 + 当前组字节数` 时并入。
- **为什么校验 address extension**：同一个 32 位地址在不同 extension 下指向不同页/段
  （标定页切换即依赖这一点）。地址数值相接但 extension 不同必须视为两段独立内存。
- **为什么组失败要回退逐变量**：`ReadMemoryBytes` 要求字节数可被 AG 整除。若组总长度不满足
  AG 对齐，整组读会被拒——但组里每个变量本身可能都合法。回退逐变量能把影响面重新收敛到
  单变量，而不是让一个对齐问题拖垮整组。
- **DAQ 互斥双层校验**（实施计划 §3.5）：账本非空只能证明**本端**配置过 DAQ；
  若 DAQ 由别的会话配置并已启动，本端账本仍为空。因此 `Start` 再用 `GET_STATUS` 的
  `DAQ_RUNNING` 位确认从机实际状态。

---

## 3. 代码变更

| 文件 | 变更 |
|------|------|
| `include/libxcp/poll/poll_session.hpp` | 新增私有 `PollSlice`（`variable_index`/`offset`/`size`）与 `PollReadGroup`（`extension`/`address`/`byte_count`/`slices`）；`EventState` 增 `groups`；新增私有 `ReadSingle()` 与 `ConvertInto()` |
| `src/poll/poll_session.cpp` | `ResolveEvent` 末尾按 `(extension, address)` 排序后贪心归并首尾相接的变量；`RunTick` 改为按组读取并按切片换算，组读失败整组回退 `ReadSingle`；`Start` 增 `QueryStatus().daq_running` 校验 |
| `tests/poll_session_test.cpp` | `PollSlave` 增 `short_upload_count` / `fail_next_short_uploads` 计数与一次性故障注入；新增套件 `PollSessionAddressMerge` 4 例 |

---

## 4. 设计决策

1. **合并条件只有两个**：同一 `extension`，且地址严格首尾相接。不合并"距离很近"的变量，
   因为空隙里的内容不可知。
2. **32 位地址溢出禁止合并**：`last.address + byte_count` 用 `uint64_t` 计算，
   超过 `0xFFFFFFFF` 即另起一组，避免回绕后误判为连续。
3. **输出顺序与用户输入一致**：内部按地址排序读取，但结果先写进 `results[variable_index]`，
   最后整体 `move` 进 `PollSample::samples`。调用方看到的样本顺序永远是 `spec.symbols` 的顺序，
   不因合并而重排。
4. **组读失败整组回退**：见 §2，不用"只重读失败的变量"这种更细的恢复——失败原因可能是
   整组对齐问题，逐变量重读才能真正定位。
5. **P2 的最小堆不实现（技术判定）**：调度循环每轮做一次 `min_element` 线性扫描。
   实际事件数是用户配置的个位数量级，扫描耗时在纳秒级；而一次 UPLOAD 往返在百微秒量级，
   两者差三个数量级，换成最小堆**没有可测量收益**，只会增加惰性删除等实现复杂度。
   因此保留线性扫描，并把"事件数达到数百时需要重新评估"写入边界（§7-3）。
6. **DAQ 互斥仍不做双向锁**：`XcpMaster` 不应反向依赖 `PollSession`（会形成循环依赖），
   poll 运行中调用 `StartDaqSync()` 仍由调用方负责先 `Stop()`（实施计划 §3.5 已定）。

---

## 5. 测试

原 P1 的 15 例（`PollSessionSkeleton`）全部保留并回归通过；新增 `PollSessionAddressMerge` 4 例：

| # | 用例 | 断言要点 |
|---|------|----------|
| 1 | `AdjacentVariablesShareOneRead` | 4 个相邻 1 字节变量 → `SHORT_UPLOAD` 只到达 **1 次**，四值全部正确、顺序不变 |
| 2 | `NonAdjacentVariablesAreReadSeparately` | 取 `0x1000` 与 `0x1002`（空出 `0x1001`）→ **2 次**读取，不跨空隙合并 |
| 3 | `DifferentExtensionsAreNotMerged` | 地址相接但 extension 不同 → **2 次**读取 |
| 4 | `MergedReadFailureFallsBackPerVariable` | 首次（合并）读注入 `ERR_OUT_OF_RANGE` → 回退 4 次单读，四变量仍 `valid=true`，总读取 **5 次** |

用例 1 与 4 直接用从机侧的 `SHORT_UPLOAD` 到达次数做断言——这是"是否真的合并"的唯一
客观证据，比断言内部结构更可靠。

---

## 6. 实测结果（本机 Release）

| 项 | 结果 |
|----|------|
| 构建 | `cmake --build cmake-build-release --target libxcp_tests` → `EXIT=0` |
| 本仓库源码告警 | **0**（`p3_build.log` 中 `warning` 计数为 0） |
| Poll 用例 | `--gtest_filter='PollSession*'` → **19 tests / 19 passed / 167 ms**（15 + 4） |
| 全量回归 | `libxcp_tests.exe` → **997 tests / 996 passed / 1 skipped / 0 failed / 46628 ms** |
| 跳过项 | `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`（P0 之前即存在） |

用例数 993（P1 后）→ 997，增量 4 与新增用例数一致。

---

## 7. 边界与遗留

1. **合并长度未按单包上限切分**：组总长超过单次 `SHORT_UPLOAD` 容量时，`ReadMemoryBytes` 内部
   会退化为 `SET_MTA` + 分块 `UPLOAD`（每块 2 次往返）。当组内变量数 N 与分块数 M 满足
   `N < 2M` 时，合并反而比逐变量多一次往返。当前按最坏情况不做预判，后续可在知道
   `MAX_CTO/AG` 后限制组大小。
2. **数组仍不支持**：`element_count != 1` 在 `ResolveEvent` 直接拒绝。
3. **线性扫描的规模边界**：事件数达到数百时 `min_element` 才可能有可测开销；届时再评估最小堆。
4. **DAQ 互斥的自动化用例仍缺**：`StartRejectsWhenDaqConfigured` 需要模拟从机完整应答
   `FREE_DAQ`/`ALLOC_*` 等命令；本批已实现并接入校验逻辑，仅缺用例。
5. **合并只覆盖同一事件内**：不同事件之间不共享读取，即使地址相同也不去重（跨事件合并会
   引入调度耦合，且需求尚未出现）。
6. **`QueryStatus()` 增加 Start 时的一次往返**：只在启动发生，不影响周期内预算。

---

## 8. 自审

| 检查项 | 结论 |
|--------|------|
| 是否有占位符/TODO | 无 |
| 是否跨空隙读取 | 否，只在地址严格首尾相接且 extension 相同时合并（用例 2/3 覆盖） |
| 输出顺序是否稳定 | 是，按 `spec.symbols` 顺序输出（用例 1 校验） |
| 合并失败是否影响其他变量 | 否，整组回退逐变量（用例 4 覆盖） |
| 中文 Doxygen | 新增类型与私有方法均有 |
| 是否改 thirdparty | 否 |
| 是否有未验证的结论 | 实测数字全部来自本机运行（§6） |
| 是否记录了不做的决定 | 是（§4-5 最小堆） |