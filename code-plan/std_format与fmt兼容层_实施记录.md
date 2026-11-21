# std::format 与 fmt 兼容层实施记录

- 日期：2026-11-21
- 分支：master
- 起因：WSL2 Ubuntu 22.04 + GCC 11.4 构建验证时，`tests/xcp_master_render_bench_test.cpp` 因缺少 `<format>` 编译失败（linux 侧唯一失败目标）

---

## 1. 问题与范围

`std::format` 在本仓有两处**硬依赖**：

| 位置 | 触发条件 | 说明 |
| --- | --- | --- |
| `include/libxcp/log.hpp:14` + `:45/:59/:73` | `LIBXCP_ENABLE_LOG=ON` | 三个日志宏体调用 `std::format(__VA_ARGS__)` |
| `tests/xcp_master_render_bench_test.cpp:27` + `:99/:239/:250` | **无条件** | 基准测试直接 `#include <format>`，与日志开关无关 |

GCC 11 的 libstdc++ 尚未提供 `<format>`（`__cpp_lib_format` 未定义，实测 `g++ 11.4.0` 确认），
因此：

- 关闭日志时，库本身可以编译；
- **基准测试无法编译** → ctest 出现 `XcpMasterRenderBenchTest_NOT_BUILT (Not Run)`；
- 打开日志时，所有包含 `log.hpp` 的 TU 全部失败。

### 本仓实际用到的格式说明符（全量统计）

| 说明符 | 出现次数 |
| --- | --- |
| `{}` | 174 |
| `{:.1f}` | 13 |
| `{:.2f}` | 4 |
| `{:.0f}` | 2 |
| `{:06d}` | 1 |

全部落在 `std::format` 与 `fmt` 的**公共子集**内，语法零差异，因此可以无缝切换后端。

### 范围边界

- 只处理上述两处；不改任何日志调用点（307 处 `LIBXCP_LOG_*` 引用一行未动）。
- 不引入新的运行时依赖：`std::format` 可用时完全不碰 fmt。
- `thirdparty/a2llib/src/a2llogstream.cpp` 里的 `std::format(L"{:L%x %R}")` 属独立子项目，已被
  `thirdparty/a2l-sdk/CMakeLists.txt:63` 的 `list(FILTER ... EXCLUDE ...)` 排除在主构建之外，本次不涉及。

---

## 2. 方案

三层结构，职责分离：

### 2.1 CMake 探测与回退（`CMakeLists.txt`）

探测从 `if(LIBXCP_ENABLE_LOG)` 内部**上移到顶层无条件执行**——因为基准测试不依赖日志开关。

```cmake
check_cxx_source_compiles([[
  #include <version>
  #if !defined(__cpp_lib_format) || (__cpp_lib_format < 201907L)
  #  error "std::format is unavailable"
  #endif
  #include <format>
  int main() {
    const auto formatted = std::format("value={} hex={:06d} f={:.1f}", 42, 42, 1.5);
    return formatted.empty();
  }
]] LIBXCP_HAS_CXX20_STD_FORMAT)
```

探测同时校验宏与真实调用（含本项目在用的 `{:06d}` / `{:.1f}`），避免"有头文件但实现不完整"的标准库被误判为可用。

不可用时回退 fmt：版本固定 **11.2.0**，URL + SHA256 钉死：

```
URL https://github.com/fmtlib/fmt/archive/refs/tags/11.2.0.tar.gz
URL_HASH SHA256=bc23066d87ab3168f27cef3e97d545fa63314f5c79df5ea444d41d56f962c6af
```

随后建立后端入口目标：

```cmake
add_library(libxcp_format INTERFACE)
add_library(libxcp::format ALIAS libxcp_format)
target_include_directories(libxcp_format INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
if(NOT LIBXCP_HAS_CXX20_STD_FORMAT)
  target_compile_definitions(libxcp_format INTERFACE LIBXCP_HAVE_FMT=1)
  target_link_libraries(libxcp_format INTERFACE fmt::fmt-header-only)
endif()
```

