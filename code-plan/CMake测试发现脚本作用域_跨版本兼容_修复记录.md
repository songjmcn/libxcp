# CMake 测试发现脚本作用域（CMake ≤ 3.28）修复记录

## 1. 现象

在 Ubuntu 24.04（CMake 3.28.3 + Ninja）上做全量编译 + 串行全量测试时，
1161 条 ctest 用例中有且仅有 1 条失败，且失败项不是真实用例：

```
1161/1161 Test #1161: XcpliteV12InteropTest_NOT_BUILT .......***Not Run   0.00 sec
Could not find executable XcpliteV12InteropTest_NOT_BUILT
99% tests passed, 1 tests failed out of 1160
```

`XcpliteV12InteropTest_NOT_BUILT` 是 `gtest_discover_tests()` 在构建期**发现失败**时
由 CMake 生成的占位符用例（`*_NOT_BUILT`），ctest 把它计入失败。

对照组：同一份源码在 Windows（CMake 4.0.2 + VS2022）上发现正常，
`XcpliteV12InteropTest` 的 7 条用例全部注册并可执行；
直接运行该可执行文件在 Linux 上也是 7/7 通过（3.7 s）。

## 2. 根因

`gtest_discover_tests()` 需要把 CMake 自带的发现脚本
`Modules/GoogleTestAddTests.cmake` 传给 `cmake -P` 执行，脚本路径来自模块内部变量。

- **CMake 3.28.3**（`/usr/share/cmake-3.28/Modules/GoogleTest.cmake:576-577`）：

  ```cmake
  set(_GOOGLETEST_DISCOVER_TESTS_SCRIPT
    ${CMAKE_CURRENT_LIST_DIR}/GoogleTestAddTests.cmake
  )
  ```

  这是普通的**目录作用域**变量。仓库里 `include(GoogleTest)` 只出现在
  `tests/CMakeLists.txt:105`，该变量只存在于 `tests/` 目录作用域；
  `examples/xcp_slave_demo_v12/` 是 `tests/` 的**兄弟目录**，继承不到，
  于是生成命令里的脚本路径为空。

- **CMake 4.0**（`GoogleTest.cmake:688`）：

  ```cmake
  -P "${CMAKE_ROOT}/Modules/GoogleTestAddTests.cmake"
  ```

  改用 `${CMAKE_ROOT}` 直接拼绝对路径，与调用者作用域无关，因此 Windows 侧正常。

**证据**（同一份 `build.ninja` 内对比，构建目录 `~/libxcp-full`）：

```
# tests/ 下的 15 个目标（正常）
POST_BUILD = ... -P /usr/share/cmake-3.28/Modules/GoogleTestAddTests.cmake

# examples/xcp_slave_demo_v12 下的 V12 目标（异常，全仓库仅此 1 处）
POST_BUILD = ... -D TEST_XML_OUTPUT_DIR= -P ""
```

`-P ""` ⇒ 发现脚本根本没有执行 ⇒ `XcpliteV12InteropTest[1]_tests.cmake`
不会被生成 ⇒ `XcpliteV12InteropTest[1]_include.cmake` 走 `else()` 分支
注册占位符（该 include 脚本内容见构建目录）。

## 3. 改法

在 `examples/xcp_slave_demo_v12/CMakeLists.txt` 的 `if(LIBXCP_BUILD_TESTS)`
块内、`gtest_discover_tests(...)` 之前补一行 `include(GoogleTest)`，
并附中文注释说明原因。`include()` 幂等，对 CMake 4.0 / CI 无副作用。

```cmake
  include(GoogleTest)
  gtest_discover_tests(XcpliteV12InteropTest DISCOVERY_TIMEOUT 60
                       PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
```

注意：修改后必须**重新执行 configure**，构建期的发现命令才会重新生成。

## 4. 影响面

| 环境 | CMake | 修复前 | 修复后 |
| --- | --- | --- | --- |
| Windows 本机 / GitHub windows-2022 | 4.0 / 3.31 | 正常（用 `${CMAKE_ROOT}`） | 正常 |
| GitHub ubuntu-22.04 runner | 3.31（runner 自带） | 正常 | 正常 |
| Ubuntu 24.04 自带 / WSL 默认 | 3.28.3 | V12 用例静默丢失 + 1 条 Not Run | 正常 |

即：CI 不受影响，受影响的是 CMake ≤ 3.28 的本地/发行版环境。

## 5. 验证

### 5.1 Linux（WSL Ubuntu-24.04，GCC 13.3，CMake 3.28.3，Ninja，Boost 1.86）

配置（`~/libxcp-full`）：

```bash
cmake -B ~/libxcp-full -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DLIBXCP_BUILD_TESTS=ON -DLIBXCP_BUILD_TOOLS=ON -DLIBXCP_BUILD_EXAMPLES=ON \
  -DLIBXCP_ENABLE_LOG=ON \
  -DFETCHCONTENT_SOURCE_DIR_FTXUI=<Windows 构建目录>/_deps/ftxui-src
```

- 编译：408 个目标全部成功（exit 0）。
- `build.ninja` 中 16 条发现命令全部带脚本路径，`-P ""` 归零。
- 串行全量测试（与 CI 同参）：

```bash
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
export LIBXCP_TEST_WATCHDOG_SECONDS=60
ctest --output-on-failure --timeout 90 \
  -E "XcpMasterRenderBenchTest|a2l_ui_perf_gen|a2l_perf_gen|ParseLargeFileUnderBudget"
```

结果：**1167 条用例，100% passed，0 failed，347.63 s**；
按设计跳过 2 条（`TcpStreamDecoderFinish.HeaderOnlyIsTruncated` Disabled、
`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8` Skipped）。
`XcpliteV12InteropTest` 的 7 条用例（含 `ReadsMeasurementThroughA2lAddress/{Udp,Tcp}`、
`CalibrationWriteIsReadBackAndChangesMeasurementArray/{Udp,Tcp}`、端口分配、A2L 生成、
auto-profile）全部由 ctest 正常执行并通过。

### 5.2 Windows（VS2022 Release，CMake 4.0.2，`cmake-build-vs`）

- 重新 configure（`-DFETCHCONTENT_FULLY_DISCONNECTED=ON`）+ `cmake --build --config Release`：exit 0。
- 串行全量测试（`TEMP=TMP=D:\project\libxcp\.tmp\ci_temp`，`LIBXCP_TEST_WATCHDOG_SECONDS=60`，
  同一 `-E` 排除列表）：
  结果：**1168 条用例，100% passed，0 failed，283.83 s**；
按设计跳过 5 条：`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`(Disabled)、
`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`(Skipped)，
以及 `SnapshotTest.StagesReadableCopyOfSource` / `CreatesDistinctFilesPerCall` /
`DeletesSnapshotWhenGuardReleased` 三条成功路径（Windows 本机项目树带沙箱注入的
非可信写 ACE，`FindPrivateBase()` 找不到"仅本人可写"目录；同一用例在 Linux 上真跑并通过）。
- `XcpliteV12InteropTest` 在 Windows 上仍为 7/7（本修复不改变其行为）。

## 6. 改动文件

| 文件 | 改动 |
| --- | --- |
| `examples/xcp_slave_demo_v12/CMakeLists.txt` | +8 行：`include(GoogleTest)` 及中文注释说明 |

## 7. 未覆盖项

- `thirdparty/a2llib/test/CMakeLists.txt:40` 也自行 `include(GoogleTest)`，
  该子工程独立配置、不参与主工程测试，无需改动。
- 本修复只解决"发现脚本路径取不到"，不改变任何用例语义与超时设置。