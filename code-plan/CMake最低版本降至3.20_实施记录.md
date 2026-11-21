# CMake 最低版本降至 3.20 实施记录

> 关联问题：WSL2 Ubuntu 22.04 上 `cmake` 为 3.22.1，被工程声明的 `cmake_minimum_required(VERSION 3.24)` 直接挡住，无法进入配置阶段。

## 1. 背景

在 WSL2 Ubuntu 22.04 尝试构建时，第一个报错就是：

```
CMake Error at CMakeLists.txt:24 (cmake_minimum_required):
  CMake 3.24 or higher is required.  You are running version 3.22.1
```

Ubuntu 22.04 官方源里的 CMake 只有 3.22.1（`apt-cache policy cmake` 仅此一个候选），
而仓库 CI 跑在 `ubuntu-22.04` 上却正常，原因是 GitHub runner 镜像自带新版 CMake，
**"22.04 + 默认 apt 工具链"这一组合从未被验证过**。

## 2. 降版本的依据（先证明 3.24 不是硬需求）

### 2.1 未使用 3.21–3.24 期间新增的任何命令

逐条核对后，工程里**没有**用到：

| 3.23/3.24 新增能力 | 是否使用 |
| --- | --- |
| `CMAKE_COMPILE_WARNING_AS_ERROR`（3.24） | 否 |
| `FetchContent` 的 `OVERRIDE_FIND_PACKAGE`（3.24） | 否 |
| `find_package(... GLOBAL)`（3.24） | 否 |
| `install(TARGETS ... FILE_SET)` / `FILE_SET`（3.23） | 否 |
| `block()/endblock()`（3.25） | 否 |

### 2.2 git 历史表明它是基线设定，不是演进结果

- 根 `CMakeLists.txt` 的 `3.24` 出现在**初始提交** `155d8ae`；
- `thirdparty/a2l-sdk/CMakeLists.txt` 的 `3.24` 随 A2L 集成批次 `731769c` 引入，属跟随主工程约定。

即不存在"为了使用某个 3.24 特性才把下限提到 3.24"的演进痕迹。

### 2.3 隔离实验：CMake 3.22 完全能处理本工程的写法

在 `/tmp` 下用系统 CMake 3.22 做最小验证（不触碰仓库）：

| 实验 | 结果 |
| --- | --- |
| `set(CMAKE_CXX_STANDARD 23)` | 配置通过，生成 `-std=gnu++23` |
| `target_compile_features(... cxx_std_23)` | 配置通过，生成 `-std=gnu++23` |
| `FetchContent_Declare(... DOWNLOAD_EXTRACT_TIMESTAMP TRUE)` | 解析通过（仅 warning） |

CMake 3.21 起就前瞻支持 `cxx_std_23`，3.22 已能正确落成 `-std=gnu++23`。

## 3. 改动内容

三处声明统一降到 **3.20**，与 `thirdparty/a2llib` 上游声明（3.20）保持同一口径。

| 文件 | 改动 |
| --- | --- |
| `CMakeLists.txt` | `3.24` → `3.20`，并新增 CMP0128 / CMP0135 显式置 NEW |
| `thirdparty/a2l-sdk/CMakeLists.txt` | `3.24` → `3.20` |
| `thirdparty/a2l-sdk/uchardet-shim/CMakeLists.txt` | `3.24` → `3.20` |

### 为什么还要显式设置 policy

降 `minimum` 后，3.20 之后引入的 policy 会处于"未设置"状态并按 OLD 处理。
其中两条会实际改变行为，因此在根 `CMakeLists.txt` 里显式置 NEW，保持与原先 3.24 语义一致：

```cmake
if(POLICY CMP0128)
  cmake_policy(SET CMP0128 NEW)   # 语言标准 flag 是否附加
endif()
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)   # FetchContent 解压时间戳
endif()
```

用 `if(POLICY ...)` 守卫，保证在真正不支持该 policy 的旧 CMake 上也不会报错。

每处改动都配了中文注释说明降版本原因。

## 4. Windows 验证

环境：Windows 10/11 + MSVC（VS 2022）+ CMake 4.0.2 + Ninja，Release。

### 4.1 配置

```
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
```

结果：`Configuring done (4.3s)` / `Generating done (0.7s)`，退出码 0，**无 policy 相关警告**。

### 4.2 构建

```
cmake --build cmake-build-release -j 4
```