`libxcp` 以 `PUBLIC` 方式链接 `libxcp::format`，因为 `log.hpp` 是安装的公共头。

### 2.2 后端兼容头（`include/libxcp/detail/format_compat.hpp`，新增）

自行探测后端，**不依赖编译定义从目标传播下来**：

```cpp
#if defined(__cpp_lib_format) && (__cpp_lib_format >= 201907L)
#include <format>
#define LIBXCP_FORMAT_BACKEND_STD 1
#elif defined(LIBXCP_HAVE_FMT)
#include <fmt/format.h>
#define LIBXCP_FORMAT_BACKEND_FMT 1
#else
#error "缺少 std::format；请通过 CMake 构建（配置期会自动启用 fmt 回退）"
#endif
```

统一入口 `calmcar::xcp::detail::FormatText`，两个分支都用带编译期检查的 `format_string`：

```cpp
template <class... Args>
[[nodiscard]] inline std::string FormatText(std::format_string<Args...> pattern,
                                            Args&&... args);   // 或 fmt::format_string
```

自探测而不是靠宏传播，意义在于：某个目标漏链 `libxcp::format` 时会在**编译期立刻报错**，
而不是悄悄退化到别的实现。

### 2.3 调用点替换（共 6 处）

| 文件 | 改动 |
| --- | --- |
| `include/libxcp/log.hpp` | `#include <format>` → `#include "libxcp/detail/format_compat.hpp"`；3 处宏体 `std::format(__VA_ARGS__)` → `::calmcar::xcp::detail::FormatText(__VA_ARGS__)`；顺便把宏续行整理到 80 列以内 |
| `tests/xcp_master_render_bench_test.cpp` | `#include <format>` → `#include "libxcp/detail/format_compat.hpp"`；3 处 `std::format(` → `calmcar::xcp::detail::FormatText(` |
| `tests/CMakeLists.txt` | 注册 `format_compat_test.cpp`；`XcpMasterRenderBenchTest` 补链 `libxcp::format`（它只链 `libxcp::a2lbridge`，拿不到 libxcp 的 PUBLIC 依赖） |

---

## 3. 关键决策与理由

| 编号 | 决策 | 理由 |
| --- | --- | --- |
| D1 | 探测移到顶层、无条件执行 | 基准测试使用 `std::format` 与日志开关无关；否则关日志时仍会编译失败 |
| D2 | 探测源码同时校验 `__cpp_lib_format` 与真实调用 | 只用宏会漏掉"声明了但未实现"的工具链；`{:06d}`/`{:.1f}` 是本仓真实用到的形态 |
| D3 | 回退库用 `fmt-header-only`（INTERFACE） | 不产生链接产物，公共头不引入二进制 ABI 依赖；本仓 `install()` 只有 archive + 头目录、无 CMake package export，因此也不必写 `find_dependency(fmt)` |
| D4 | 默认走 FetchContent，系统 fmt 需 `-DLIBXCP_USE_SYSTEM_FMT=ON` | 实测 WSL 下 `find_package(fmt CONFIG QUIET)` 会命中 `/mnt/c/Users/<user>/miniconda3/Library/lib/cmake/fmt`——Windows 侧的 conda 安装。跨平台误用会造成头文件/二进制不匹配，且 `/mnt/c` 的 IO 很慢 |
| D5 | 公共头自探测后端 | 不依赖编译定义的传播链；漏链目标在编译期就报错 |
| D6 | 调用点语法零改动 | 307 处日志调用与 3 处基准测试调用的格式串写法保持不变，差异全部收敛在宏体/兼容头内 |
| D7 | 版本固定 11.2.0 + SHA256 | 与 glog（`v0.7.1` + SHA256）的处理方式一致，保证可复现构建 |

---

## 4. 验证结果

### 4.1 Windows（MSVC 19.x / CMake 4.0.2 / Ninja，走 std::format 分支）

