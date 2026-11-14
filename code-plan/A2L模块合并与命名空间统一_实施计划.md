# A2L 模块合并与命名空间统一 —— 实施计划

> 状态：**范围已缩小并实施完毕**（实施记录见 `code-plan/A2L模块合并_实施记录.md`）
> 决策来源：用户 m03614（adapter/a2l 的 4 个文件；"有关 a2l 的操作都封装到一个模块中"；"同意都更新到 `calmcar::a2l` 中"；build-sdk.ps1 其他项目不用，外部项目通过 CMake 消费 libxcp）
>
> **范围变更（m03663）**："命名空间不再修改，只做代码合并的修改。adapter 目录用
> thirdparty/a2l-sdk/adapter/{include/a2l,src}"。因此本计划的 **M1/M2/M3/M6（命名空间统一）
> 与 §6（ABI v12→v13）全部取消**，§4/§7 中的命名空间内容仅作为当时分析结论留存；
> 实际执行的是 §7 的 **M5（搬运 + CMake + 文档）**，目录形态采用 §12 的 D2 推荐方案。

---

## 1. 目标

1. **一个模块**：所有 A2L 相关代码（DLL 契约、语义桥接、核心适配）集中到 `thirdparty/a2l-sdk/`，主树不再有 `adapter/a2l/`。
2. **一个命名空间**：所有 A2L 对外接口位于 `calmcar::a2l`。
3. 外部项目只通过 CMake 消费 libxcp，`build-sdk.ps1` 产出的准备根不再承担第三方分发职责。

## 2. 现状（实测）

| 位置 | 内容 | 规模 | 当前命名空间 |
| --- | --- | --- | --- |
| `thirdparty/a2l-sdk/include/liba2l/` + `src/` | DLL 导出契约（`IDoc` + 33 个 DTO） | 头 1015 行 / 实现 2312 行 | `liba2l`（2 处 `namespace` 声明：`liba2l_api.hpp:32`、`liba2l_export.cpp:47`） |
| `thirdparty/a2l-sdk/a2lbridge/` | 语义桥接层 | 21 文件 / 5816 行，8 个公开头 + 53 个公开类型 | `calmcar::xcp::a2l` |
| `adapter/a2l/` | 核心适配器 | 4 文件 / 601 行，5 个公开类型 | `calmcar::xcp::a2l`（嵌套写法） |
| 上游 `thirdparty/a2llib/` | 上游解析库 | 子模块 | 全局 `a2l`（**不可改**） |

`thirdparty/a2l-sdk/CMakeLists.txt:142` 已 `add_subdirectory(a2lbridge …)`，所以"a2lbridge 并入 SDK"在批次9 已完成；本次真正要做的是**把 adapter 也搬进来 + 统一命名空间**。

## 3. 目标结构

```
thirdparty/a2l-sdk/                      ← A2L 模块（唯一 A2L 代码所在地）
├── CMakeLists.txt
├── include/liba2l/                      ← ① DLL 契约（calmcar::a2l::dll）
├── src/liba2l_export.cpp
├── a2lbridge/                           ← ② 语义桥接（calmcar::a2l）
│   ├── include/libxcp/a2l/
│   └── src/
└── adapter/                             ← ③ 核心适配（calmcar::a2l），从 adapter/a2l 搬入
    ├── CMakeLists.txt
    ├── include/a2l/                     ← 保持 "a2l/xxx.hpp" 包含写法，消费方零改动
    │   ├── a2l_measurement_database.hpp
    │   └── measurement_runtime_profile.hpp
    └── src/
        ├── a2l_measurement_database.cpp
        └── measurement_runtime_profile.cpp
```

主树删除 `adapter/` 目录；根 `CMakeLists.txt:363-374` 的 `add_library(libxcp_measurement_adapter …)` 整块删除，迁到 `thirdparty/a2l-sdk/adapter/CMakeLists.txt`。

## 4. 命名空间映射

| 现在 | 之后 | 影响面 |
| --- | --- | --- |
| `liba2l` | **`calmcar::a2l::dll`** | 2 处声明 + 146 处裸 `a2l::` + 178 处 `liba2l::` + 1 个测试 |
| `calmcar::xcp::a2l`（bridge） | `calmcar::a2l` | 21 个自身文件 + 33 个消费文件 |
| `calmcar::xcp::a2l`（adapter） | `calmcar::a2l` | 4 个自身文件 + 6 个消费文件 |
| 上游全局 `a2l` | 不动 | git 子模块（MIT 上游） |

### 4.1 为什么 `liba2l` 必须下沉一层（硬约束，实测证据）

**类型重名**：

```
liba2l 顶层类型 33 个：SymbolDto / StructInfoDto / ErrorCode / Phase(无) / …
bridge 顶层类型 53 个：SymbolInfo / ErrorCode / Phase / LoadWarning / …
两者交集：ErrorCode
```

`liba2l::ErrorCode`（DLL 返回码）与 `calmcar::xcp::a2l::ErrorCode`（领域错误码）**同名不同义**。若两者都平铺到 `calmcar::a2l`，编译期直接重定义冲突。

