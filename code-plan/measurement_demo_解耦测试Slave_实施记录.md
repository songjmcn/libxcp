# measurement_demo 解耦测试 Slave 实施记录

## 目标

让 `examples/measurement_demo` 不再依赖由 `LIBXCP_BUILD_TESTS` 控制的 `xcp_test_slave`；默认使用 `examples/xcp_master_udp` 创建的 XCPlite `cpp_demo`，并保留 `--slave` 对其他兼容 Slave 的覆盖能力。

## 修改

- `examples/measurement_demo/measurement_demo.cpp`：默认 `CPP_DEMO_EXECUTABLE`、UDP 端口 5555、项目 `cpp_demo`、事件 `mainloop`、dynamic 模式、采集 4 秒，默认测量 `temperature,counter` 并给出对应范围；帮助文本与顶部说明同步。`--slave` 仍可覆盖默认对手端，固定值模式需显式给出 `--expect`。
- 修正 `ResolveA2lEvents` 遗漏 EVENT `type` 和 `max_daq` 的问题：按 A2L `DAQ`/`STIM`/`DAQ_STIM` 映射 type 1/2/3，解析并校验 max_daq_list 的 uint8 范围后填入 metadata。此前该字段保留默认 0，导致动态事件被 runtime profile 错误拒绝。
- `examples/measurement_demo/CMakeLists.txt`：删除 `xcp_test_slave` target 存在性检查、`XCP_TEST_SLAVE_EXECUTABLE` 定义和依赖；改注入 `CPP_DEMO_EXECUTABLE="$<TARGET_FILE:cpp_demo>"`，仅依赖 `cpp_demo`。零参数 `ExampleMeasurementDemo` 现在验证新的默认 cpp_demo 流程；多变量与在线标定专测仍独立保留。
- `examples/CMakeLists.txt`：更新注释，明确测量示例使用同级已定义的 cpp_demo，不要求 tests。
- `docs/modules/示例程序.md`、根 `README.md`：说明 examples 可在 `LIBXCP_BUILD_TESTS=OFF` 时构建、更新默认启动行为和 CTest 命令。
- `code-plan/libxcp_测量子系统_v0.9_记录.md`：注明原 v0.9 记录的 tests 依赖限制已由本次变更消除。
- 未修改 `thirdparty/` 源文件。

## 验证

- 搜索 `examples/measurement_demo` 中 `xcp_test_slave`、`XCP_TEST_SLAVE_EXECUTABLE`、`testev`、旧端口 5556：无残留引用。
- `clang-format --dry-run --Werror examples/measurement_demo/measurement_demo.cpp`：通过。
- `git diff --check`：通过。
- AppleClang C++20 `-fsyntax-only`：通过；仅有既有注释中的 `/*` 格式警告。
- AppleClang Release 配置 `LIBXCP_BUILD_TESTS=OFF`、`LIBXCP_BUILD_EXAMPLES=ON` 成功（`Boost_USE_STATIC_LIBS=ON`、Python 3.12.13）；`CMAKE_CXX_COMPILER=/usr/bin/clang++`。用户授权后初始化 `thirdparty/XCPlite`，checkout `88e3cbeec79318f50067eede2502bcf4d34dab5f`；未修改子模块源码。
- 构建 `measurement_demo`、`cpp_demo`：通过。编译有既有注释格式及未使用局部变量 warning，无错误。
- `ctest --test-dir cmake-build-measurement-demo/examples -R '^(ExampleMeasurementDemo|ExampleCppDemoMeasurement|ExampleCppDemoCalibration)$' --output-on-failure`：3/3 通过，覆盖无参数默认流程、多变量测量、在线标定。
- 示例 CTest 注册在 `examples/CTestTestfile.cmake`；tests OFF 时仓库根 CTest 没有 test file，故文档命令使用 build tree 的 `examples/` 子目录。
- AppleClang C++20 `-fsyntax-only`：通过；`clang-format --dry-run --Werror examples/measurement_demo/measurement_demo.cpp` 与 `git diff --check`：通过。

## 待完成

- 若需额外确认 tests ON 与 examples ON 同时配置时的回归，可另建 Release build tree 检查；本次核心验收已在 tests OFF/examples ON 下完成。Windows/Linux 的独立平台运行不在当前 macOS 环境内验证。
