# Linux（WSL2 Ubuntu 22.04）构建验证记录

> 目的：在 WSL2 Ubuntu 22.04 上验证工程可编译性。用户已自装 Boost 1.86 到 `/usr/local/boost`。
> 结论先行：**编译期 0 错误**；唯一阻塞是 `libliba2l.so` 链接时 Boost 静态库缺少 `-fPIC`。

## 1. 环境

| 项 | 值 |
| --- | --- |
| 发行版 | Ubuntu 22.04.5 LTS (jammy) |
| 编译器 | g++ 11.4.0（系统默认） |
| CMake | 3.31.6（便携版，装在家目录，避开 apt 的 3.22.1） |
| Ninja | 1.10.1 |
| Boost | 1.86.0，静态库（`libboost_locale.a` / `filesystem.a` / `process.a`，**无 .so**） |

前置改动：`cmake_minimum_required` 已由 3.24 降到 3.20（见 `CMake最低版本降至3.20_实施记录.md`），
否则 CMake 3.22 会在第一行就中止。

## 2. 配置结果：通过

```
cmake -B cmake-build-linux -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBoost_DIR=/usr/local/boost/lib/cmake/Boost-1.86.0
```

关键输出：

```
Boost_FOUND="1" ; Boost_VERSION_STRING="1.86.0"
Boost_LIBRARIES="Boost::locale;Boost::filesystem;Boost::process"
-- liba2l: Boost 库搜索目录 = /usr/local/boost/lib
-- Configuring done (7.3s)
-- Generating done (7.5s)
```

三个组件（locale/filesystem/process）全部解析成功，配置无警告。

## 3. 编译结果：0 错误

`cmake --build cmake-build-linux -j 8` 推进到 165/241 后因链接失败中止；
随后用 `ninja -k 0` 继续，收齐所有编译期问题：

```
== 编译错误（排除链接） ==      （空）
== 编译错误总数 ==              1（仅下游链接错误的 collect2 行）
```

也就是说：**241 个目标里，所有参与编译的源文件在 GCC 11 下全部编译通过**，包括：

- `thirdparty/a2llib` 的 96 个 C++23 编译单元（含 `amlsection.cpp` 的 `std::ranges::find_if`、
  `a2llogstream` 的替换实现）——印证了 a2l-sdk 的 PCH 与源替换策略确实生效；
- `thirdparty/a2l-sdk/src/a2llogstream_boost_locale.cpp`；
- 全部 `tests/` 下的测试源文件。

产物统计：31 个测试可执行文件已成功链接，`tests/libxcp_tests` 可用。

## 4. 唯一阻塞：`libliba2l.so` 链接失败（Boost 非 PIC）

```
FAILED: thirdparty/a2l-sdk/libliba2l.so
/usr/bin/ld: /usr/local/boost/lib/libboost_locale.a(codepage.o):
  relocation R_X86_64_PC32 against symbol
  `_ZTVN5boost6locale4conv21invalid_charset_errorE'
  can not be used when making a shared object; recompile with -fPIC
collect2: error: ld returned 1 exit status
```

### 原因链

1. `thirdparty/a2l-sdk/CMakeLists.txt` 把 `liba2l` 定义为 **SHARED**（`libliba2l.so`），
   作为 a2llib 的共享库边界；
2. 该共享库链接的是 **静态 Boost**（`libboost_locale.a` 等）；
3. 用户的 Boost 编译时未加 `-fPIC`，静态库目标文件里含 `R_X86_64_PC32` 重定位，
   这类重定位不能出现在共享对象中；
4. 工程侧的 `-DCMAKE_POSITION_INDEPENDENT_CODE=ON` 只作用于本工程自己编译的目标，
   **管不到预编译的第三方静态库**。

### 影响范围

- `libliba2l.so` 本身；
- 依赖 A2L 桥接的产物：`tools/xcp_master`（未生成）、依赖 a2lbridge 的集成测试/示例。

核心库 `libxcp.a` 与 31 个测试可执行文件不受影响。

## 5. 运行验证：核心单测 972/973 通过

```
./cmake-build-linux/tests/libxcp_tests --gtest_brief=1
[==========] 973 tests from 165 test suites ran. (41242 ms total)
[  PASSED  ] 972 tests.
[  SKIPPED ] 1 test.
```

在 Linux 上无失败，说明核心库的跨平台实现可用。

## 6. 解决方案（三选一，需决策）

### A. 重编 Boost 静态库并加 `-fPIC`（推荐）

```bash
cd <boost_1_86_0_src>
./b2 --with-locale --with-filesystem --with-process \
     link=static threading=multi variant=release \
     cxxflags=-fPIC cflags=-fPIC \
     --prefix=/usr/local/boost install -j"$(nproc)"
```

优点：保持"静态 Boost"语义，产物无运行时依赖；缺点：需重编一次。

### B. 改为共享版 Boost

```bash
./b2 --with-locale --with-filesystem --with-process \
     link=shared threading=multi variant=release \
     --prefix=/usr/local/boost install -j"$(nproc)"
```

优点：共享库本身即 PIC，无需额外 flag；缺点：运行时需能找到 `.so`
（当前链接行已带 `-Wl,-rpath,/usr/local/boost/lib`，通常可直接跑）。

### C. 工程侧把 `liba2l` 改为 STATIC

改动点在 `thirdparty/a2l-sdk/CMakeLists.txt` 的 `add_library(liba2l SHARED ...)`。
会改变 SDK 的 ABI 边界设计（设计文档明确 liba2l 为 SHARED 边界），属功能决策，**未实施**。

## 7. 复现命令

```bash
cd /mnt/d/project/libxcp

# 配置（Boost 需带 -fPIC 或为 shared，见上）
"$HOME/opt/cmake-pkg/cmake/data/bin/cmake" -B cmake-build-linux -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBoost_DIR=/usr/local/boost/lib/cmake/Boost-1.86.0

# 构建
ninja -C cmake-build-linux -j 8

# 核心单测
./cmake-build-linux/tests/libxcp_tests --gtest_brief=1
```

## 8. 备注

- 构建目录位于 `/mnt/d`（9p 挂载），IO 明显慢于 WSL 原生文件系统；
  完整构建耗时受此影响，建议后续在 `~/build` 下做 Linux 构建。
- 本次验证未修改任何源码；仅使用此前已提交的 CMake 最低版本调整。

## 9. 更新（2026-11-21，后续进展）

- Boost 1.86 静态库已补 `-fPIC` 重编，`libliba2l.so` 的
  `relocation R_X86_64_PC32 ... recompile with -fPIC` 链接失败消失。
- FTXUI 的全量 clone 在 WSL 下过慢（19 分钟仅 5.3 MB / 完整约 105 MB），
  改为复用 Windows 侧已下载源码：
  `-DFETCHCONTENT_SOURCE_DIR_FTXUI=/mnt/d/project/libxcp/cmake-build-release/_deps/ftxui-src`。
- 唯一剩余失败目标 `tests/xcp_master_render_bench_test.cpp`（GCC 11 缺 `<format>`）
  已由 std::format / fmt 兼容层解决，详见
  `code-plan/std_format与fmt兼容层_实施记录.md`。
- 最终结果：**构建 172/172 全部成功**；全量 ctest **1447 项全部通过、0 失败**（703.53 s）。
- 另外验证了日志开启组合：`-DLIBXCP_ENABLE_LOG=ON` 在 GCC 11 + fmt 下 `libxcp` 可完整编译。
