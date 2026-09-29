# libxcp 测量子系统 v0.9 实施记录 —— XCPlite 端到端

日期：2026-09-29 ｜ 计划依据：`code-plan/libxcp_测量子系统代码增长计划.md` §v0.9（:745-759）
目标：真实回环环境验证完整测量链路，**全程无测试专用手工拆包**；补齐交付示例。

---

## 1. 交付物清单

| # | 交付物 | 落点 | 状态 |
|---|--------|------|------|
| 1 | `xcplite_daq_test.cpp` 手工拆包替换 | `tests/xcplite_daq_test.cpp`（460 行） | ✅ `EnvelopeView`/`SplitEnvelope` 已删除，改 `MakeXcpliteEnvelopeLayout()` + `DecodeFrame()`（`DtoEnvelopeDecoder`），:31-33 注释记录来龙去脉 |
| 2 | L3 全链路集成测试 | `tests/xcplite_measurement_test.cpp`（364 行，新增） | ✅ 2 用例，注册进 `XcpliteIntegration`（`LIBXCP_BUILD_XCPLITE_SLAVE`+`LIBXCP_BUILD_A2L` 双门，`tests/CMakeLists.txt` :109 一带） |
| 3 | 端到端示例 | `examples/measurement_demo/`（cpp 453 行 + CMake 53 行） | ✅ `LIBXCP_BUILD_EXAMPLES` 门（根 `CMakeLists.txt` :231-247，需 SLAVE+A2L+TESTS） |
| 4 | 支撑性核心增量（追加式） | 见 §2 | ✅ 默认关闭，既有行为不变 |
| 5 | L4 手工冒烟清单 | 本 MD §6 | ✅ 文档化（不标 CI） |

## 2. 核心/适配层增量（全部为追加，不改动既有签名）

- **`DtoFrameLayout::timestamp_relative_first_only`**（`include/libxcp/daq/dto_envelope_types.hpp` :56-61，默认 false）：
  XCPlite 实然（D13）只对事件首 ODT 帧盖时间戳，而 v0.2 解码器的时间戳段是静态逐帧消费。
  新增门控在 `src/daq/dto_envelope_decoder.cpp` :128-146：`ts_present = timestamp_enabled &&
  !(first_only && type!=Absolute && identity.odt != first_odt)`。默认 false ⇒ v0.2/v0.6 语义与测试零变化。
  规格歧义（部分 ECU 逐帧盖章）⇒ 做成 opt-in 而非硬编码。
- **`MeasurementSession::SetTimestampFirstOdtOnly(bool)`**（hpp :150，Running 时抛 InvalidState，与 `SetTimestampUnit` 同纪律），WorkerLoop 透传至 layout（`src/measurement/measurement_session.cpp` :302）。
- **适配器 const 收窄**（计划外缺陷修正）：`A2lMeasurementDatabase` 构造改收 `const a2l::IA2lDatabase&`——
  `A2lBridge::Database()` 只暴露 const 指针，v0.5 写测试时才暴露签名不可用；L3 编译验证当场修复。
- **OnDto 漏唤醒（本里程碑最重要缺陷）**：`OnDto` 入队后从未 `notify_all`，worker 睡在 `cv.wait` 直到 Stop。
  单测（L2）靠"帧少且入队先于 wait"侥幸通过；L3 真流下暴露：`recv=4521 dropped=4265 decode_err=0`、零帧回调。
  修复：`src/measurement/measurement_session.cpp` OnDto :279-282 入队后 `m_run_cv_.notify_all()`（仍持锁唤醒，无丢帧竞态）。
  这就是 v0.6 §5 登记的两个 carry-over（唤醒缺失藏在"队列"语义里、时间戳宽度时序）之外的第三个真流才能暴露的缺口。

## 3. L3 测试设计（`tests/xcplite_measurement_test.cpp`）

- **FullChain**：`FetchA2lViaUpload()`（GET_ID IDT_ASAM_UPLOAD + UPLOAD，协议面取证、不读盘捷径）→ 落盘 run-dir 上传副本 →
  `A2lBridge::Load` → `A2lMeasurementDatabase` + **`EventBound` 装饰器**（`Find` 后补 `testev` 通道——适配器恒回 0，
  运行时事件绑定归上层，见 `adapter/a2l/a2l_measurement_database.cpp` :101-103；不改会话 API）→
  session 先构造作 listener → `Bind` → `Connect` → `SetEnvelopeMode(RelativeByte,4)` + `SetTimestampFirstOdtOnly(true)` +
  `SetTimestampUnit(1)` → Add `g_basic_u32`/`g_basic_u8` → Prepare → Start → 断言物理值 0xDEADBEEF/0x42（A2L 定标）、
  odt==0 帧 `timestamp_valid`、`decode_errors==0`。
- **SessionSeesLiveUpdates**：运行中 `WriteMemoryBytes` 改 `g_basic_u32` → 新帧必须体现新值（实时性）。
- **RAII 时序教训**（SEH 0xc0000005 复盘）：断言失败展开时 master 先于 session 析构 ⇒ Stop→StopDaq 悬垂。
  引入 `AutoStopSession` 守卫（声明于 master 之后、session 之前），示例同样以 `SessionGuard` 落实。