**名字查找劫持**：`thirdparty/a2l-sdk/src/liba2l_export.cpp` 内有 **146 处**裸 `a2l::`（首个在 `:56` 的 `a2l::A2lDataType`），指的是上游全局 `a2l`。该文件当前位于 `namespace liba2l`：

- 若改成 `namespace calmcar::a2l`：从 `calmcar::a2l` 内部查找 `a2l::` → 在 `calmcar` 作用域命中自己 → 全部解析成 `calmcar::a2l::A2lDataType`（不存在）→ **146 处全错**。
- 若改成 `namespace calmcar::a2l::dll`：查找 `a2l::` 时 `dll`、`calmcar::a2l`、`calmcar` 均无成员 `a2l` → 正确落到全局 `a2l`，**无需改动这 146 处**（仍建议显式写 `::a2l::` 自证意图）。

结论：`calmcar::a2l::dll` 既满足"所有 A2L 接口都在 `calmcar::a2l` 命名空间下"，又避开重名与查找劫持，且它只被 bridge 的 `.cpp` 使用（bridge 公开头里 `liba2l::` 仅 1 处注释，`a2l_result.hpp:30`），对最终用户完全不可见。

### 4.2 adapter 与 bridge 同命名空间的重名检查

adapter 的 5 个公开类型：`A2lMeasurementDatabase`、`EventBoundMeasurementDatabase`、`A2lEventMetadata`、`MeasurementRuntimeProfile`、`MeasurementEventBindingCandidates`。
与 bridge 53 个类型**无交集**，可安全合并到 `calmcar::a2l`。

## 5. 依赖方向与构建守卫（本次唯一需要让步的地方）

`adapter/a2l` 真实依赖 libxcp 核心：`IMeasurementDatabase`、`MeasurementResult`、`MeasurementSession`、`XcpMaster`（`measurement_runtime_profile.cpp:10-11` 直接包含 `libxcp/xcp_master.hpp`）。这不是可以取消的依赖，所以搬进 SDK 后：

- adapter 目标 **只在 `libxcp` 目标存在时构建**（根 `CMakeLists.txt:177` 先建 `libxcp`，`:361` 才 `add_subdirectory(thirdparty/a2l-sdk)`，顺序成立）：

```cmake
# thirdparty/a2l-sdk/CMakeLists.txt 末尾
# 适配器依赖主树核心的窄接口，只随主树构建；独立 SDK 构建（build-sdk.ps1）跳过。
if(TARGET libxcp)
    add_subdirectory(adapter ${CMAKE_CURRENT_BINARY_DIR}/adapter-build)
endif()
```

- 独立 SDK 构建不再产出 adapter（与现状一致：`build-sdk.ps1` 只拷 `liba2l.dll`、`liba2l.lib`、`libxcp_a2lbridge.lib`、头文件）。
- 消费方（`tools/xcp_master`、4 个 tests、2 个 examples）只链接目标名 `libxcp_measurement_adapter`，**目标名不变、包含路径写法不变**，这 6 个文件零改动。

## 6. ABI 影响

`liba2l` 改名 → 全部导出符号 mangled name 变化 → 硬 ABI 断裂。按既有惯例（`liba2l_api.hpp` 顶部 v2…v12 版本史）升版：

- `kLibA2lAbiVersion`：**12 → 13**
- 版本史追加 v13 条目：`命名空间 liba2l → calmcar::a2l::dll（ABI 断裂，必须同步重建）`
- `CreateDoc()` 的运行时校验（`liba2l_export.cpp`）自动生效，旧 DLL/新头不匹配会在加载期被拒绝而不是崩溃。

因为 `build-sdk.ps1` 不再对第三方分发，且外部项目统一经 CMake 消费，ABI 断裂的影响面收敛在仓库内 + 使用者的重新构建。

## 7. 分阶段实施步骤（每阶段可独立编译、可回归）

| 步骤 | 内容 | 验证 |
| --- | --- | --- |
| **M1** | `liba2l` → `calmcar::a2l::dll`：改 2 处命名空间声明、`kLibA2lAbiVersion` 升 13、`liba2l_export.cpp` 的 146 处裸 `a2l::` 显式写成 `::a2l::`、bridge 内 178 处 `liba2l::` 改为 `dll::`、`tests/a2l_smoke_test.cpp` 改全限定名 | 构建通过；`A2lSmoke` 全绿 |
| **M2** | bridge 命名空间 `calmcar::xcp::a2l` → `calmcar::a2l`：8 个公开头 + 13 个实现文件 | 构建通过 |
| **M3** | 消费方同步：33 个文件（tests/tools/examples/adapter）+ 6 处 `namespace a2l = …` 别名改名为 `a2lmod` | 构建通过 |
| **M4** | 门禁同步：`tests/a2l_isolation_check.cmake:48` 与 `a2lbridge/CMakeLists.txt` 的正则改为 `calmcar::a2l`；`A2lIsolation` 用例跑通 | `ctest -R A2lIsolation` |
| **M5** | 搬迁 adapter：`git mv adapter/a2l/{*.cpp,*.hpp} → thirdparty/a2l-sdk/adapter/{src,include/a2l}/`，新增 `adapter/CMakeLists.txt`，删除根 `CMakeLists.txt:363-374` 块，SDK 加 `if(TARGET libxcp)` 守卫 | MSVC Release 全量构建 |
| **M6** | adapter 命名空间 `calmcar::xcp::a2l` → `calmcar::a2l`（含 4 个自身文件 + 6 个消费文件）；`IMeasurementDatabase` 等核心类型改全限定 `calmcar::xcp::…` | 构建通过 |
| **M7** | 文档与记录：`docs/API_说明.md:3`、`docs/XCP_调用_QuickStart.md:29,96`、`docs/modules/A2L集成.md:23`、`thirdparty/a2l-sdk/CMakeLists.txt` 头注释、`AGENTS.md` 架构图若涉及；写 `code-plan/A2L模块合并与命名空间统一_实施记录.md` | 人工核对 |

