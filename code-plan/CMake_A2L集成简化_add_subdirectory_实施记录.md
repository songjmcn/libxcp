# CMake A2L 集成简化：add_subdirectory 直接编译 —— 实施记录

## 1. 背景与目标

原机制开启 A2L 支持需要三步：

1. 手动运行 `thirdparty/a2l-sdk/build-sdk.ps1`（需 `-BoostRoot`、`-Config`、`-OutRoot`）产出"准备根"；
2. 主工程 configure 时同时指定 `-DLIBXCP_BUILD_A2L=ON` 与 `-DLIBXCP_LIBA2L_ROOT=<准备根>`；
3. Boost 路径在脚本与主树两处分别维护。

**简化后一条命令即可：**

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_A2L=ON
```

用户决策（本次会话确认）：
- 集成方式 = **add_subdirectory 直接编译**（消除 IMPORTED target 与准备根概念）；
- Boost = **find_package 自动发现**，找不到再 FATAL_ERROR 给出明确指引；
- C++ 标准 = **保持各自不变**（主工程 C++20 / a2l-sdk C++23，按 target 隔离）；
- `build-sdk.ps1` = **保留**作为独立 SDK 开发入口，不再是主树前置步骤；
- `libxcp_measurement_adapter` = **保持独立 target**（维持核心↔桥接层隔离边界）。

## 2. 变更内容

### 2.1 根 `CMakeLists.txt`（原 L132–L189 整块替换）

删除：`LIBXCP_LIBA2L_ROOT` 缓存变量、6 个 `find_path/find_library/find_file`、
`liba2l::liba2l` 与 `libxcp::a2lbridge` 两个 SHARED/STATIC IMPORTED target 的创建。

新增逻辑：

1. **Boost 自动发现**（挂载前完成，结果缓存，上游 `script/boost.cmake` 见
   `Boost_FOUND` 已置位即跳过自身查找——幂等）：
   - 尊重用户显式 `-DBoost_DIR=<prefix>/lib/cmake/Boost-<ver>`；
   - Windows 本机约定 glob `C:/boost/lib/cmake/Boost-*`（多版本取最新）；
   - 其次尊重 `BOOST_ROOT` 环境变量（探测 `$ENV{BOOST_ROOT}/lib/cmake/Boost-*` 与 `$ENV{BOOST_ROOT}/Boost-*`）；
   - `find_package(Boost CONFIG COMPONENTS locale filesystem process HINTS ... NO_CMAKE_ENVIRONMENT_PATH)`；
   - 失败 → FATAL_ERROR 附带 `-DBoost_DIR` 用法示例。
2. `add_subdirectory(thirdparty/a2l-sdk)` —— 子工程真实 target 及其 ALIAS
   （`liba2l::liba2l`、`libxcp::a2lbridge`）在主树直接可见。
3. `libxcp_measurement_adapter` 定义保持不变（adapter/a2l 两文件 + PUBLIC 链接
   libxcp::libxcp 与 libxcp::a2lbridge）。

### 2.2 `thirdparty/a2l-sdk/CMakeLists.txt`

- 头注释更新：Boost 来源改为"两种消费形态"（主树 add_subdirectory 复用根工程
  find_package 结果 / build-sdk.ps1 独立构建传 `-DBoost_DIR`）。
- **C++ 标准隔离修复**（实测发现的必要改动）：目录作用域 `CMAKE_CXX_STANDARD 23`
  会经 `target_compile_features(PUBLIC cxx_std_23)` 传播给主树消费者，把
  gtest/libxcp_tests 等 C++20 目标抬到 stdcpplatest。现对 `liba2l` 显式
  `target_compile_features(PRIVATE cxx_std_23)` 并从 INTERFACE_COMPILE_FEATURES
  中剥离 `cxx_std_*`。跨 liba2l.dll 边界的 ABI 不受影响（导出宏整类导出）。

### 2.3 零改动部分（验证确认）

- `tests/CMakeLists.txt`、`examples/*/CMakeLists.txt`：引用的 target 名与 ALIAS
  完全一致，`$<TARGET_FILE:liba2l::liba2l>` POST_BUILD 复制照常工作；
- `build-sdk.ps1`：保留不动；
- `thirdparty/a2llib`（子模块）：未修改任何文件（AGENTS.md 约束）。

## 3. 验证结果

| 门禁 | 命令 | 结果 |
|------|------|------|
| OFF 回归 configure | `cmake -B build-verify-off -S . -DCMAKE_BUILD_TYPE=Release` | ✅ exit 0 |
| OFF 回归 build | `cmake --build build-verify-off --config Release` | ✅ exit 0，构建树无任何 a2l/liba2l/a2lbridge target |
| ON 一键 configure | `cmake -B build-verify-on ... -DLIBXCP_BUILD_A2L=ON` | ✅ 自动发现 `C:/boost/lib/cmake/Boost-1.86.0`，exit 0 |
| ON 完整构建 | `cmake --build build-verify-on --config Release` | ✅ exit 0（a2l.lib→liba2l.dll→libxcp_a2lbridge.lib 全链从零编通） |
| 标准隔离 | 检查 vcxproj | ✅ `libxcp_tests`=stdcpp20；`a2l`/`liba2l`=stdcpplatest |
| A2L 定向测试 | `ctest -R "A2lSmoke\|A2lGolden\|A2lE2E\|A2lIsolation\|a2l_gen\|a2l_perf"` | ✅ 6/6 Passed |
| 适配器测试 | `ctest -R "A2lMeasurementDatabase\|EventBoundMeasurementDatabase"` | ✅ 8/8 Passed |
| 全量套件 | `ctest --test-dir build-verify-on -C Release` | ✅ **478/478 Passed**（1 项参数化 Skip 为既有现象） |
| examples 门 | `-DLIBXCP_BUILD_A2L=ON -DLIBXCP_BUILD_XCPLITE_SLAVE=ON -DLIBXCP_BUILD_EXAMPLES=ON` configure + build measurement_demo/xcp_integration_demo | ✅ exit 0 |

环境事实：a2llib 子模块 SHA = `c31057498555cbb29ab48e718329ca3163c10221`（与
build-sdk.ps1 锁定值一致）；本机 Boost 1.86.0 位于 `C:\boost`。

## 4. 已知注意事项

- **vcpkg 全局集成干扰**：本机装有 vcpkg user-wide MSBuild 集成。根工程 Boost
  查找用 `NO_CMAKE_ENVIRONMENT_PATH` 屏蔽了 `BOOST_ROOT`/`Boost_ROOT` 环境变量
  劫持（vcpkg 发行版缺 locale/process 组件）；但 VS 生成器下链接期仍可能受
  vcpkg 注入影响——build-sdk.ps1 已有的 `VCPkgLocalAppDataDisabled=true` 经验
  同样适用于主树：若链接报 vcpkg 通配符伪项 LNK1104，设置该环境变量即可。
- **首次构建时间**：ON 模式首次配置需从零编译上游 a2llib（约数分钟），增量构建
  不受影响；追求预构建产物仍可用 `build-sdk.ps1`。
- **子模块未初始化**：ON 模式下 `add_subdirectory(thirdparty/a2l-sdk)` 会因
  `../a2llib` 缺失直接 CONFIGURE 报错，处置 = `git submodule update --init`。
- 验证用构建目录（build-verify-off/on/examples）为临时产物，可随手删除。
