# 通用 XCP Master 工具 FTXUI 重写实施记录

## 状态

- 用户于 m00119 批准《通用XCP_Master工具_FTXUI重写计划.md》并授权实施。
- FTXUI TUI、A 档标量标定、模型/策略测试及文档已实现。AppleClang Release 下 `xcp_master`、`XcpMasterSymbolPolicyTest`、`A2lLogTimestampTest` 聚焦目标构建通过，相关 CTest 26/26 通过。
- 用户 m03145 批准并完成 `tests/xcp_daq_test.cpp:114` narrowing 修复后，clean 的 AppleClang Release 全目标构建成功；Python 3.12 下全量 CTest 共 514 项、0 失败、1 项显式跳过。GCC 15 定向构建仍因已安装 Boost.Locale 的 libc++ ABI 与 GCC/libstdc++ ABI 不匹配而链接失败；Windows/Linux 尚未验证。
- 按用户授权更新项目自有 `thirdparty/a2l-sdk` 集成层；未修改 `thirdparty/a2llib` 子模块源码、Boost 或协议核心公共接口。未连接真实 Slave、未执行真实标定写入。

## 实施内容

### TUI 与线程调度

- `tools/xcp_master/main.cpp` 将普通 `getline` 循环改为 FTXUI 全屏交互界面：观测、变量标定、命令结果、日志四视图；固定命令输入区与状态/控制栏；F2 循环切视图，Tab 切焦点，Esc 返回命令输入。命令框焦点时 ↑/↓ 浏览最多 200 条历史并恢复草稿，F5 请求优先停止。
- 后台 POLL/DAQ/命令输出经 UI 模型显示，不覆盖输入；命令通过单一后台线程串行执行。普通业务命令忙时拒绝，stop/quit 等控制请求优先。
- 写入和 DAQ 确认使用 UI 模态 gate：仅 Y/y 明确确认，N/n/Esc 取消，默认取消；退出时取消未决确认并唤醒等待线程。运行交互 TUI 要求 stdin/stdout 为终端，命令参数帮助与错误仍可重定向。

### 标量标定与安全边界

- 独立标定视图只搜索 A2L 标量 `CHARACTERISTIC/VALUE`，显示策略拒绝原因；允许选择、读当前物理值、编辑目标值、明确确认写入并回读。不支持数组、曲线、MAP、Flash。
- 继续复用 `SymbolWriteUnsupportedReason`、`ScalarLayoutUnsupportedReason`、`EncodedScalarUnsupportedReason`、A2L `FromPhysical`/`ToPhysical`；保留有限数值、地址粒度、字节序、宽度、DAQ 活跃期间禁手动写等校验。A2L `read_write` 不视为 ECU 授权；传输异常可能产生部分写入，不宣称写操作原子。
- 保留原 CLI 参数/命令，增加 `find [pattern]`。

### FTXUI 获取与文档

- `tools/xcp_master/CMakeLists.txt` 仅在 `LIBXCP_BUILD_TOOLS=ON` 时通过 FetchContent 获取 FTXUI v7.0.3，固定提交 `f921fad208912747c17d129a8ef75ec7624b6eec`，链接 `ftxui::ftxui`；不使用 submodule、不写入 `thirdparty/`。只关闭 FTXUI examples/docs/tests/install，不覆盖用户 CMake Cache。
- `tools/xcp_master/README.md` 说明 UI、FTXUI 自动获取/离线覆盖方式、Windows/Linux/macOS 目标和 Boost/A2L 编译器 ABI 前提；另说明项目自有 A2L SDK 显式使用 Boost.Locale。
- 官方 API 依据及锁定版本链接见本计划文件 §5；FTXUI tools=OFF 不下载已通过独立 Release configure 验证（无 FTXUI Cache 项、无 `_deps` 目录）。

### A2L SDK 跨平台集成

- 用户明确授权修改项目自有 `thirdparty/a2l-sdk`，但未授权改 Boost 或 `thirdparty/a2llib` 子模块源码。`thirdparty/a2l-sdk/CMakeLists.txt` 在 `a2l` target 排除上游 `src/a2llogstream.cpp`，加入项目自有 `src/a2llogstream_boost_locale.cpp`，并将 `Boost::locale` 显式加入 `liba2l` PRIVATE link dependencies。
- 新增 `thirdparty/a2l-sdk/src/a2l_log_timestamp.hpp`，接口 `FormatLocalLogTimestamp(std::time_t)` 使用 Boost.Locale 本地时区格式化；替代 logger 保留原 callback、日志级别、source location 和 console 输出字段。
- AppleClang 构建还发现上游 `amlsection.cpp` 使用 `std::ranges::find_if` 缺 `<algorithm>`，`a2ltask.cpp` 使用 `std::ostringstream` 缺 `<sstream>`。为遵守“不改子模块源码”，在 SDK 的 `a2l` target 以 `target_precompile_headers(a2l PRIVATE <algorithm> <sstream>)` 提供这些标准头；定向 Release 构建验证通过。
- `tests/CMakeLists.txt` 新增 `A2lLogTimestampTest`；同时给 `XcpMasterSymbolPolicyTest` 增项目 `include/` 搜索路径，修复策略测试构建找不到 `libxcp/protocol_types.hpp` 的问题。