## 4. 验证结果

- **build-v09**（`-DLIBXCP_BUILD_TESTS=ON -DLIBXCP_BUILD_XCPLITE_SLAVE=ON -DLIBXCP_BUILD_A2L=ON
  -DLIBXCP_LIBA2L_ROOT=build/liba2l-prepared/msvc-x64-release`，liba2l 预备根由 `thirdparty/a2l-sdk/build-sdk.ps1`
  产出，首次可在本机编 A2L 门）：
  - `XcpliteIntegration` **31/31 全绿**（12.6s），含 XcpliteDaqTest 4/4（decoder 路径）与 XcpliteMeasurementTest 2/2；
  - `libxcp_tests` 420 → 419 pass / 1 skip（既有 Ag1_Cto8 不变）；`MeasurementAdapterTest` 6/6（首次本地实跑，v0.5 起一直门内未编译）；
  - build-v02 单元回归（notify 修复后）同样全绿 ⇒ 修复无行为回退。
- **A2lIsolation**：PASS——S1 扫描 21 个桥接层文件、S2 扫描 37 个主树文件，命中均为 0。
- **measurement_demo 实跑**：8/8 OK、exit 0。UPLOAD A2L 13588B；帧样例 `g_basic_u32=3735928559`、`g_basic_f32=3.14`、
  odt=0 帧 `ts_valid=1`；`received≈71-73 dropped=0 decode_err=0 wraps=0`；Stop 后非运行态。
  首跑 1 FAIL 为**示例验收口径写错**（"事件通道取证非零"）：XCPlite 运行时 A2L 的 `testev` 实然为 0x0
  （首个 `DaqCreateEvent` 即通道 0；规划器按无事件组处理），改为取证成功即通过 + info 打印实然值，非实现缺陷。

## 5. 已知限制（如实登记）

1. **时间戳宽度时序**（承 v0.6 §5，维持决策不变）：`Prepare` 在 `ConfigureDaqListsDynamic` 前读
   `DaqTimestampBytesCached()`，惰性 GET_DAQ_RESOLUTION_INFO 在 dynamic 门内才发（`src/xcp_master.cpp` :481）⇒
   首次 Prepare 可能读到 0。L3 环境实证：由上层显式注入（`SetEnvelopeMode`+`SetTimestampFirstOdtOnly`+
   `SetTimestampUnit`）即可正确工作；真实 ECU 场景按 §6 冒烟清单取证注入。重排预检会破坏规划预算与
   preflight 的一致性，维持现状。
2. 示例/L3 中 XCPlite 信封参数为**实然取证值硬注入**（D3 口径）；生产接入须走 `QueryDaqProcessorInfo/
   QueryDaqResolutionInfo` 取证，API 已具备（`xcp_master.hpp` :348-391）。
3. `examples/measurement_demo` 依赖 `xcp_test_slave` 目标（需 `LIBXCP_BUILD_TESTS=ON`），CMake 以
   FATAL_ERROR 明示，不静默降级。
4. Linux/macOS 两平台本机无法编译验证（Windows 宿主）；三平台回归列为 v1.0 已知限制。

## 6. L4 真 ECU 手工冒烟清单（不标 CI）

1. `Connect` → 记 `SessionParameters`（max_dto/byte_order/ag）；
2. `QueryDaqProcessorInfo` → 记 DAQ 能力位（DYNAMIC/TIMESTAMP）；
3. `QueryDaqResolutionInfo` → 记时间戳模式/分辨率（换算 ns/tick 后 `SetTimestampUnit` 注入）与 ODT 地址宽度；
4. 对目标事件 `QueryDaqEventInfo`（CMD_UNKNOWN ⇒ 回退 A2L EVENT 静态面）；
5. `SetEnvelopeMode` 按实然识别字段（RelativeByte 含 0xAA 标志 / RelativeWord / Absolute）；
6. `Add` 若干符号 → `Prepare` → 核对规划日志（list/odt/event 分布符合 ECU 容量）；
7. `Start` 收帧 ≥ 10min：断言 `decode_errors==0`、`timestamp_valid` 递增合理（wrap 计数与分辨率自洽）、
   统计接口与实际帧率相符；
8. 运行中 `Stop` → 复 `Start`：wrap 累计连续（`Reset` 保留 `WrapCount` 语义）；
9. `Disconnect` 后 ECU 侧 DAQ 表无残留（可用 `GET_DAQ_LIST_MODE`/再配置探测）。

## 7. 遗留到 v1.0

签名冻结审计、README「测量快速上手」、全量 ctest 回归、§25 封板清单闭环（含"长时间 DAQ 稳定性"未做项的
如实登记）、`code-plan/libxcp_测量子系统_v1.0_封板记录.md`。

## 8. 经验记录

- **单元/mock 通过 ≠ 真流正确**：notify 缺失这类并发缺陷只有真实帧率能暴露——L3 的存在意义即此。
- 断言口径必须来自被测实然（testev=0 基），先入为主写"非零"反而制造假缺陷。
- 测试/示例 RAII 顺序是 SDK 可用性的一部分：`SessionGuard` 模式已写入示例正文，供用户抄用。