| 步骤 | 结果 |
| --- | --- |
| configure | `-- libxcp: 使用标准库 std::format`；`Configuring done (3.7s)`；退出码 0 |
| build（`libxcp_tests` + `XcpMasterRenderBenchTest` + `xcp_master`） | 16/16 完成，退出码 0，零新增告警（仅 glog 既有 C4244/C4996） |
| `--gtest_filter="FormatCompat*"` | **8/8 通过**（0 ms） |
| 全量 ctest | **1454 项全部通过、0 失败**（340.85 s） |

### 4.2 Linux（WSL2 Ubuntu 22.04 / GCC 11.4，走 fmt 分支）

| 步骤 | 结果 |
| --- | --- |
| configure | `-- libxcp: 当前工具链缺少 std::format，回退到 fmt`；`-- {fmt} version: 11.2.0`；fmt 下载 + 配置共 17.7 s；退出码 0 |
| build（`XcpMasterRenderBenchTest` + `libxcp_tests`） | **79/79 完成，`error:` 计数 0**；之前失败的目标 `xcp_master_render_bench_test.cpp.o` 正常编过 |
| `--gtest_filter="FormatCompat*"` | **8/8 通过**（17 ms），即 fmt 后端输出与预期一致 |
| 日志开启组合（`-DLIBXCP_ENABLE_LOG=ON`，独立构建目录 `cmake-build-linux-log`） | configure 成功（glog 自动获取）；`libxcp` 46/46 编译链接成功，`libxcp.a` 3.34 MB，`error:` 计数 0。其中 `src/session.cpp`、`src/xcp_master.cpp`、`src/measurement/measurement_session.cpp` 等**实际使用 `LIBXCP_LOG_*` 的 TU 全部编过**，证明 glog + fmt 组合可用 |
| 全量 ctest | 见 §4.3 |

### 4.3 全量回归对比

| 平台 | 改动前 | 改动后 |
| --- | --- | --- |
| Linux GCC 11 | 1435 项：1434 通过 / **1 失败（`XcpMasterRenderBenchTest_NOT_BUILT`）** | **1447 项全部通过，0 失败**（703.53 s） |
| Windows MSVC | 1447 项：1446 通过 / 0 失败 | **1454 项全部通过、0 失败**（340.85 s） |

### 4.4 一个与本次改动无关的抖动观察

Windows 侧在**与 Linux 全量 ctest 并发执行**的两轮里，
`UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection` 失败（2.3 s 超时），
但同一二进制：

- 单独运行：**173 ms 通过**；
- 无并发负载下重跑全量 ctest：**1454/1454 全部通过**。

该用例固定绑定 UDP 端口 43150（`tests/udp_test_slave_test.cpp:448-472`），
与格式化后端无关；判定为机器负载导致的既有抖动，本次不做改动，仅记录。

### 4.5 Linux 未运行项（均为既有 Disabled / Skipped）

```
236  TcpStreamDecoderFinish.HeaderOnlyIsTruncated (Disabled)
967  AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8 (Skipped)
1173 SnapshotTest.StagesReadableCopyOfSource (Skipped)
1177 SnapshotTest.CreatesDistinctFilesPerCall (Skipped)
1178 SnapshotTest.DeletesSnapshotWhenGuardReleased (Skipped)
1341 XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList (Skipped)
```

最后一项是新出现的跳过：该用例依赖 40 MB 合成 A2L 夹具，无夹具时自行 `GTEST_SKIP()`，
属用例自身设计（与 `<format>` 无关）。同一可执行文件的另外两条用例已正常运行。

---

## 5. 新增测试

`tests/format_compat_test.cpp`（8 个用例，覆盖本仓实际用到的全部形态）：

