# A2L 集成 R4 实施记录 —— 批次 9：SDK 工程合并（a2l-sdk + uchardet-shim + a2lbridge 一体）

日期：2026-09-26　　状态：已完成并全量验证

## 0. 任务来源与裁决

- 用户指令（m00457）：**"我现在希望将 a2l-sdk，uchardet-shim 和 a2lbridge 分装成一个工程，工程都合并到 a2l-sdk 工程下。"**
- 裁决 1（构建入口，用户选定 **X1**）：bridge 源码移入 SDK 工程，`build-sdk.ps1` 一次构建出
  `liba2l.dll + liba2l.lib + libxcp_a2lbridge.lib + 两套头树`；主树只创建 **2 个 IMPORTED target**
  消费，不再 `add_subdirectory(a2lbridge)` —— 符合设计 §5.1"主树不编 thirdparty"边界。
- 裁决 2（uchardet 整合深度，用户选定**工程内目标**）：假包机制（`uchardetConfig.cmake` 文件对 +
  `build-shim.ps1` + `CMAKE_PREFIX_PATH` 注入）**退役**，shim 改为 SDK 子工程。

前提事实（批8 已实测）：bridge 源码不引用主树 `libxcp/xcp/*` 任何头（grep 0 命中），
只依赖 `liba2l::liba2l` → 完全脱离主树构建可行，无循环依赖。

## 1. 终态目录

```text
thirdparty/a2l-sdk/                 # A2L 栈唯一构建工程（project(liba2l_sdk C++23)）
├── CMakeLists.txt                  # add_subdirectory 顺序：uchardet-shim → ../a2llib → liba2l → a2lbridge
├── build-sdk.ps1                   # 一键构建 + prepared root 填充
├── include/liba2l/                 # SDK 契约头（不变）
├── src/                            # liba2l_export.cpp / dll_main.cpp（不变）
├── uchardet-shim/                  # ← 自主树 thirdparty/uchardet-shim/ 迁入（删 build-shim.ps1、uchardetConfig.cmake.in）
└── a2lbridge/                      # ← 自主树顶层 a2lbridge/ 迁入（C++20 桥接层，9 TU，P9 扫描随迁）
```

prepared root `build/liba2l-prepared/msvc-x64-release/` 新增：
`lib/libxcp_a2lbridge.lib`（968,324 B）与 `include/libxcp/a2l/*.hpp`（8 个公开头）。

## 2. 文件变更清单

| 文件 | 变更 |
|---|---|
| `a2lbridge/`（主树顶层） | **移动** → `thirdparty/a2l-sdk/a2lbridge/`（源码零改动，仅 CMakeLists 头注释/链接注释更新为"父工程=SDK"） |
| `thirdparty/uchardet-shim/` | **移动** → `thirdparty/a2l-sdk/uchardet-shim/`；**删除** `build-shim.ps1`、`uchardetConfig.cmake.in` |
| `thirdparty/a2l-sdk/CMakeLists.txt` | 新增 shim 子工程挂载 + `set(uchardet_FOUND TRUE)`；新增 `add_library(liba2l::liba2l ALIAS liba2l)`；新增 `add_subdirectory(a2lbridge)`；头注释更新 |
| `thirdparty/a2l-sdk/uchardet-shim/CMakeLists.txt` | 文末新增 `uchardet::libuchardet`（INTERFACE IMPORTED GLOBAL）承接上游链接名；注释重写 |
| `thirdparty/a2l-sdk/build-sdk.ps1` | 删 shim 委托段与 `-DCMAKE_PREFIX_PATH` 注入；`Find-Artifact` 增加递归兜底；产物检查/拷贝增 `libxcp_a2lbridge.lib` + `a2lbridge/include/libxcp → include/`；注释更新 |
| `CMakeLists.txt`（主树根） | ON 分支删 `add_subdirectory(a2lbridge)`，增 `find_path(libxcp/a2l/a2l_bridge.hpp)` + `find_library(libxcp_a2lbridge)` + `add_library(libxcp::a2lbridge STATIC IMPORTED GLOBAL)`；注释更新 |
| `code-plan/A2L_集成_a2llib选型与适配层详细设计_R4.md` | §3.2/§5.1/§5.2/§5.3.7 四处**批次9 勘误指针**；§5.2 勘误同时更正 P1 事实（a2lobject 6 符号 → 实际不生效，仅 a2lhelper 5 符号） |
| `tests/CMakeLists.txt`、`tests/a2l_smoke_test.cpp`、`tests/a2l_gen/gen_a2l.py` | **零改动**（ALIAS 名 `libxcp::a2lbridge` 保持，IMPORTED 形态透明） |

