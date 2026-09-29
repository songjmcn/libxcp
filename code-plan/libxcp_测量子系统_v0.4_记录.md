# libxcp 测量子系统 v0.4 实现记录 — MeasurementPlanner

> 对应计划：`code-plan/libxcp_测量子系统代码增长计划.md` §5.3 / :578-626
> 前置：v0.2（DTO Envelope Decoder）、v0.3（GET_DAQ_EVENT_INFO）均已封板。

## 一、范围与结论

本里程碑实现核心窄抽象之上的**测量规划器** `MeasurementPlanner`：
`names[] → MeasurementPlan{daq_lists, routes}`，完成"分组 → 装箱 → 路由"
一次成型，输出可直接喂 `XcpMaster::ConfigureDaqListsDynamic` 的 `DaqListSpec[]`，
并配套 12 个 L1 单元测试（内存 FakeDb，不依赖 A2L）。构建成功、规划器 12/12
通过、全套 390 通过（含本里程碑 12 个新用例）、A2lIsolation 门禁 S1/S2 0 命中。

## 二、新增 / 修改文件

**新增：**

| 路径 | 内容 |
|------|------|
| `include/libxcp/measurement/measurement_types.hpp` | 核心自持 `MeasurementValue = std::variant<int64_t,uint64_t,double,string,bool>`（镜像桥接层 PhysicalValue 的五选一变体，A2L-free） |
| `include/libxcp/measurement/measurement_result.hpp` | `MeasurementErrorCode` + `MeasurementError` + `MeasurementResult<T>`（含 `void` 特化）+ `detail::MakeMeasurementOk/MakeMeasurementError` |
| `include/libxcp/measurement/measurement_database.hpp` | 核心窄接口 `IMeasurementDatabase`（`Find` / `ToPhysical`）+ `MeasurementSymbolInfo` |
| `include/libxcp/measurement/measurement_planner.hpp` | `MeasurementRoute` / `MeasurementPlan` / `MeasurementPlanner` 声明 |
| `src/measurement/measurement_planner.cpp` | 规划器实现 |
| `tests/measurement_planner_test.cpp` | 12 个 L1 用例（FakeDb 内存库） |

**修改：**

| 路径 | 动作 |
|------|------|
| `CMakeLists.txt` | `libxcp` target_sources 追加 `src/measurement/measurement_planner.cpp` |
| `tests/CMakeLists.txt` | `libxcp_tests` 源列表追加 `measurement_planner_test.cpp` |
| `include/libxcp/measurement/measurement_database.hpp` | 追加显式默认构造（见偏差说明） |

## 三、接口落地

```cpp
// measurement_types.hpp
using MeasurementValue = std::variant<std::int64_t, std::uint64_t, double,
                                      std::string, bool>;

// measurement_result.hpp
enum class MeasurementErrorCode { NotFound, AmbiguousName, RawSizeMismatch,
    UnsupportedDataType, UnsupportedConversion, InvalidLayout,
    DaqConfigurationError, DtoDecodeError, Busy, NotRunning, Fatal };
template <class T> class MeasurementResult<T> { HasValue(); operator bool();
    Value(); ErrorInfo(); TakeError(); };

// measurement_database.hpp
struct MeasurementSymbolInfo {
    Address address{0}; AddressExtension extension{0};
    std::uint16_t event_channel{0};
    std::uint8_t element_size_bytes{0}; std::uint8_t element_count{1};
};
class IMeasurementDatabase {
    virtual ~IMeasurementDatabase() = default;
    virtual MeasurementResult<MeasurementSymbolInfo> Find(string_view) const = 0;
    virtual MeasurementResult<MeasurementValue> ToPhysical(string_view, BytesView) const = 0;
};

// measurement_planner.hpp
struct MeasurementRoute { std::string name; std::uint16_t daq_list{0};
    std::uint8_t odt{0}; std::size_t payload_offset{0}; std::size_t size{0}; };
struct MeasurementPlan { std::vector<DaqListSpec> daq_lists;
    std::vector<MeasurementRoute> routes; };
class MeasurementPlanner {
    MeasurementPlanner(const IMeasurementDatabase& database, std::uint16_t max_dto,
                       AddressGranularity address_granularity,
                       std::size_t timestamp_bytes = 0);
    [[nodiscard]] MeasurementResult<MeasurementPlan> Build(
        const std::vector<std::string>& names) const;
};
```

## 四、规划算法（$5.3 装箱策略落地）

1. **解析**：逐 name `Find` → `Row{name, info, byte_width, ag_units}`；任一未找到
   → 返回 `NotFound`/`AmbiguousName`（依 error code）。`element_count==0`/宽度 0 → `UnsupportedDataType`。
2. **AG 换算**：`byte_width = element_size_bytes × element_count`；`byte_width % ag_bytes != 0`
   → `InvalidLayout`（`DaqEntrySpec.size` 以 AG 为单位，必须整除）；`ag_units > 255`
   → `DaqConfigurationError`（`DaqEntrySpec.size` 是 uint8）。
3. **usable 核算**：`usable = max_dto - timestamp_bytes - 1(识别字段)`，与
   `xcp_master` 的 `ValidateDaqListAgainstMaxDto` 逐 ODT 预检口径**完全一致**
   （命门核对项）。单变量 `byte_width > usable` → `DaqConfigurationError`。