结果：**87/87 全部完成，退出码 0**。仅剩既有告警（glog 的 C4244/C4996、`tests/xcplite_tcp_interop_test.cpp` 的 C4834、
`tests/xcp_slave_demo_integration_test.cpp` 的 C4267），与本次改动无关。

### 4.3 全量回归

```
ctest --test-dir cmake-build-release --output-on-failure
```

结果：**1446 / 1447 通过，0 失败，342.22 s**，与改动前基线一致。
6 项未运行均为既有 Disabled/Skipped：

- `TcpStreamDecoderFinish.HeaderOnlyIsTruncated`（Disabled）
- `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`（Skipped）
- `SnapshotTest.StagesReadableCopyOfSource` / `CreatesDistinctFilesPerCall` / `DeletesSnapshotWhenGuardReleased`（Skipped）
- `XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`（Skipped）

## 5. 验证过程中踩到的环境陷阱（重要，非本改动引起）

首次全量 `ctest` 出现 **31 个失败**，全部集中在写临时文件的用例：

```
create_directories: Access is denied.: "C:\Users\songj\AppData\Local\Temp\libxcp_calibration_access"
无法写入测试 A2L："C:\Users\songj\AppData\Local\Temp\libxcp_calibration_access\....a2l"
```

排查用了一个最小 MSVC 探针程序，得到明确的写边界：

| 位置 | exe 建文件 | exe 建目录 |
| --- | --- | --- |
| `D:/project/libxcp/.tmp`（工作区内） | OK | OK |
| `C:\Users\songj\AppData\Local\Temp` | 失败 | 失败 |
| `D:/tmp` | 失败 | 失败 |

结论：**沙箱只允许 exe 写入会话工作区内**，系统 Temp 对它们是只读的。
把 `TEMP`/`TMP` 指向工作区内目录后，同一批用例立即恢复通过，全量回归随之全绿。

因此这 31 个失败是**环境限制**，与 CMake 最低版本改动无关；但它们说明：
本地跑 `ctest` 时若不带工作区内 `TEMP`，写临时文件的用例会大面积为假失败。

## 6. 已知影响与后续

1. **Linux 侧仍未打通**：本次只解决了 CMake 版本这一层。Ubuntu 22.04 上真正的硬阻塞是
   `find_package(Boost CONFIG REQUIRED COMPONENTS locale filesystem process)`——
   系统 Boost 1.74 只提供 `boost_headers` / `boost_system` 的 CMake config，
   缺 `locale`/`filesystem`/`process`（`process` 在 1.74 里还没有 config 文件）。
2. **GCC 13 不是 A2L 栈的需求**：`<format>` 只出现在 `include/libxcp/log.hpp`，
   且被 `#if LIBXCP_ENABLE_LOG` 包住；A2L 栈自己的 `<format>`/ranges 问题已由
   a2l-sdk 的源替换与 PCH 处理。即：日志关闭时 GCC 11 够用，开日志才需要 GCC 13。
3. `DOWNLOAD_EXTRACT_TIMESTAMP` 在 CMake < 3.24 上只是 warning，已由 CMP0135 NEW 固定语义。

## 7. 复现命令

```bash
# Linux（Ubuntu 22.04，CMake 3.22 + 满足依赖的 Boost）
cmake -B cmake-build-linux -S . -DCMAKE_BUILD_TYPE=Release \
      -DBoost_DIR=<boost-root>/lib/cmake/Boost-<ver>
cmake --build cmake-build-linux -j "$(nproc)"

# Windows（MSVC + Ninja；TEMP 需指向工作区内目录）
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j 4
set "TEMP=%CD%\.tmp\ctest-temp" & set "TMP=%TEMP%"
ctest --test-dir cmake-build-release --output-on-failure
```

## 8. 改动文件

| 文件 | 说明 |
| --- | --- |
| `CMakeLists.txt` | 最低版本 3.20；CMP0128/CMP0135 显式 NEW；中文注释 |
| `thirdparty/a2l-sdk/CMakeLists.txt` | 最低版本 3.20；中文注释 |
| `thirdparty/a2l-sdk/uchardet-shim/CMakeLists.txt` | 最低版本 3.20；中文注释 |

未新增测试用例：本次仅调整构建系统的最低版本声明，不改变任何运行时行为；
验证依靠既有全量测试（1446/1447）覆盖。