## 3. 关键决策与实现要点

### 3.1 uchardet 工程内目标方案（两个 CMake 硬错误教训，均实录）

上游 `script/uchardet.cmake:6` 以 `if (NOT uchardet_FOUND)` 守卫 `find_package(uchardet CONFIG REQUIRED)`，
SDK 根在 `add_subdirectory(../a2llib)` 前预置 `set(uchardet_FOUND TRUE)`（普通变量，子作用域可见）即跳过；
上游 `:18` 的 `cmake_print_properties(... INTERFACE_LOCATION)` 与 `a2l/CMakeLists.txt:369` 的
PUBLIC 链接要求存在名为 `uchardet::libuchardet` 的目标。目标形态迭代：

1. ❌ `add_library(uchardet::libuchardet ALIAS uchardet_shim)` →
   `CMake Error: The LOCATION property may not be read from target "uchardet_shim"`
   （**ALIAS 上查询 LOCATION 类属性硬报错**，configure 直接失败）。
2. ❌ `add_library(uchardet::libuchardet INTERFACE)` →
   `The target name "uchardet::libuchardet" is reserved`（CMake 4 禁止创建带命名空间的非 ALIAS/非 IMPORTED 目标）。
3. ✅ 终态：`add_library(uchardet::libuchardet INTERFACE IMPORTED GLOBAL)` +
   `INTERFACE_INCLUDE_DIRECTORIES=<shim>/include` + `INTERFACE_LINK_LIBRARIES=uchardet_shim`（工程内静态桩）。
   属性查询返回空值不报错，桩库经 a2l 的 PUBLIC 传播最终吞入 `liba2l.dll`（A-12 语义不变）。

### 3.2 SDK 工程内挂载与标准分治

`add_subdirectory` 顺序 shim → a2llib → liba2l（SHARED，根作用域）→ `liba2l::liba2l` ALIAS → a2lbridge。
工程全局 `CMAKE_CXX_STANDARD 23`；bridge 子目录自带 `CXX_STANDARD 20` + `cxx_std_20` 覆写，互不冲突。
bridge 的 P9 隔离扫描（GLOB + file(READ) + FATAL_ERROR）路径全部相对 `${CMAKE_CURRENT_SOURCE_DIR}`，随目录迁移零改动生效。

### 3.3 主树 IMPORTED 消费

```cmake
add_library(libxcp::a2lbridge STATIC IMPORTED GLOBAL)
set_target_properties(libxcp::a2lbridge PROPERTIES
    IMPORTED_LOCATION "${LIBXCP_LIBA2L_ROOT}/lib/libxcp_a2lbridge.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBXCP_LIBA2L_ROOT}/include"
    INTERFACE_LINK_LIBRARIES liba2l::liba2l)
```

bridge 公开头内 `#include <liba2l/liba2l_api.hpp>` 由 `INTERFACE_LINK_LIBRARIES` 传播的 liba2l
导入目标 include 目录满足；`$<TARGET_FILE:liba2l::liba2l>`（tests POST_BUILD 拷 DLL）对 IMPORTED
SHARED 同样有效。目标名与旧 ALIAS 完全一致 → tests 零改动。

### 3.4 build-sdk.ps1