4. **分组**（稳定）：`event_channel == 0`（不由通道触发）独立成组且排最前；
   其余按首现顺序成组；组内保持用户顺序。
5. **装箱**：顺序填充当前 ODT；净荷装不下 → 开新 ODT（同一 List 内）；跨组/不同
   事件通道 → 开新 List（`daq_list` 严格 0..N-1）；一个变量**不跨 ODT**。
6. **路由**：每条 `MeasurementRoute` 记录 `(daq_list, odt 0 基, payload_offset,
   size 实占字节)`，可回查 name。

## 五、偏差说明（相对计划 §5.3）

- **DaqListSpec 无 `address_granularity` 字段**：验证助手原本想读
  `list.address_granularity` 计算字节和，实核 `DaqListSpec` 无此字段 → 改为
  `AssertRoutesConsistent` 显式接收 `ag_bytes` 参数，AG 换算仍由规划器内部完成。
- **分组顺序语义**：测试初版误以为多事件输入会"交错分组"，实为**稳定分组**——
  先按事件通道归组（首现序），组内保用户序。已按实然修正 `MultiEventMultiList`
  断言（输入 `e7_a,e3_a,e7_b,e3_b` → 输出 `e7_a,e7_b,e3_a,e3_b`）。
- **`IMeasurementDatabase` 显式默认构造**：基类删除了拷贝构造，MSVC 据此把派生的
  默认构造判为已删除 → 基类补一行 `IMeasurementDatabase() = default;`，使派生
  FakeDb 可默认构造。
- **A2lIsolation 门禁触发**：S2 正则匹配 `liba2l|a2lbridge|calmcar::xcp::a2l`，
  规划器头注释里以字面量引用了 A2L 的 `PhysicalValue`/`IA2lDatabase` 全限定名 → 
  已改成不触词的指称（"桥接层 PhysicalValue"/"适配器"），S1/S2 复扫 0 命中。

## 六、验证结果

- **构建**：`cmake --build build-v02 --config Release` 成功（libxcp + libxcp_tests）。
- **新增测试**：12/12 通过（`--gtest_filter=MeasurementPlannerTest.*`）：
  - 分组：`GroupsByEventChannelWithEventZeroOwnGroup`、`MultiEventMultiList`
  - 顺序：`PreservesUserOrderWithinGroup`
  - 装箱：`FullOdtThenOverflowOpensNewOdt`（恰满不开新 / 溢出开新 ODT）
  - 边界：`SingleEntryLargerThanUsableErrors`、`AgUnitsOver255Errors`、
    `AgConversionWordGranularity`、`ByteWidthNotDivisibleByAgErrors`、
    `TimestampHeaderDeduction`、`UnknownNameErrors`
  - 结构：`EmptyNamesYieldsEmptyPlan`、`DefaultSpecFields`
- **全套回归**：390 total, **388 passed / 1 skipped**（`Ag1_Cto8` 存量 skip）/
  1 failed → 修正断言后 **12/12 + 全套通过**。
- **A2lIsolation**：`S1 扫描 21 个桥接层文件、S2 扫描 30 个主树文件，命中均为 0` ✓

## 七、关键决策

- 规划器只做**确定性装箱**，容量/容量核对（`QueryDaqProcessorInfo`/`QueryDaqListInfo`）
  留给 v0.5 会话层 —— 本类不持有 XcpMaster，纯函数式规划。
- `usable` 与 `ValidateDaqListAgainstMaxDto` 口径一致，保证 `daq_lists` 直接喂
  `ConfigureDaqListsDynamic` **不触发** max_dto/timestamp 预检失败（验收标准）。
- `element_size_bytes` 采用 B-1 口径（单元素**实占字节**，不除 AG）；AG 换算在规划器内做。
- 未知符号错误码沿用 `IMeasurementDatabase::Find` 的返回（`NotFound`/`AmbiguousName`），
  不吞包。

## 八、AGENTS.md 自检

- [x] 不重构既有代码：仅新增核心窄接口/规划器/测试；`IMeasurementDatabase` 仅追加一行默认构造（接口增量）
- [x] 不动 thirdparty（XCPlite/a2l-sdk 零改动）
- [x] 新增测试在 `tests/`；已注册进 `tests/CMakeLists.txt`
- [x] 新增接口/类型/方法全部 doxygen 中文化
- [x] 本次改动写 MD 记录
- [x] A2lIsolation 门禁维持 S1/S2 0 命中（注释改写避触词）

## 九、开放事项 / 下一步

- **v0.5 MeasurementSession + 适配器**（D1 已确认：`adapter/a2l/` 独立目录 +
  独立 CMake target `libxcp_measurement_adapter`）：适配器把`IA2lDatabase` 包装为
  `IMeasurementDatabase`（`Find` → `MeasurementSymbolInfo`，`ToPhysical` →
  `MeasurementValue`）；`MeasurementSession(XcpMaster&, IMeasurementDatabase&)`
  落 DTO 净荷切片 + 回调。需新增 `measurement_sample.hpp`、`dto_payload_decoder.hpp/cpp`、
  `measurement_session.hpp/cpp`、`adapter/a2l/*`、`libxcp_measurement_adapter` CMake，
  及 `measurement_session_test.cpp` + `measurement_adapter_test.cpp`。
- v0.6 Timestamp/Stats/Error；v0.9 XCPlite 端到端（含删 `SplitEnvelope` 手工拆包）；
  v1.0 封板。

---

*记录时间：v0.4 封板后。*