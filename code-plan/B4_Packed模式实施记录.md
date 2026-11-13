# B4 批次实施记录：DAQ Packed Mode 全链路

> 对应计划：`code-plan/XCP_V1.5_测量标定修复与测试实施计划.md` §3 B4
> 设计依据：`code-plan/B4_Packed模式设计方案.md`（方案 A，用户已批准）
> 规范权威：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`
> （Markdown 版 `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md` 仅用于定位）
> 实施日期：2026-11-04　分支：`feate_daq_all`

---

## 1. 批次范围

计划 §3 B4 要求的四件事，逐项落地：

| # | 要求 | 落地情况 |
|---|---|---|
| 1 | `SET_DAQ_PACKED_MODE`(0xC0,0x01) / `GET_DAQ_PACKED_MODE`(0xC0,0x02) 编码与解析 | 完成（第 3 节 Codec/Parser 行） |
| 2 | `DaqPackedMode`/`DaqPackedTimestampMode` 状态、`CanUseDaqPackedMode` 属性取证、协商与回读确认 | 完成（第 3 节 Master 行） |
| 3 | 规划层按「采样数 × 单采样宽度」折算容量、给出每路由 `packed_stride` | 完成（第 3 节 Planner 行） |
| 4 | 数据面：一个 packed DTO → S 个 `MeasurementFrame`，按事件周期×降频因子重建非锚点时间戳 | 完成（第 3 节 Session 行） |

**本批不做**（与原计划一致）：`GET_DAQ_CLOCK_MULTICAST`、跨链路 Time Sync Bridge、
多 Slave Cluster/Grandmaster 策略、A2L 侧 packed 声明推导（B5 范畴）。

---

## 2. 规范核对要点

| 主题 | 规范位置 | 结论（编码时逐条遵循） |
|---|---|---|
| packed 概念与两种分组 | §4.1.10 `:791-878` | element-grouped = 每元素连续 S 个采样；event-grouped = 整组采样连续排列；**不同 ODT 不混排** |
| 时间戳重建 | `:830-878` | 一帧只有一个时间戳；`DPM_TIMESTAMP_MODE=0` 锚在末采样、`=1` 锚在首采样；第 k 个采样偏移 = `(S-1-k)` 或 `k` 个 `EventPeriod × Prescaler` |
| 错误处理 | `:880-890` | 丢采样则整组丢弃重收；**收到的一帧总是完整 S 个连续采样**（因此不做"半组补齐"） |
| SET 布局 | `:4655-4712` Table 173 | CTO=`[0xC0][0x01][DAQ_LIST WORD][DPM_PACKED_MODE][DPM_TIMESTAMP_MODE][DPM_SAMPLE_COUNT WORD]`，共 8 字节；`mode=0` 时后三字节被忽略 |
| `DPM_SAMPLE_COUNT` | 同上 | 合法范围 **[2, 0xFFFF]**，0 与 1 非法 |
| 负响应 | `:4714-4728` | 组合不支持 → `ERR_MODE_NOT_VALID`；列表不支持 → `ERR_OUT_OF_RANGE` |
| GET 布局 | `:4730-4752` Table 174/175/176 | CTO=`[0xC0][0x02][DAQ_LIST WORD]`；RES mode=0 为 `[0xFF][reserved][mode]`（剥 PID **2** 字节），mode=1/2 为 `[0xFF][reserved][mode][ts_mode][DPM_SAMPLE_COUNT WORD]`（剥 PID **5** 字节） |
| 能力位 | Table 172 `:5407-5412` / Table 155 `:5188-5194` | `DAQ_LIST_PROPERTIES` bit4 PACKED、`DAQ_EVENT_PROPERTIES` bit4 PACKED |
| 事件周期单位 | `:5236-5251` Table 156 | `EVENT_CHANNEL_TIME_UNIT` 在**低半字节**（0=1ns…9=1s，10..12 亚纳秒），与前缀时间戳单位的高半字节编码**不是同一张表** |
| 复位语义 | `:4529` | `CLEAR_DAQ_LIST` 把列表复位为 non-packed |

**第二次踩到同一类陷阱**：规范表格里的字节数常**含 0xFF PID**。
B3 的 `TIME_CORRELATION_PROPERTIES`（含 PID 8 / 剥 PID 7）已踩过，
本批 `GET_DAQ_PACKED_MODE` 同样按「剥 PID」计长，故常量为 2 与 5 而非 3 与 6。

---

## 3. 代码变更（五层）

### 3.1 协议层 `include/libxcp/protocol_types.hpp`

| 位置 | 内容 |
|---|---|
| `:529` | `DaqListPropertyBit::kPacked = 0x10`（Table 172 bit4） |
| `:546-559` | 新增 `enum class DaqEventPropertyBit{kNone, kDaq=0x04, kStim=0x08, kPacked=0x10}` + `operator|` + `HasDaqEventProperty` |
| `:566-587` | `enum class DaqPackedMode{NotPacked, ElementGrouped, EventGrouped}`、`DaqPackedModeName`、`ToDaqPackedMode`（3..255 → nullopt） |
| `:589-600` | `enum class DaqPackedTimestampMode{LastSample=0, FirstSample=1}`、`ToDaqPackedTimestampMode`（bit1-7 置位 → nullopt） |
| `:623-655` | `EventChannelTimeUnitNanoseconds(unit_code)`：取低 4 位映射到纳秒；13..15 → nullopt；10..12 亚纳秒按 1ns 向上取整 |
| `:657-659` | `kMinDaqPackedSampleCount = 2`、`kMaxDaqPackedSampleCount = 0xFFFF` |
| `:1486-1496` | 上述 constexpr 定义 |

`src/protocol_types.cpp`：`DaqPackedModeName` 返回 `"DAQ_PACKED_MODE_NONE"` /
`"_ELEMENT_GROUPED"` / `"_EVENT_GROUPED"` / `"_UNKNOWN"`（未知值不编造语义）。

### 3.2 编码层 `include/libxcp/command_codec.hpp` / `src/command_codec.cpp`

| 位置 | 内容 |
|---|---|
| `.hpp:319-328` | `EncodeSetDaqPackedMode(daq_list, mode, timestamp_mode, sample_count)`、`EncodeGetDaqPackedMode(daq_list)` |
| `.cpp:372-419` | SET 生成 8 字节；packed 时校验 S∈[2,0xFFFF]，`NotPacked` 时拒绝非默认 ts/S 并把尾部三字节写 0 |
| `.cpp:420-425` | GET 生成 4 字节 |

### 3.3 解析层 `include/libxcp/response_parser.hpp` / `src/response_parser.cpp`

| 位置 | 内容 |
|---|---|
| `.hpp:133-140` | `struct DaqPackedModeResponse{mode, timestamp_mode, sample_count}` |
| `.hpp:565` | `ParseDaqPackedMode(BytesView)` 声明 |
| `.cpp:39-41` | `kDaqPackedModeResMinSize=2`、`kDaqPackedModeResPackedSize=5` |
| `.cpp:390-419` | mode=0 只读 2 字节；mode=1/2 需 5 字节；未定义 mode 或保留 ts 位 → nullopt |

### 3.4 执行器层 `include/libxcp/command_executor.hpp` / `src/command_executor.cpp`

| 位置 | 内容 |
|---|---|
| `.hpp:472-483` | `ExecuteSetDaqPackedMode`、`ExecuteGetDaqPackedMode` |
| `.cpp:1537-1560` | SET：`IsCmdUnknown` → 抛 `UnsupportedFeature`（Level-1 子命令 0x01 未实现） |
| `.cpp:1561-1590` | GET：`IsCmdUnknown` → nullopt；短帧 → `MalformedPacket` |

两者都经 `RunCommand(CommandCode::GetVersion, encoded)` 发送——Level-1 双字节命令
共用 0xC0 前缀，`CommandCode` 里没有单独的码位，复用 `GetVersion` 才能让
`Parse` → `DispatchResponse` 的预期命令匹配。

### 3.5 主控层 `include/libxcp/xcp_master.hpp` / `src/xcp_master.cpp`

| 位置 | 内容 |
|---|---|
| `.hpp:163-175` | `struct DaqPackedModeNegotiation{bool active; DaqPackedModeResponse mode; std::string reason;}` |
| `.hpp:712-745` | `CanUseDaqPackedMode` / `QueryDaqPackedMode` / `SetDaqPackedMode` |
| `.hpp:917` | `UnsupportedCapability::DaqPackedMode` 文档改为"不再是静态边界" |
| `.cpp:331-350` | `IsCapabilityUnsupported(DaqPackedMode)` 返回 **false**（其余三项仍 true） |
| `.cpp:1899-1932` | `CanUseDaqPackedMode`：列表 bit4 **且** 事件 bit4 **且** `time_cycle != 0`；取证不可用即 false |
| `.cpp:1934-1937` | `QueryDaqPackedMode`：纯 GET，nullopt 透传 |
| `.cpp:1939-2005` | `SetDaqPackedMode`：本地校验 S → **列表 PACKED 能力前置判定** → SET → GET 回读逐项比对（mode/S/ts）→ 全一致才 `active=true`；任何 `XcpException` 只写入 `reason`，不上抛 |

`IsCapabilityUnsupported` 翻转是**语义变更**：Packed Mode 从"确定不提供"变成
"运行时取证后可能提供"。为此同步修改了 `tests/xcp_master_integration_test.cpp`
的旧断言（见 §5.3）。

### 3.6 规划层 `include/libxcp/measurement/measurement_planner.hpp` / `.cpp`

| 位置 | 内容 |
|---|---|
| `.hpp:42` | `MeasurementRoute::packed_stride`（0 = 非 packed） |
| `.hpp:93-94` | ctor 尾部两个参数 `packed_mode` / `packed_sample_count` |
| `.cpp:56` | `sample_multiplier = packed ? S : 1` |
| `.cpp:201-209` | 单变量超容量判定改为 `byte_width × sample_multiplier > usable` |
| `.cpp:260-283` | 新增 `close_odt(Active&)`：ODT 关闭时回填 `packed_stride`（ElementGrouped → Entry 宽度；EventGrouped → 整个 ODT 单采样宽度） |
| `.cpp:284/315/328` | 开新 ODT、开新 List、Build 收尾三处调用 `close_odt`；累计宽度按 `byte_width × sample_multiplier` |

### 3.7 会话层 `include/libxcp/measurement/*`

| 位置 | 内容 |
|---|---|
| `measurement_sample.hpp` | `enum class MeasurementTimestampSource{None, Dto, Reconstructed}`；`MeasurementFrame` 增 `sample_index` / `sample_count` / `timestamp_source` |
| `measurement_session.hpp:238-249` | `m_packed_mode_` / `m_packed_timestamp_mode_` / `m_packed_sample_count_` / `m_packed_active_` / `std::map<std::uint16_t, PackedListInfo> m_packed_lists_` |
| `measurement_session.hpp:281` | `OdtSlice::packed_stride`（**必须放在结构体最后**） |
| `measurement_session.cpp:168-186` | `SetDaqPackedMode`：`InvalidState` 拒绝运行中调用、`InvalidArgument` 拒绝非法 S 与保留 ts 语义 |
| `measurement_session.cpp:300-357` | `Prepare()` 协商：列表号 < 256；事件周期与单位可解析；`master.SetDaqPackedMode` 必须 `active`；算出 `sample_interval_ns = time_cycle × unit_ns × prescaler` |
| `measurement_session.cpp:621-707` | worker：每 DTO 只换算一次锚点时间戳，再按 `sample_index` 切片产出 S 个 frame |
| `measurement_session.cpp:690-705` | 时间戳：`steps=0` → 原值（`Dto`）；否则按 `steps × interval` 加减，减法前判下溢（不足则 `timestamp_valid=false`） |

---

## 4. 设计决策

| # | 决策 | 理由 |
|---|---|---|
| 4.1 | 一 DTO → S 个 `MeasurementFrame`（方案 A） | 回调契约不变（1 frame = 1 个采样时刻）；用 `sample_index`/`sample_count` 让调用方知道它们来自同一 DTO，而不是解析第二个信封 |
| 4.2 | 时间戳**每 DTO 只换算一次** | 回卷延展器按「每帧一次递增」设计；对同一帧的 S 个采样重复喂同一个 raw 值会把回卷计数打乱 |
| 4.3 | 减法下溢时置 `timestamp_valid=false` 而非钳到 0 | 首采样锚点减去 S-1 个周期可能早于时钟零点；钳位会造出一个"看起来合法"的假时间 |
| 4.4 | 协商失败一律 `UnsupportedFeature` + 具体原因，**绝不降级为普通布局** | 按 packed 配置、按普通 DTO 解码会得到静默错位的物理量——这是最坏的失败模式 |
| 4.5 | `SetDaqPackedMode` 先做列表 PACKED 前置判定，再发 SET | 规范说列表不支持回 `ERR_OUT_OF_RANGE`；注定失败的帧不必发，也让"不支持"与"暂不可读"分开 |
| 4.6 | `IsCapabilityUnsupported(DaqPackedMode)` 翻转成 false | 该枚举表达"本库有无编码路径"，B4 之后有；"对端能否用"是另一维，交给 `CanUseDaqPackedMode` |
| 4.7 | 采样序号按 8 位表达，故要求 DAQ List 号 < 256 | `MeasurementFrame::sample_index` 是 `uint8_t`；限制在协商前即可失败的层面，而不是静默截断 |
| 4.8 | `OdtSlice::packed_stride` 放结构体最后 | 放中间会让既有聚合初始化逐字段错位，实测报 `C2397 从 const size_t 转换到 uint8_t 需要收缩转换` |

---

## 5. 测试与实测

新增 `tests/daq_packed_mode_test.cpp`（注册于 `tests/CMakeLists.txt:87`），5 个套件共 **27** 个用例：

### 5.1 用例清单

| 套件 | 用例 | 断言要点 |
|---|---|---|
| Codec | `SetFrameIsEightBytesInIntelOrder` | `[C0][01][34][12][02][01][07][00]` |
| | `SetFrameFollowsMotorolaOrder` | 大端 WORD 布局 |
| | `GetFrameIsFourBytes` | `[C0][02][34][12]` |
| | `RejectsSampleCountBelowTwo` | S=0/1 抛异常 |
| | `RejectsReservedTimestampMode` | ts=0x02 抛异常 |
| | `NotPackedZeroesTrailingBytes` | mode=0 时尾部三字节为 0 |
| Parser | `NotPackedResponseIsTwoBytes` | 2 字节可解析 |
| | `PackedResponseCarriesTimestampAndCount` | mode/ts/S 三字段 |
| | `PackedResponseFollowsMotorolaOrder` | 大端 S=0x0102 |
| | `RejectsUndefinedMode` | mode=3 → nullopt |
| | `RejectsReservedTimestampMode` | ts=0x02 → nullopt |
| | `ShortPackedResponseIsNullopt` | mode=1 只给 4 字节 → nullopt |
| Master | `QueryReportsSlaveValues` | GET 回读解出 mode/ts/S |
| | `QueryNulloptOnlyWhenCommandUnknown` | `ERR_CMD_UNKNOWN` → nullopt |
| | `CanUseRequiresListEventAndPeriod` | 列表位/事件位/周期三者缺一即 false |
| | `SetRequiresMatchingReadback` | 回读不一致 → `active=false` + `reason` |
| | `SetRejectsWhenSlaveRefusesMode` | SET 被拒 → `active=false` |
| | `SetRejectsInvalidSampleCountLocally` | 一帧都不发 |
| | `NotPackedIsAcceptedAsReset` | 复位到 non-packed 不要求能力位 |
| Planner | `CapacityIsMultipliedBySampleCount` | 8B×2 超 9B 可用 → 2 个 ODT；stride=4 |
| | `EventGroupedStrideIsWholeOdtWidth` | stride=整个 ODT 单采样宽度 |
| | `RejectsInvalidSampleCount` | S=1 → `DaqConfigurationError` |
| Session | `SplitsOneDtoIntoSampleFrames` | 1 DTO → 2 frame，末采样锚点 1000ns、首采样重建为 0 |
| | `ReconstructsFromFirstSampleAnchor` | 首采样锚点 5000/6000/7000 |
| | `RejectsWhenEventIsNotPeriodic` | `time_cycle=0` → `Prepare()` 抛 |
| | `RejectsWhenEventLacksPackedProperty` | 事件/列表缺 PACKED 位 → `Prepare()` 抛 |
| | `RejectsInvalidConfigurationLocally` | 运行前本地拒绝 S=0/1 |

### 5.2 实施过程中修掉的四个真实缺陷

| # | 现象 | 根因 | 修正 |
|---|---|---|---|
| 5.2.1 | 编译报 `error C2397: 从"const size_t"转换到"uint8_t"需要收缩转换`（`measurement_session.cpp:387`） | `OdtSlice::packed_stride` 插在 `is_array` 之后，与既有的逐字段聚合初始化错位 | 把字段移到结构体最后 |
| 5.2.2 | `error C7595`：`LIBXCP_LOG_WARNING` 不能接受运行期拼接字符串 | 该宏基于 `std::format`，格式串必须是常量表达式 | 改为 `LIBXCP_LOG_WARNING("...{}...", arg)` 形式 |
| 5.2.3 | `CanUseRequiresListEventAndPeriod` 之外的 `RejectsWhenEventLacksPackedProperty` 首版不抛 | `SetDaqPackedMode` 只校验本地参数就发 SET，模拟 Slave 照单全收 | 增加列表 PACKED 前置判定（决策 4.5） |
| 5.2.4 | `QueryNulloptOnlyWhenCommandUnknown` 抛 `命令 0xFD 收到 ERR ERR_CMD_UNKNOWN` | 该用例的模拟 Slave 只应答 CONNECT，连 `Connect()` 必需的 `GET_STATUS` 都回了 `ERR_CMD_UNKNOWN` | 让模拟 Slave 同时应答 `GET_STATUS` |

### 5.3 旧断言更新

`tests/xcp_master_integration_test.cpp`：

- `DaqPackedModeIsBlockedLocally` → **`DaqPackedModeIsDecidedAtRuntime`**：
  `EXPECT_FALSE(IsCapabilityUnsupported(DaqPackedMode))`，
  其余三项仍 `EXPECT_TRUE`。
- `ThrowIfUnsupportedRaisesUnsupported`：对 `DaqPackedMode` 改为
  `EXPECT_NO_THROW(...)`，其余三项仍断言抛。

### 5.4 实测（Release，本轮真实输出）

```
构建：.tmp-build-tests.bat（vcvars64 + cmake --build cmake-build-release -j 4 --target libxcp_tests）
      EXIT=0，库与测试目标均通过；构建日志中无本仓库源码告警
      （仅 glog 头文件 C4244/C4996 第三方告警）

B4 专项：DaqPackedMode*  → 27 / 27 通过（25 ms）

全量：cmake-build-release/tests/libxcp_tests.exe
      [==========] 966 tests from 165 test suites ran. (46543 ms total)
      [  PASSED  ] 965 tests.
      [  SKIPPED] 1 test  （AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8，B0 之前即跳过）
      [  FAILED ] 0 tests
```

批次间总量演进：B3 结束 939 → B4 新增 27 例 = 966。

`git diff --stat`：20 文件、+4061 / −152（含 B0–B4 全部改动；B4 自身见 §3）。

---

## 6. 边界与遗留

| # | 遗留 | 影响 | 后续 |
|---|---|---|---|
| 6.1 | 采样时间戳**依赖事件周期**；非周期事件（`time_cycle==0`）整体拒绝 packed | 对端把测量挂在非周期事件上时无法用 packed | 规范允许拒绝，不做猜测性插值 |
| 6.2 | `sample_index` 是 8 位，故限制 DAQ List 号 < 256 且实际采样数按 `min(S,255)` 记录 | S>255 时 `sample_count` 字段被截断（帧数仍按实际 S 拆分，见下条） | 6.3 一起收紧 |
| 6.3 | `sample_count` 字段同样是 `uint8_t`，S>255 时截断 | 调用方拿不到完整 S | 需扩展为 `uint16_t`（接口变更，留待评审） |
| 6.4 | 未处理 packed 下的 DTO 计数缺口语义 | 计数器表示"采样组序号"还是"采样序号"，规范未要求，未定义 | 需对端实测后再定 |
| 6.5 | 未校验 `MAX_DTO` 上 packed 帧是否真的按 S 倍宽校验 | 由规划层按 S 倍容量装箱，会话层不再复核对端声明 | 若对端 `max_dto` 与实际不符，属对端违约 |
| 6.6 | 未实现 `GET_DAQ_CLOCK_MULTICAST` 与 Time Sync Bridge 下的 packed | 超出本批范围（B3/B4 明确排除） | 需要集群级时间同步设计 |
| 6.7 | A2L 侧尚无 packed 声明推导 | 用户需显式调用 `SetDaqPackedMode` | B5 复杂对象批次 |
| 6.8 | `EventChannelTimeUnitNanoseconds` 对 10..12（亚纳秒）按 1ns 向上取整 | 时间戳分辨率不足 1ns 时会有量化误差 | 规范未定义亚纳秒场景下的 DAQ 时间戳，保持显式近似 |

---

## 7. 自审

| 检查项 | 结论 |
|---|---|
| 每个新公开 API 都有中文 Doxygen | 是（`SetDaqPackedMode`/`CanUseDaqPackedMode`/`QueryDaqPackedMode`/`Encode*`/`Parse*`/`Execute*`） |
| 新接口是否配了测试 | 是，27 例覆盖编解码/解析/协商/规划/会话五层 |
| 是否有"静默降级"路径 | 无：协商失败一律 `UnsupportedFeature`，时间戳不可重建则 `timestamp_valid=false` + `source=None` |
| 是否改动 thirdparty | 否 |
| 是否引入绝对路径 | 否（构建脚本为临时文件，已删除） |
| 是否与既有行为兼容 | 默认 `NotPacked`，不调用 `SetDaqPackedMode` 时路径与 B3 完全一致（939 例旧用例全绿） |
| 计划 §7 的"风险清单"是否有未覆盖项 | 唯一遗留是 6.2/6.3 的 8 位宽度，已在边界表登记，未隐藏 |
| 文档与代码是否一致 | 本文 §3 行号均来自本轮 `grep -n` 实测，非记忆 |
