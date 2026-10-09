# CMake 去除 LIBXCP_BUILD_A2L 开关（A2L 必编化）实施记录

日期：本轮批次（第二轮简化任务）。计划批准：用户确认"去除 A2L 编译对开关的依赖"
（立项 m00140，批准 m00161）。

## 1. 动机与决策

xcp 工程必须编译 A2L 依赖，开关不再有存在意义。目标：日常构建回归最简——

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release
```

用户已确认的两项决策：
- **Boost 缺失策略**：configure 期硬报错（FATAL_ERROR + `-DBoost_DIR` 指引），不降级；
- **文档范围**：活文档全部同步；code-plan/ 历史记录不动。

行为变化（知情项）：
1. Boost 成为所有构建的无条件硬依赖（locale/filesystem/process 组件，CONFIG 模式）；
2. 全新 build 目录首编要从零编上游 a2llib（数分钟级）；增量不受影响；
3. `LIBXCP_HAS_A2LBRIDGE` 宏删除（全仓 grep 证实无任何代码条件编译使用它）。

## 2. 变更清单

### CMake

| 文件 | 改动 |
|---|---|
| `CMakeLists.txt`（根） | 删 `option(LIBXCP_BUILD_A2L ...)` 与包裹块 `if(...)...endif()`；Boost 自动发现 → `find_package(Boost CONFIG COMPONENTS locale filesystem process NO_CMAKE_ENVIRONMENT_PATH)` → FATAL_ERROR → `add_subdirectory(thirdparty/a2l-sdk)` → `libxcp_measurement_adapter` 全部提升为顶层无条件执行；消息前缀改中性 `libxcp:`；examples 前置门删除 `if(NOT LIBXCP_BUILD_A2L) FATAL_ERROR`（仅保留 XCPLITE_SLAVE 门）；头部注释重写为"A2L 栈必编组件" |
| `tests/CMakeLists.txt` | XcpliteIntegration：A2L 用例源（`xcplite_a2l_read_test.cpp`、`xcplite_measurement_test.cpp`）并入主 `add_executable`，链接 `libxcp::a2lbridge libxcp_measurement_adapter` 与 POST_BUILD dll 复制无条件化；删 `LIBXCP_HAS_A2LBRIDGE=1` 定义；A2L 专项大块（Python3/A2lSmoke/A2lGolden/A2lE2E/MeasurementAdapterTest/a2l_perf_gen/ParseLargeFileUnderBudget/A2lIsolation）去 `if(LIBXCP_BUILD_A2L)` 包裹并整体降一级缩进，随 `LIBXCP_BUILD_TESTS` 无条件构建 |
| `examples/CMakeLists.txt`、`examples/measurement_demo/CMakeLists.txt`、`thirdparty/a2l-sdk/CMakeLists.txt` | 注释同步（无逻辑改动） |
| `thirdparty/a2l-sdk/build-sdk.ps1` | 头注释与输出文案中 `LIBXCP_LIBA2L_ROOT` 字样改为"独立 SDK 分发场景准备根"（无逻辑改动） |

### 测试源注释（无代码逻辑改动）

`tests/xcplite_a2l_read_test.cpp:7`、`tests/xcplite_measurement_test.cpp:8`、
`tests/measurement_adapter_test.cpp:16`、`tests/a2l_smoke_test.cpp:11` ——
"仅在 LIBXCP_BUILD_A2L=ON 时编译"表述改为"A2L 栈为必编组件"。

### 活文档

`README.md` §3.5/§4（开关表删 A2L 行、最简命令、Boost 说明、测试分层措辞）、
`docs/modules/构建系统.md`（§2 目标表、§4 开关表改 3 个 + Boost 硬依赖说明、§5 离线依赖）、
`docs/modules/A2L集成.md`（归属行、§1 定位、§2 组成表、§4 构建命令）、
`docs/modules/测试.md`（L3/A2L 专项行）、`docs/modules/示例程序.md`（前置条件+命令）、
`docs/XCPlite_对手端协议调试指南.md`（开关表+命令）、
`examples/xcp_master_udp/README.md`、`examples/xcp_integration_demo/README.md`（命令去开关）。

## 3. 验证结果

| 项 | 命令/判据 | 结果 |
|---|---|---|
| 裸 configure | `cmake -B build-verify -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_TESTS=ON`（无任何 A2L 参数） | ✅ exit 0；自动发现 `C:/boost/lib/cmake/Boost-1.86.0` |
| 裸构建 | `cmake --build build-verify --config Release` | ✅ exit 0 |
| A2L 产物在树 | `liba2l.dll` / `libxcp_a2lbridge.lib` / `libxcp_measurement_adapter.lib` | ✅ 均存在 |
| 全量 ctest（裸配置） | `ctest --test-dir build-verify -C Release` | ✅ **478/478 Passed**（1 项既有 skip：AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8） |
| examples 门 | `-DLIBXCP_BUILD_XCPLITE_SLAVE=ON -DLIBXCP_BUILD_EXAMPLES=ON`（无 A2L 开关）configure+build 全目标 | ✅ exit 0 |
| examples 全量 ctest | 524 项 | ⚠️ 523 通过 + 1 既有失败（见 §4）+ 既有 skip |
| 残留引用 | grep `LIBXCP_BUILD_A2L\|LIBXCP_LIBA2L_ROOT\|LIBXCP_HAS_A2LBRIDGE`（活文件范围） | ✅ 仅剩 code-plan/ 历史记录（按决策不动） |

## 4. 既有失败（非本轮引入，已基线对照证实）

`ExampleXcpIntegrationDemoCppDemo`：cpp_demo 上 `SET_DAQ_LIST_MODE (daq=0,event=0)
event channel redefinition → ERR_DAQ_CONFIG`。**对照实验**：检出 HEAD（旧机制
`-DLIBXCP_BUILD_A2L=ON -DLIBXCP_LIBA2L_ROOT=...`）建 `build-baseline-check` 全量
构建后单跑该用例——同样 Failed。判定为既有问题（疑与该 run-dir 下历史生成的
cpp_demo_V201.a2l 状态相关），不在本轮收口范围，留待后续批次处理。

## 6. 追加批次：EXAMPLES 自动跟随打开 XCPLITE_SLAVE（用户建议 m00364）

User said (m00364): "LIBXCP_BUILD_EXAMPLES=ON requires LIBXCP_BUILD_XCPLITE_SLAVE=ON。
我建议LIBXCP_BUILD_EXAMPLES开启后，自动打开LIBXCP_BUILD_XCPLITE_SLAVE"。

实现（根 CMakeLists.txt）：`option(LIBXCP_BUILD_EXAMPLES)` 前移至 XCPlite 段之前
（约 L208-210），XCPlite 段的 `option(LIBXCP_BUILD_XCPLITE_SLAVE)` 之后加判定块
`if(LIBXCP_BUILD_EXAMPLES AND NOT LIBXCP_BUILD_XCPLITE_SLAVE)` → STATUS 提示 +
`set(... ON CACHE INTERNAL ...)`；原 EXAMPLES 门内 FATAL_ERROR 前置检查删除；
examples/CMakeLists.txt 头注释同步。行为：显式 `-DLIBXCP_BUILD_XCPLITE_SLAVE=OFF`
+ EXAMPLES=ON 时自动覆盖为 ON（跟随语义）。

验证：全新目录仅 `-DLIBXCP_BUILD_EXAMPLES=ON` 一次 configure exit 0（STATUS 提示
出现、xcplite 子项目加入、examples 三目标配置生成）；全量构建 exit 0，产物齐备
（xcp_master_udp/measurement_demo/xcp_integration_demo/cpp_demo/xcplite.lib）；
重配置幂等；build-verify 回归 ctest **478/478 Passed**。

文档同步：README.md §3.5 表、docs/modules/构建系统.md §4 表、docs/modules/示例
程序.md §1 命令、examples/xcp_master_udp/README.md §1、examples/xcp_integration
_demo/README.md 构建节、docs/XCPlite_对手端协议调试指南.md 开关表。临时目录
build-examples-auto 已清理。

## 7. 注意事项

- vcpkg 全局 MSBuild 集成仍会劫持裸 `find_package(Boost)`（其发行版缺 locale/process），
  故查找保持 `NO_CMAKE_ENVIRONMENT_PATH`；受干扰机器可设 `VCPkgLocalAppDataDisabled=true`
  或显式传 `-DBoost_DIR`。
- Linux/macOS：走系统 Boost Config 包或显式 `-DBoost_DIR`；本机未验证非 Windows 平台。
- 子模块 `thirdparty/a2llib` 未初始化时 `add_subdirectory` 直接报错（明示先
  `git submodule update --init`）。
- 临时验证目录 `build-verify`（保留可复用）；`build-verify-off/on/examples`、
  `build-baseline-check` 已清理。