## 8. 受影响文件清单（预估）

| 类别 | 文件数 | 改动性质 |
| --- | --- | --- |
| a2l-sdk 导出层 | 2 | 命名空间、ABI 号、146 处 `::a2l::` |
| a2lbridge 自身 | 21 | 命名空间声明、`liba2l::` → `dll::` |
| bridge 消费方 | 33 | 全限定名替换 |
| adapter 自身 | 4（+1 新 CMakeLists） | 搬迁 + 命名空间 |
| adapter 消费方 | 6 | 目标名不变，仅命名空间若被引用 |
| 顶层 CMake | 2 | 删除 adapter 目标块；SDK 增加与守卫 |
| 门禁脚本 | 2 | 正则更新 |
| 文档 | 4 | 命名空间与目录说明 |
| 测试 | 1（新增用例可选） | `a2l_smoke_test.cpp` 命名空间 |

## 9. 测试计划

1. **构建**：MSVC Release 全量（`cmake --build cmake-build-release -j 4`），零新增警告。
2. **定向**：`ctest -R "A2l|XcpMaster"`（当前基线 200 项全绿）。
3. **全量**：`ctest`（基线 1247 项，需先设 `TMPDIR/TEMP/TMP` 到可写目录，否则 29 项环境性误报）。
4. **门禁**：`ctest -R A2lIsolation` 必须仍然命中 0（改完正则后要有一次"故意注入"验证：临时在 `src/` 加 `calmcar::a2l` 字样确认能报错，验证后撤销）。
5. **ABI**：`A2lSmoke` 中 `CreateDoc(kLibA2lAbiVersion-1)` 必须被拒绝、`+1` 也被拒绝，等于 13 时成功（既有用例已覆盖，仅数字变化）。
6. **人工**：`xcp_master --a2l <真实A2L> --transport tcp --host …` 跑通加载 + 变量预览。

## 10. 风险与缓解

| 风险 | 缓解 |
| --- | --- |
| 命名空间改名漏改导致"能编过但语义错"（如某处 `a2l::` 悄悄指向上游） | 改完全仓 `grep -rn "calmcar::xcp::a2l"` 必须为 0；`grep -rn "namespace a2l ="` 必须为 0 |
| 门禁假绿（正则没同步） | M4 单独提交 + 注入验证 |
| adapter 搬迁打断 6 个消费方 | 保持目标名与 `"a2l/xxx.hpp"` 包含写法不变，消费方零改动 |
| 独立 SDK 构建被 adapter 拖坏 | `if(TARGET libxcp)` 守卫；用 `build-sdk.ps1` 做一次独立构建验证 |
| 与正在运行的 `xcp_master.exe` 抢文件导致 LNK1104 | 构建前检查进程占用 |
| 行尾被补丁脚本改成 CRLF 造成整文件 diff | 判行尾用 `raw.count('\r\n') == raw.count('\n')`，改完用 `git diff --stat` 复核行数 |

## 11. 验收标准

1. 主树不再存在 `adapter/` 目录，A2L 相关代码全部位于 `thirdparty/a2l-sdk/`。
2. 所有 A2L 公共接口的命名空间为 `calmcar::a2l`（DLL 契约位于其子空间 `calmcar::a2l::dll`）。
3. `ctest` 全量通过（1247 项基线）；`A2lIsolation` 注入验证可复现拦截。
4. 6 个 adapter 消费方源码零改动即通过构建。
5. `build-sdk.ps1` 独立构建仍可用（不含 adapter）。
6. 文档与 AGENTS.md 架构描述同步。

## 12. 待确认的两个决策点

- **D1（命名空间细节）**：`liba2l` 采用子空间 `calmcar::a2l::dll`（推荐，理由见 §4.1 的重名与查找劫持），还是平铺到 `calmcar::a2l` 并把 `liba2l::ErrorCode` 改名为 `SdkErrorCode`？
- **D2（adapter 目录形态）**：`thirdparty/a2l-sdk/adapter/{include/a2l,src}`（推荐，消费方 `#include "a2l/xxx.hpp"` 零改动），还是扁平化成 `thirdparty/a2l-sdk/adapter/` 并统一改成 `#include <libxcp/a2l/adapter/…>`（多改 6 个文件，但路径语义更统一）？
