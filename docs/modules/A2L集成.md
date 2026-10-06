# A2L 集成（adapter + a2lbridge）

> 归属：`adapter/a2l/`、`thirdparty/a2l-sdk/`、`LIBXCP_BUILD_A2L` 开关
> 本文档描述 A2L 描述文件的加载、桥接与核心适配，全部为**可选组件**。

## 1. 模块定位

核心库与测量路径不依赖 A2L；A2L 接入由三个层次组成，依赖方向单向：

```text
libxcp 核心 (IMeasurementDatabase 窄接口)
    ↑  adapter/a2l/A2lMeasurementDatabase   ← 核心↔桥接层唯一接触点
    ↑  libxcp::a2lbridge（thirdparty/a2l-sdk/a2lbridge，语义适配层）
    ↑  liba2l.dll（thirdparty/a2llib 上游解析库的 DLL 封装）
```

边界纪律（`tests/a2l_isolation_check.cmake` ctest 门禁 `A2lIsolation`）：

- **S1**：桥接层目录内 `#include <a2l/` 命中数必须 == 0（上游头只允许出现在
  SDK 导出层）
- **S2**：主树 `include/` 与 `src/` 内 `liba2l` / `a2lbridge` /
  `calmcar::xcp::a2l` 命中数必须 == 0（主库不得知道 A2L 栈存在）

## 2. 组成

| 组件 | 位置 | 职责 |
|---|---|---|
| 适配器 `A2lMeasurementDatabase` | `adapter/a2l/a2l_measurement_database.*`，target `libxcp_measurement_adapter` | 把桥接层 `IA2lDatabase` 包装为核心 `IMeasurementDatabase`；只做 `Find`（符号→`MeasurementSymbolInfo`）与 `ToPhysical`（换算委托桥接库）两件事，**不实现任何换算/AG 逻辑** |
| 桥接层 `libxcp_a2lbridge` | `thirdparty/a2l-sdk/a2lbridge/`，target `libxcp::a2lbridge` | 语义适配：`A2lBridge` 门面（Load/LoadAsync、不可变快照）、`IA2lDatabase` 查询（Find/Search/Count/ByteSizeOf）、CompuMethod 求值、DAQ Layout、IF_DATA XCP、RecordLayout、结构体布局 |
| A2L 栈构建工程 | `thirdparty/a2l-sdk/`（`build-sdk.ps1`） | 一键产出准备根：`liba2l.dll` + `libxcp_a2lbridge.lib` + 头树；主树只建 IMPORTED target 消费，不编译任何 thirdparty 源码 |

## 3. 关键接口

### `A2lMeasurementDatabase`（适配器）

```cpp
const a2l::A2lBridge* bridge;               // Load 成功后
A2lMeasurementDatabase adapter(*bridge->Database());
MeasurementSession session(adapter);
```

持有对 `IA2lDatabase` 的 const 引用（非拥有），Bridge/数据库生命周期须长于
适配器；不可拷贝。

### `A2lBridge`（桥接层门面，`libxcp/a2l/a2l_bridge.hpp`）

- `Load`/`LoadAsync` 管理 `liba2l.dll` 生命周期并发布不可变快照；
  `Database()`/`XcpInfo()` 在快照发布后才可用（加载中 `NotReady`）
- `LoadOptions`：`module_information_only`、`require_if_data_xcp`、
  `active_module`（多 MODULE 时消歧）、`allow_include_outside_root`
  （默认禁止 /include 越出主 A2L 目录，循环 include 与深度 >32 一律拒绝）
- `CompareWithRuntime`：A2L 声明值与 Slave 运行时值差异比对（B-16）

### `IA2lDatabase`（只读查询视图）

`Find`（`module::name` 精确 / 裸名唯一）、`Search`（大小写无关通配符 `*`/`?`）、
`Count`、`ByteSizeOf`（B-1 口径不除 AG）；不可变快照，可并发只读访问。

## 4. 构建与测试

```bash
# 1) 先产出准备根（需 Boost）
pwsh -File thirdparty/a2l-sdk/build-sdk.ps1 -BoostRoot <boost前缀> \
     -Config Release -OutRoot build/liba2l-prepared
# 2) 主树开启
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release \
      -DLIBXCP_BUILD_A2L=ON \
      -DLIBXCP_LIBA2L_ROOT=build/liba2l-prepared/msvc-x64-Release
```

测试（见 `tests/CMakeLists.txt`）：`a2l_gen`（Python 黄金样本 fixture）→
`A2lSmoke`、`A2lGolden`（黄金回归 T1~T8）、`A2lE2E`（端到端）、
`MeasurementAdapterTest`、`ParseLargeFileUnderBudget`（性能基线）、
`A2lIsolation`（隔离门禁）。

设计文档：`code-plan/A2L_集成_a2llib选型与适配层详细设计_R4.md`、
`code-plan/A2L_CMake最小开发验证集_R4.md`。
