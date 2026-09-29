# libxcp 测量子系统 v0.5 实施记录 —— MeasurementSession + A2L 适配器

> 日期：2026-09-29　对应计划：`code-plan/libxcp_测量子系统代码增长计划.md` §5.5 / §5.6
> 前置里程碑：v0.2（DtoEnvelopeDecoder）、v0.3（GET_DAQ_EVENT_INFO）、v0.4（MeasurementPlanner）均已完成并通过全量回归。

## 1. 本步目标

打通"Add → Prepare → Start(回调) → DTO 帧 → 解码 → MeasurementFrame 回调"的测量主链路：

- `MeasurementSession`：测量会话核心（绑定 Master、装配计划、动态 DAQ 配置、DTO 消费线程、统计）。
- `DtoPayloadDecoder`：DTO 载荷按 ODT 路由切片（核心侧，不依赖 A2L）。
- `A2lMeasurementDatabase`（适配器）：把 `calmcar::xcp::a2l::IA2lDatabase` 适配为核心窄接口 `IMeasurementDatabase`。
- L2 测试（脚本化 Slave + 真实线程链路）12 例；L3 适配器测试 6 例（A2L 门控目标）。

## 2. 新增 / 修改文件清单

新增：
| 文件 | 说明 |
|---|---|
| `include/libxcp/measurement/measurement_sample.hpp` | `MeasurementSample` / `MeasurementFrame` / `MeasurementCallback` |
| `include/libxcp/measurement/measurement_session.hpp` | `MeasurementSession`（193 行） |
| `include/libxcp/daq/dto_payload_decoder.hpp` | `OdtSlice` 切片解码 |
| `src/measurement/measurement_session.cpp` | 会话实现（349 行） |
| `src/daq/dto_payload_decoder.cpp` | 载荷解码实现 |
| `adapter/a2l/a2l_measurement_database.hpp` | 适配器头（独立目录，D1 决策） |
| `adapter/a2l/a2l_measurement_database.cpp` | 适配器实现 |
| `tests/measurement_session_test.cpp` | L2 主链路测试（498 行，12 例） |
| `tests/measurement_adapter_test.cpp` | 适配器 L3 测试（290 行，6 例，A2L 门控） |

修改：
- `CMakeLists.txt`：`target_sources` 追加 `src/daq/dto_payload_decoder.cpp`、`src/measurement/measurement_session.cpp`；`LIBXCP_BUILD_A2L` 块内新增 `libxcp_measurement_adapter`（STATIC，链接 `libxcp::libxcp` PUBLIC + `libxcp::a2lbridge` INTERFACE）。
- `tests/CMakeLists.txt`：`libxcp_tests` 追加 `measurement_session_test.cpp`；A2L 块内新增 `MeasurementAdapterTest` 可执行目标。
- `include/libxcp/xcp_master.hpp`：新增 `friend class MeasurementSession;`（见 §4.3）。

## 3. 接口形态（最终冻结面）

```cpp
// measurement_session.hpp（核心，namespace calmcar::xcp）
class MeasurementSession : public IEventListener {
public:
    explicit MeasurementSession(IMeasurementDatabase& database);
    ~MeasurementSession() override;
    void Bind(XcpMaster& master);                       // 见 §4.1 偏差
    void Add(std::string_view name);                    // 运行中拒绝（InvalidState）
    void Remove(std::string_view name);
    void Clear();
    void Prepare();                                     // planner + ConfigureDaqListsDynamic + 路由表
    void Start(MeasurementCallback callback);           // StartDaqSync + 工作线程
    void Stop();                                        // join 线程 → StopDaq
    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] MeasurementStatistics Statistics() const;
    void SetEnvelopeMode(IdentificationFieldType type, std::size_t id_field_bytes); // 见 §4.2
    // IEventListener: OnDto → 入队（有界 256）；OnEvent/OnService 空实现
};
```

数据通路（§6.3 管道）：`OnDto` 在传输线程仅做有界入队（满则丢最旧并计 `dto_dropped`）；独立工作线程 `WorkerLoop` 用 v0.2 的 `DtoEnvelopeDecoder` 解出身份/时间戳/载荷 → `(daq_list, odt)` 查路由 → `DtoPayloadDecoder` 按 `payload_offset/size` 切片 → 每片经 `IMeasurementDatabase::ToPhysical` 转换 → 聚成 `MeasurementFrame` 在锁外回调用户。转换失败只降级该样本（`valid=false` + `decode_errors++`），不丢弃整帧。

## 4. 与计划的偏差及理由

### 4.1 构造签名：`(XcpMaster&, IMeasurementDatabase&)` → `session(db) + Bind(master)`
`XcpMaster` 的 `IEventListener*` 只能在构造时传入、无 setter（`command_executor.hpp:88`），而 `MeasurementSession` 要作为监听器收到 `OnDto`。鸡生蛋问题的解法（用户已确认，m01067："构造时传监听器 + session 先构造"）：session 先构造，`XcpMaster transport_owner(..., &session)` 时把它登记为监听器，随后 `session.Bind(master)`。使用不变式：Bind 必须在 Master 构造后立即调用；重复 Bind 抛 `InvalidArgument`。

