# Poll 模式数组支持 实施记录

- 日期：2026-11-14
- 任务来源：用户要求「先修复数组类型不支持多 event 的 poll 模式」，随后再定多 event 更新计划。
- 账本任务：`poll-array-support`

## 1. 问题与范围

`PollSession` 的 poll 路径原本只接受标量：`ResolveEvent()` 与 `Validate()` 都在
`MeasurementSymbolInfo::element_count != 1` 时直接拒绝，错误文本为
「poll 路径暂只支持标量，不支持数组读取」。

这会让「把 `xcp_master` 的轮询切到 `PollSession` 以支持多 event 不同周期」变成功能回退——
`xcp_master` 现有的 poll 路径通过 `ReadObservedArray` 是**支持数组**的。

本次修复把 poll 路径补到与 DAQ 路径同等的数组能力：读整数组、逐元素换算、失败隔离。
**不在本次范围内**：多 event 的命令行/配置接线、UI、周期分配（属后续 P6 阶段）。

## 2. 设计决策

| 编号 | 决策 | 理由 |
| --- | --- | --- |
| D1 | `PollVariableSample` 增加 `is_array` + `elements`，保留 `value` | 与 DAQ 路径 `MeasurementSample{is_array, elements}` 同构；标量消费方读 `value` 零改动 |
| D2 | 一次 tick 的读取量统一用 `byte_count = element_count × element_size_bytes` | 标量与数组走同一条「读组归并 / 读取」代码，避免两套分支漂移 |
| D3 | 布局合法性由 `ComputePollByteCount()` 统一判定 | 宽度 0、元素数 0、乘积溢出三种非法情况一处处理，`ResolveEvent` 与 `Validate` 结论必然一致 |
| D4 | 数组换算逐元素调 `ToPhysicalElement`，任一失败则整条样本无效并清空 `elements` | 与 DAQ 路径完全一致：不对外暴露部分结果，消费方不会读到「前 2 个元素是新的、后 2 个是旧的」 |
| D5 | `is_array` 在 `RunTick()` 里按布局填，而不是按换算结果填 | 换算失败时消费方仍需知道这个变量是数组，否则会把空 `elements` 误判成标量 |

### 适配器已具备的能力（本次未改动）

`A2lMeasurementDatabase::ToPhysicalElement()` 已实现逐元素换算，且
`A2lMeasurementDatabase` 在构造时强制 `dimension.byte_stride == element_size_bytes`，
总字节数按 `extent × element_size_bytes` 计算——即**数组元素连续、无 padding**，
poll 按 `element_size_bytes` 步进是安全的。这是 D2 成立的前提。

## 3. 代码改动

### 3.1 `include/libxcp/poll/poll_types.hpp`

`PollVariableSample`（`poll_types.hpp:42`）新增两个字段：

- `bool is_array{false};`（`poll_types.hpp:53`）——一元素数组也为 true；
- `std::vector<MeasurementValue> elements;`（`poll_types.hpp:57`）——仅整条换算全部成功时填充。

### 3.2 `include/libxcp/poll/poll_session.hpp`

- `ResolvedVariable` 新增 `std::size_t byte_count{0};`（`poll_session.hpp:123`），
  在 `ResolveEvent` 里算好，读组归并与读取只依赖它；
- `PollSlice::size` 注释由「= `element_size_bytes`」改为「= `byte_count`」（`poll_session.hpp:131`）；
- `Validate()` 文档由「只支持标量」改为「标量与数组都支持」（`poll_session.hpp:88-91`）。

### 3.3 `src/poll/poll_session.cpp`

