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

1. **Linux 侧已打通**：Linux 真正的硬阻塞不是 CMake，而是 Boost——系统 Boost 1.74 只提供
   `boost_headers` / `boost_system` 的 CMake config，缺 `locale`/`filesystem`/`process`。
   自建 Boost 1.86 静态库并加 `-fPIC` 后，GCC 11.4 + CMake 3.22.1 可完整构建并跑通全量测试，
   细节见 `code-plan/Linux_WSL_构建验证_实施记录.md`。
2. **GCC 13 不是 A2L 栈的需求**：`<format>` 只出现在 `include/libxcp/log.hpp`，
   且被 `#if LIBXCP_ENABLE_LOG` 包住；A2L 栈自己的 `<format>`/ranges 问题已由
   a2l-sdk 的源替换与 PCH 处理。即：日志关闭时 GCC 11 够用，开日志才需要 GCC 13。
3. **`DOWNLOAD_EXTRACT_TIMESTAMP` 在 CMake 3.22 上不是 warning，而是硬错误**（实测推翻早前判断）：
   ExternalProject 直到 3.24 才认识该关键字，3.22 会把它并入 `URL_HASH`，报
   `URL_HASH is set to SHA256=…;DOWNLOAD_EXTRACT_TIMESTAMP;TRUE but must be ALGO=value`。
   处置见 §6.1。CMP0135 NEW 只能固定「解压时间戳取归档时间」的语义，不能补上这个关键字。

### 6.1 真实 CMake 3.22.1 的适配（本次补做）

用 Ubuntu 22.04 自带的 `/usr/bin/cmake`（3.22.1）实测，暴露两处必须在 3.22 上才能发现的阻塞：

| 位置 | 问题 | 处置 |
| --- | --- | --- |
| `CMakeLists.txt` 顶层 | 无（用于放置公共变量） | 新增 `LIBXCP_DOWNLOAD_EXTRACT_TIMESTAMP_ARGS`：`CMAKE_VERSION >= 3.24` 时为 `DOWNLOAD_EXTRACT_TIMESTAMP TRUE`，否则为空串 |
| fmt `FetchContent_Declare` | 3.22 下把关键字并入 `URL_HASH` → 配置中止 | 改用上述变量传参 |
| glog `FetchContent_Declare` | 同上（`URL_HASH is set to SHA256=c17d85c0…;DOWNLOAD_EXTRACT_TIMESTAMP;TRUE`） | 同上 |

实测结果（Ubuntu 22.04 / GCC 11.4 / CMake 3.22.1 / Ninja / LOG=ON / tools+examples+tests）：

```
CFG_EXIT=0  BUILD_EXIT=0  ERRORS=0      # 433/433 目标
-- libxcp: 当前工具链缺少 std::format，回退到 fmt
-- {fmt} version: 11.2.0
```

即：**CMake 3.22 现在可以完整构建**，与 CI 的 `apt install cmake` 版本一致。

4. **WSL 上的 `file RENAME` 失败不是代码问题**：`/mnt/d` 是 9p 挂载，CMake 3.22 解压 FetchContent
   包后对目录做 rename 会失败（`ex-fmt-populate1234/fmt-11.2.0 → fmt-src`）。本地验证时用
   `-DFETCHCONTENT_SOURCE_DIR_{FMT,GLOG,FTXUI}=<已解压源码>` 绕开；CI 在 ext4 上不存在该问题。

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