### 4.2 新增 `SetEnvelopeMode`：识别字段模式不猜测（B-16）
计划未列此 API。实现发现工作线程需要一个 `DtoFrameLayout` 才能解包，而识别字段模式（Absolute/RelativeByte/RelativeWord/…、字节宽）只能来自会话取证（真 ECU 待 v0.9 固化），绝不允许在核心里硬编码猜测。因此提供显式设置口（默认 RelativeWord/2 字节以匹配现有 L2 脚本帧），Prepare 之后调用抛 `InvalidArgument`。

### 4.3 `friend class MeasurementSession;`（xcp_master.hpp）
Prepare 需要 `DaqTimestampBytesCached()`（时间戳宽度预检缓存，private，`xcp_master.hpp:431`）。选择最小改动 = 友元声明，而不是把该探测函数公开成公共 API（公开会诱导绕过取证直接使用）。

### 4.4 `MeasurementResult` 无 `operator->`
`Prepare` 内取计划用 `plan.Value().daq_lists / .routes`；`MeasurementResult<T>` 保持窄接口（`HasValue/Value/ErrorInfo/TakeError`），与 `a2l::Result` 语义对齐，不再扩面。

## 5. 实施中发现并修复的真实缺陷（WorkerLoop 头长双计）

首版 `WorkerLoop` 写 `layout.header_bytes = 2 + ts_bytes`，把时间戳同时计入"识别字段"和 `timestamp_size_bits`，解码器会再跳一次时间戳 → 带时间戳的帧必然 `MalformedPacket`。已修正为 `header_bytes = m_id_field_bytes_`（仅识别字段；时间戳由 `timestamp_size_bits` 单独消费），`src/measurement/measurement_session.cpp:241-250` 有注释说明。测试 `SingleVariableFullChain`（带 4B 时间戳）回归验证通过。

## 6. 已知遗留（v0.6 处理，勿丢）

1. **时间戳宽度时序**：`Prepare` 在 `ConfigureDaqListsDynamic` 之前读 `DaqTimestampBytesCached()`，但该懒加载查询只在动态配置门内（`xcp_master.cpp:481`，spec.timestamp 时）才发 `GET_DAQ_RESOLUTION_INFO` → 时间戳 slave 首次 Prepare 可能拿到 0/旧值，包络预检宽度与实际不符。v0.6：以真实宽度重规划（或让 Master 在配置成功后暴露刷新口）。
2. **错误类别有损映射**：`Prepare` 把 `MeasurementError` 统一抛成 `XcpException(InvalidArgument)`。v0.6 按 §18 分类表映射（DaqConfigurationError→RecoveryFailed/InvalidState 等）细化。
3. **统计硬化**：`timestamp_wraps` 目前恒 0（时间戳直读），v0.6 引入 `DaqTimestampConverter` 后接通回绕扩展与计数。

## 7. 验证结果

- 构建：`cmake --build build-v02 --config Release`（串行）全绿。
- 单测：`libxcp_tests.exe` → **402 例 / 77 套件：401 通过 / 1 跳过**（跳过为既有 `Ag1_Cto8`，`tests/xcp_master_integration_test.cpp:495`），含新增 12 例 MeasurementSession（生命周期、单变量全链路、多事件双列表路由、畸形帧计数、转换失败降级、50 帧有序、动态 DAQ 事务计数、Prepare 前置校验、运行期拒改、停后忽略、重复 Bind、未 Prepare 即 Start）。
- 隔离：`A2lIsolation` S1 扫描 21 个桥接文件、S2 扫描 35 个主树文件，命中均 0（adapter/ 目录不在主树扫描范围，符合 D1 决策：适配器只在 `LIBXCP_BUILD_A2L=ON` 构建）。
- 本机环境限制：无预编译 liba2l 根（`.deps-cache` 仅 googletest），`libxcp_measurement_adapter` 与 `MeasurementAdapterTest` 在本机未编译，属门控通过项；其 6 例（字段窄化、数组元素数截断 255、错误码映射两向、variant 五备选拷贝、默认转换、错误透传）待有 liba2l 的环境跑通。

## 8. 踩坑记录

- Doxygen `@code` 示例里写 `/* 消费 */` → 内层 `*/` 提前终止块注释，引发 C2059/C2143 连锁。教训：**块注释示例内绝不出现 `*/`**，回调示例改用 `// 6) 启动` 行尾注释。
- 测试 Rig 成员声明顺序：`MeasurementSession` 必须在 `unique_ptr<XcpMaster>` **之前**声明/构造（Master 构造即取 `&session`）。当前实现按先 session 后 master 的顺序构造，编译与全部 L2 通过。
- C2280（既往）：抽象基类作数据成员时须显式 `= default` 保护构造；Fake 派生类照此处理。