| 位置 | 改动 |
| --- | --- |
| `poll_session.cpp:20-45` | 新增文件内匿名 namespace 的 `ComputePollByteCount(info, byte_count)`：宽度/元素数非 0 且乘积不溢出 `size_t` |
| `poll_session.cpp:108-125` | `ResolveEvent()` 去掉数组拒绝，改为调 `ComputePollByteCount()` 并写入 `variable.byte_count` |
| `poll_session.cpp:146` | 读组归并的步长由 `info.element_size_bytes` 改为 `state.variables[index].byte_count` |
| `poll_session.cpp:179-190` | `Validate()` 同样只校验布局，不再拒绝数组 |
| `poll_session.cpp:323-372` | `ConvertInto()` 增加数组分支：按 `width × count` 切片，逐元素 `ToPhysicalElement`；任一失败清空 `elements` 并置 `valid=false` |
| `poll_session.cpp:376-388` | `ReadSingle()` 按 `byte_count` 读整数组，并加 `ByteCount` 上限检查 |
| `poll_session.cpp:406-410` | `RunTick()` 从布局设置 `results[i].is_array` |

## 4. 测试

`tests/poll_session_test.cpp`：

| 用例 | 覆盖点 |
| --- | --- |
| `ValidateAcceptsArraySymbol`（原 `ValidateReportsArraySymbolAsUnsupported` 改写） | 数组不再被 `Validate()` 拒绝 |
| `ArraySymbolProducesElementSamples`（新增） | 数组符号一次 tick 产出 `is_array=true`、`elements.size()==4`、逐元素物理值正确 |
| `ArrayElementFailureDoesNotLeakPartialResult`（新增） | 第 2 个元素换算失败 → 整条无效、`elements` 为空；**同 tick 的标量变量仍 valid** |

`FakeDb` 补充 `ToPhysicalElement()` 覆写与 `fail_element` 注入点（`std::atomic<int>`），
用于按元素下标制造换算失败。

## 5. 验证结果

| 命令 | 结果 |
| --- | --- |
| `cmake --build cmake-build-release --target libxcp_tests -j 4` | 4/4 通过，零错误 |
| `libxcp_tests.exe --gtest_filter="PollSession*"` | **25/25 通过**（1.49 s） |
| `libxcp_tests.exe --gtest_filter="*Array*"` | **10/10 通过**（其中 poll 3 例） |
| `libxcp_tests.exe`（全量单元） | **1002 通过 / 1 skipped / 1 disabled** |
| `ctest --output-on-failure`（全量） | **1477/1477 通过，0 失败**（618.33 s） |

全量 ctest 的 6 项未运行均为既有 Disabled/Skipped，与本次改动无关：
`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`(Disabled)、`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`、
`SnapshotTest.StagesReadableCopyOfSource`、`SnapshotTest.CreatesDistinctFilesPerCall`、
`SnapshotTest.DeletesSnapshotWhenGuardReleased`、`XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`。

> 说明：`PollSession` 相关用例本次为 **25 例**（改写 1 例 + 新增 2 例）；全量单测本次为 1002 通过 + 1 skipped + 1 disabled。

## 6. 已知限制

1. 数组读取按 `element_count × element_size_bytes` 一次 `UPLOAD`。容量上限取决于
   `XcpMaster::ReadMemoryBytes` 的 `ByteCount` 与 ECU 最大 CTO——超长数组会像标量
   超长读一样失败并按错误隔离处理，不会自动拆分成多次读。
2. 只按 `byte_stride == element_size_bytes` 的紧凑布局设计（适配器当前强制该约束）。
   若将来引入带 padding 的数组，`ComputePollByteCount` 需要改为按 stride 计算。
3. `PollVariableSample::value` 在数组样本中保持默认值，消费方必须判 `is_array`
   决定读 `value` 还是 `elements`——这是 D1 的既有取舍，未提供「数组也填 value[0]」的兼容。
4. 多 event 不同周期的命令行/配置接线仍未做（本次只解除数组阻塞）。

## 7. 复现命令

```bash
# 构建
cmake --build cmake-build-release --target libxcp_tests -j 4

# 定向
./cmake-build-release/tests/libxcp_tests.exe --gtest_filter="PollSession*"

# 全量单元
./cmake-build-release/tests/libxcp_tests.exe
```

## 8. 改动文件

- `include/libxcp/poll/poll_types.hpp`
- `include/libxcp/poll/poll_session.hpp`
- `src/poll/poll_session.cpp`
- `tests/poll_session_test.cpp`
- `code-plan/Poll数组支持_实施记录.md`（本文件）