| 用例 | 覆盖点 |
| --- | --- |
| `FormatsPlainPlaceholders` | `{}` 对 `int` / `const char*` / `std::string` / `std::string_view` |
| `FormatsZeroPaddedInteger` | `{:06d}`，含超出宽度的 `1234567` |
| `FormatsFixedPrecision` | `{:.1f}` / `{:.2f}` / `{:.0f}` |
| `FormatsMixedPlaceholders` | 混合形态（与日志真实调用一致） |
| `EscapesBraces` | `{{` / `}}` 转义 |
| `IgnoresExtraArguments` | 参数多于占位符时被忽略 |
| `BuildsBenchmarkPreviewLabel` | 基准测试 `MakePreviewLabel` 的真实标签形态 |
| `BuildsBenchmarkSkipReason` | 基准测试跳过原因文案（`{:.1f}` / `{:.0f}` / `{:.2f}`） |

断言用**逐字节相等的期望字符串**，因此两个后端必须产出一致结果才算通过。

---

## 6. 已知限制

1. 回退库只覆盖 `std::format` 与 `fmt` 的公共语法子集。若将来引入 `{:>8}`（对齐）、`{:x}`（十六进制）、
   `{:L}`（本地化）等 fmt 扩展或 std 扩展说明符，需要重新评估两边一致性。
2. 回退路径依赖配置期联网下载 fmt（首次）。离线环境可用
   `-DFETCHCONTENT_SOURCE_DIR_FMT=<本地 fmt 源码>` 复用已有源码，
   或 `-DLIBXCP_USE_SYSTEM_FMT=ON` 配合系统 CONFIG 包（需确认平台匹配）。
3. `libxcp` 以 `PUBLIC` 暴露 `libxcp::format`。本仓 `install()` 不导出 CMake package，
   因此安装后的外部消费者若在无 `std::format` 的工具链上使用 `log.hpp`，需自行提供 fmt。
4. 未验证 macOS 侧（Clang/libc++ 的 `__cpp_lib_format` 版本差异），CI 有 macOS 任务，留给流水线确认。

---

## 7. 复现命令

### Windows（std::format 分支）

```bat
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --target libxcp_tests XcpMasterRenderBenchTest xcp_master -j 4
cmake-build-release\tests\libxcp_tests.exe --gtest_filter="FormatCompat*"
```

### Linux（fmt 回退分支）

```bash
"$HOME/opt/cmake-pkg/cmake/data/bin/cmake" -B cmake-build-linux -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBoost_DIR=/usr/local/boost/lib/cmake/Boost-1.86.0 \
  -DLIBXCP_BUILD_TOOLS=ON -DLIBXCP_BUILD_EXAMPLES=ON \
  -DFETCHCONTENT_SOURCE_DIR_FTXUI=/mnt/d/project/libxcp/cmake-build-release/_deps/ftxui-src
"$HOME/opt/cmake-pkg/cmake/data/bin/cmake" --build cmake-build-linux \
  --target XcpMasterRenderBenchTest libxcp_tests -j 4
./cmake-build-linux/tests/libxcp_tests --gtest_filter="FormatCompat*"
```

### Linux + 日志开启（验证 glog + fmt）

```bash
"$HOME/opt/cmake-pkg/cmake/data/bin/cmake" -B cmake-build-linux-log -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLIBXCP_ENABLE_LOG=ON \
  -DBoost_DIR=/usr/local/boost/lib/cmake/Boost-1.86.0 \
  -DLIBXCP_BUILD_TOOLS=OFF -DLIBXCP_BUILD_EXAMPLES=OFF -DLIBXCP_BUILD_TESTS=OFF
"$HOME/opt/cmake-pkg/cmake/data/bin/cmake" --build cmake-build-linux-log --target libxcp -j 4
```

---

## 8. 改动文件清单

| 文件 | 类型 |
| --- | --- |
| `CMakeLists.txt` | 修改（探测上移、fmt 回退、`libxcp::format` 目标） |
| `include/libxcp/detail/format_compat.hpp` | **新增** |
| `include/libxcp/log.hpp` | 修改（3 处宏体 + 头文件引用 + 注释 + 续行对齐） |
| `tests/xcp_master_render_bench_test.cpp` | 修改（头文件 + 3 处调用） |
| `tests/format_compat_test.cpp` | **新增** |
| `tests/CMakeLists.txt` | 修改（注册单测、基准测试补链） |