- 删"委托 build-shim.ps1 + 假包目录校验 + `-DCMAKE_PREFIX_PATH`"整段。
- `Find-Artifact` 增加递归兜底：VS 生成器下子工程产物落 `<子目录构建树>/<Config>/`
  （实测 `build/liba2l-sdk-Release/a2lbridge-build/Release/libxcp_a2lbridge.lib`），
  原两处平铺查找命中不到。
- prepared root 拷贝清单扩为 4 类产物（DLL / 2 个 lib / 2 个头树；头树用
  `Copy-Item -Recurse` 到 `include/`，保持 `libxcp/a2l/` 相对结构）。

### 3.5 工作流代价（X1 的已知成本）

改 bridge 源码后需重跑 `build-sdk.ps1`（SDK 构建树存在时为增量：只重编 bridge 的 9 TU 并刷新
prepared root），再在主树重新构建链接。旧 `build/uchardet-shim-Release/` 构建树已删除。

## 4. 测试结果

| 项 | 命令/判据 | 结果 |
|---|---|---|
| SDK configure（判据1） | `cmake -S thirdparty/a2l-sdk -B build/liba2l-sdk-Release`（已 `-U uchardet_DIR -U CMAKE_PREFIX_PATH` 清缓存，确保不靠旧假包） | ✅ Configuring done；打印 `uchardet_FOUND="TRUE"`、`INTERFACE_INCLUDE_DIRECTORIES=.../uchardet-shim/include` |
| build-sdk.ps1 一次完成（判据2） | `build-sdk.ps1 -BoostRoot C:\boost -Config Release` | ✅ EXIT=0；submodule SHA 命中 c3105749 |
| prepared root 产物 | 递归清单 | ✅ `bin/liba2l.dll`(710,656B)、`lib/liba2l.lib`、`lib/libxcp_a2lbridge.lib`、`include/liba2l/{api,export}`、`include/libxcp/a2l/*.hpp`×8 |
| 主树 ON 构建+全量测试（判据3） | 重配置 `LIBXCP_BUILD_A2L=ON` → 串行 Release 构建 → `ctest -C Release` | ✅ 0 error；**290/290 Passed**（#289 a2l_gen、#290 A2lSmoke） |
| G7 OFF 门禁 | `-DLIBXCP_BUILD_A2L=OFF` 重配置构建 | ✅ 仅 gtest/gtest_main/libxcp/libxcp_tests 四目标；**288/288 Passed**；A2L 测试对整体消失 |
| ON 恢复幂等 | 重配置 ON + 构建 + `ctest -R "a2l_gen\|A2lSmoke"` | ✅ 2/2 Passed |
| P9 隔离 | grep `#include\s*[<"]a2l/` on `thirdparty/a2l-sdk/a2lbridge/` | ✅ 源码 0 命中（唯一命中为 `a2lbridge/CMakeLists.txt:5` 禁令注释本身；configure 扫描只 glob src/+include/，且每次 SDK configure 强制）；`libxcp/xcp\|calmcar` 亦 0 |
| 残留引用 | grep `build-shim\|uchardetConfig\|thirdparty/uchardet-shim\|add_subdirectory(a2lbridge)` | ✅ 仅注释历史说明与批次8历史文档命中；生效构建文件零残留（XCPlite 参考树无关） |

## 5. 已知限制与提醒

- prepared root 内 `include/libxcp/a2l/*.hpp` 是**拷贝快照**：bridge 头改动后未重跑
  `build-sdk.ps1` 时，主树编译用的是旧头（症状=编译期签名不匹配）。纪律：改 bridge → 先跑脚本 → 再动主树。
- 桥接 .lib 仅 Release/vc143（与 DLL 同一 A-10 手工核对范围）；Debug/CRT 矩阵按设计推迟。
- `libxcp_a2lbridge.lib` 的 `LIBXCP_BUILD_A2L=OFF` 门禁不校验"prepared root 是否过期"——
  若 SDK 源码更新而未重建，主树仍会链接旧 lib（最小集接受此风险，批次10+ 可考虑指纹校验）。
- 主树 `cmake-build-release` 中残留的旧 `a2lbridge` 子构建目录文件不再参与生成器产物，可随时手动清理。