## 测试与验证

- `tests/xcp_master_calibration_policy_test.cpp` 覆盖标量布局、只读/非 VALUE characteristic 拒绝、地址粒度/宽度/字节序不兼容；`tests/xcp_master_tui_model_test.cpp` 覆盖有界行缓冲、最新观测更新/淘汰/停止标记、命令历史/草稿、确认决定/gate、F2 视图索引。计划 §7.1 的真实 FTXUI 渲染/焦点事件、完整业务并发和应用生命周期仍未覆盖。
- `tests/a2l_log_timestamp_test.cpp` 覆盖 UTC epoch 格式化和 console logger 字段输出；Boost.Locale logger/source 与测试语法检查通过，独立手动链接运行 2/2 通过。另以系统 `TZ=America/Los_Angeles`、`TZ=Asia/Tokyo` 独立探针格式化 POSIX epoch，分别得到 `16:00:00`、`09:00:00`，确认使用系统本地时区偏移。
- AppleClang Release 配置成功：Boost 1.86.0 CONFIG + `-DBoost_USE_STATIC_LIBS=ON`，FTXUI 7.0.3 按 pinned commit 配置。`xcp_master`、`XcpMasterSymbolPolicyTest`、`A2lLogTimestampTest` 定向构建通过。
- 按用户 m03000 要求，新建 `cmake-build-release-clang`，显式设置 `-DCMAKE_CXX_COMPILER=/usr/bin/clang++`；CMake 识别 AppleClang 21.0.0.21000334，使用 Boost 1.86.0 static package 和本地 FTXUI/GoogleTest FetchContent 源。`xcp_master`、`XcpMasterSymbolPolicyTest`、`A2lLogTimestampTest` 均构建成功，验证现有 libc++ Boost.Locale 可与该 Clang/libc++ 配置链接。
- 定向 CTest：原 `cmake-build-release` 与新 `cmake-build-release-clang` 均运行 XCP Master/标定策略/TUI模型/Boost.Locale日志/CLI smoke suites；新 Clang tree 为 **26/26 通过**（XCP symbol/calibration/TUI model 20 项、logger 2 项、CLI smoke 4 项）。
- 按用户 m03145 批准修复 `tests/xcp_daq_test.cpp:114` narrowing：将运行时 DAQ 状态条件值显式转换为 `std::uint8_t`，并给 `GetStatus` case 加局部作用域，避免跨 case 跳过变量初始化。Clang Release 的 `libxcp_tests` 构建通过，`ctest -R '^XcpDaq'` 32/32 通过。
- clean 后重新完成 `cmake-build-release-clang` 全目标 Release 构建；随后以 `/opt/homebrew/bin/python3.12` 运行全量 CTest，共 514 项、0 失败、1 项按测试定义显式 Skipped。系统默认 Python 3.9 运行 A2L 生成器时曾报 `TypeError: write_text() got an unexpected keyword argument 'newline'`；改用已安装 Python 3.12 运行，未改生成器源码。
- GCC 15 重新 clean/configure 并尝试 `xcp_master` 定向 Release 构建。CMake 已确认 `Boost::locale` 显式进入 link command，但最终仍报 Boost archive 中 `std::__1::basic_string`/libc++ symbols 与 GCC/libstdc++ ABI 不匹配（目标要求 `std::__cxx11`）；因此显式 link 修复缺库问题，不解决当前 Boost binary ABI。需换与工具链 ABI 匹配的 Boost build 后再验证 GCC。
- `clang-format --dry-run --Werror` 覆盖所有修改的 C++ 源/头文件，通过；`git diff --check` 通过。AppleClang 与 GCC 15 对 `tools/xcp_master/main.cpp` 的 C++20 `-fsyntax-only` 已通过；隔离 FTXUI 7.0.3 API probe/Release build 通过。Windows/Linux 构建与设备联测尚未执行。

## 尚待验证

1. 使用 ABI 匹配的 Boost.Locale 在 GCC 15 完成 `xcp_master` 定向构建和相关 CTest；当前 `/usr/local/boost` archive 是 libc++ ABI。
2. 在 Windows、Linux 运行完整配置/构建/测试，核验终端输入、退出恢复及 FetchContent。
3. 补计划 §7.1 中真实 FTXUI 渲染/焦点、命令/确认完整时序、并发与生命周期测试。真实 Slave 联测仍可选；不进行未经用户要求的标定写操作。
