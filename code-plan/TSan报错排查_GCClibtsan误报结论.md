# TSan 报错排查结论：GCC 11 的 libtsan 误报（已用 shim 规避）

> 结论先行：CI 上 `sanitize-thread` 的 153 条「数据竞争」**不是代码问题**，是 **GCC 11 自带的 ThreadSanitizer 不拦截 `pthread_cond_clockwait`** 造成的环境性误报。
> 约束是必须使用 GCC 11，因此不升级编译器，改为在**测试可执行文件**里注入一个转发 shim（把 `clockwait` 落到已被 TSan 拦截的 `cond_timedwait` 上）。实测由 `exit=66 / 153 条` 变为 **`exit=0 / 0 条`，122 个用例全过**。
> 记录时间：2026-11-22

## 1. 现象

PR #25（`e0d67d8`）的 `sanitize-thread` job 在「Test（并发相关用例）」步骤失败：

```
Process completed with exit code 66
注解：ThreadSanitizer 检测到数据竞争
```

退出码 66 是 TSan 报告问题时的默认退出码。**122 个用例本身全部 PASSED**，失败只来自 TSan 报告。

## 2. 复现（WSL2 Ubuntu 22.04 + GCC 11.4）

```bash
cmake -B cmake-build-linux-tsan -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DLIBXCP_SANITIZER=thread -DLIBXCP_BUILD_TESTS=ON \
  -DLIBXCP_BUILD_TOOLS=OFF -DLIBXCP_BUILD_EXAMPLES=OFF -DLIBXCP_ENABLE_LOG=OFF \
  -DBoost_DIR=/usr/local/boost/lib/cmake/Boost-1.86.0 \
  -DFETCHCONTENT_SOURCE_DIR_FMT=<已解压的 fmt-11.2.0 源码>
cmake --build cmake-build-linux-tsan -j "$(nproc)"

cd cmake-build-linux-tsan
TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=7" \
  ./tests/libxcp_tests \
  "--gtest_filter=UdpTransportLifecycle.*:TcpEndToEndTest.*:TcpLoopbackTest.*:*Transport*:*EventPoller*:*Socket*"
```

修复前实测：`122 tests from 27 test suites ran (11122 ms)` / `[  PASSED  ] 122 tests.` / **`exit=66`** / **153 条** `WARNING: ThreadSanitizer: data race`。

## 3. 报告本身的矛盾点（定位的起点）

以 `TcpEndToEndTest.ReceiveDecodesFrameSentByRawPeer` 为例：

```
Write of size 8 ... by thread T10 (mutexes: write M1242):
  #2 OnPacketReceived  tests/tcp_transport_test.cpp:70       ← 在 lock_guard 内
Previous read of size 8 ... by main thread (mutexes: write M1242):
  #1 operator()        tests/tcp_transport_test.cpp:97       ← wait_for 谓词，在锁内
```

**两侧都持有同一把互斥量 `M1242`**，按互斥量语义这两次访问必然有先后，不该被报成竞争。库侧 `CommandExecutor::WaitForResponse`（`src/command_executor.cpp:497`）同样是「两侧都在 `m_mutex_` 锁内」。

`RecordingListener`（`tests/udp_transport_test.cpp:53-128`）逐行核对过：`m_packets_`/`m_warnings_`/`m_closed_` 的读写全在同一把 `m_mutex_` 内，没有漏锁。

## 4. 最小复现探针（决定性证据）

40 行的教科书式程序：一个线程在 `lock_guard` 内 `emplace_back` 后 `notify_all()`，主线程在 `cv.wait_for` 谓词里读 `size()`。GCC 11 + TSan 下：

```
WARNING: ThreadSanitizer: double lock of a mutex
  #4 Listener::Push()  tsan_probe.cpp:19        ← 显然没有重复加锁
WARNING: ThreadSanitizer: data race ...
SUMMARY: ThreadSanitizer: reported 3 warnings
exit=66
```

对照实验矩阵：

| 工具链 | 探针 | 结果 |
| --- | --- | --- |
| GCC 11.4 / `libtsan.so.0`，`-O2` | mutex + `condition_variable` | ❌ 3 条误报，`exit=66` |
| GCC 11.4 / `libtsan.so.0`，`-O0` | mutex + `condition_variable` | ❌ 同样 3 条 |
| GCC 11.4 / `libtsan.so.0` | **只有 mutex**（无条件变量） | ✅ 干净 |
| GCC 13.3 / `libtsan.so.2` | mutex + `condition_variable` | ✅ 干净（仅作对照，本项目不采用） |

进一步用 **纯 pthread** 探针把变量收敛到「等待函数」这一个维度（同一份代码，只换等待调用）：

| 等待方式 | 结果 |
| --- | --- |
| `pthread_cond_timedwait`（CLOCK_REALTIME 绝对时刻） | ✅ 0 条告警 |
| `pthread_cond_clockwait`（CLOCK_MONOTONIC 绝对时刻） | ❌ 3 条告警，`exit=66` |

**根因确认**：libtsan 的拦截器里没有 `pthread_cond_clockwait`。

## 5. 根因

glibc ≥ 2.30 提供 `pthread_cond_clockwait`，libstdc++ 11 在 `_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT` 存在时（本机 `c++config.h:1746` 定义为 1）用它实现 `std::condition_variable::wait_for()/wait_until()`。

GCC 11 的 libtsan 没有拦截该函数，于是「等待期间释放互斥量、醒来后重新获取」这一步没有被建模：

- 等待线程被当作仍持有互斥量 → 写线程的加锁被判为 `double lock of a mutex`；
- 之后的读写被判为 `data race`。

已排除「运行时与编译器版本不匹配」：`gcc 11.4.0` 与 `libtsan0:amd64 11.4.0-1ubuntu1~22.04.3` 同版本。

## 6. CI 历史佐证

同一份 `linux.yml`、同一组 122 个用例的 filter，只差编译器版本：

| 提交 | sanitize-thread 编译器 | Linux workflow |
| --- | --- | --- |
| `fd5b560`（master） | `gcc-13`（PPA） | ✅ success |
| `e0d67d8`（本 PR，改用系统 GCC 11） | GCC 11.4 | ❌ failure |

说明这些并发用例在**能正确建模 clockwait 的 TSan** 下是干净的 —— 缺陷在工具链，不在代码。

## 7. 处置：注入 `pthread_cond_clockwait` 转发 shim（保持 GCC 11）

既然必须用 GCC 11，就不能靠升级编译器绕过。做法：

**新增 `tests/tsan_cond_clockwait_shim.cpp`**
- 在 Linux/glibc 上定义 `pthread_cond_clockwait`，把「以指定时钟为基准的绝对超时」换算成相对剩余时间，再落到 `pthread_cond_timedwait`（该函数 libtsan 有完整拦截，因此释放/重取互斥量被正确建模）；
- 已过期时立即超时、`clock_gettime` 失败时退回原语，语义与 libc 版本一致。

**`tests/CMakeLists.txt` 条件注入**
```cmake
if(LIBXCP_SANITIZER STREQUAL "thread"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
  target_sources(libxcp_tests PRIVATE tsan_cond_clockwait_shim.cpp)
endif()
```
只加进 `libxcp_tests` 这个可执行文件：符号定义在可执行内部，静态链接时库侧（`libxcp` 里的 `CommandExecutor`）与 libstdc++ 内联产生的调用都会绑定到它；普通构建、GCC ≥ 12、Clang 均不注入。

**上一轮的临时手段全部撤销**：`continue-on-error`、降级注解都不再存在，恢复「有竞争即失败」的强校验；`sanitize-thread` 与 `build-linux` 一样使用系统 GCC 11。

## 8. 验证结果（WSL2 Ubuntu 22.04，GCC 11.4）

| 项 | 结果 |
| --- | --- |
| 配置 | `CFG_EXIT=0`，输出 `ThreadSanitizer: 注入 pthread_cond_clockwait 转发 shim（GCC 11 误报规避）` |
| 构建 | `BUILD_EXIT=0`，`error:` 计数 0 |
| 符号 | `nm -C tests/libxcp_tests` 出现 `T pthread_cond_clockwait`（定义在可执行内） |
| 同一组 122 用例 | **`exit=0`、`0` 条 TSan 告警、`[  PASSED  ] 122 tests.`（9685 ms）** |

修复前后对比：`exit=66 / 153 条` → **`exit=0 / 0 条`**。

## 9. 踩过的一个坑（值得记住）

第一版 shim 被静默编译成空翻译单元：文件开头就用 `#if defined(__linux__) && defined(__GLIBC__)` 判断，但 `__GLIBC__` 由 `<features.h>` 定义，条件编译发生时它还没被包含 → 恒为假。
表现是「构建成功但符号仍是 `U`，误报一条没少」。修法是先在 `#if defined(__linux__)` 下包含 `<features.h>`，再判断 `__GLIBC__`；并用 `nm` 断言符号确实是 `T` 而不是 `U`。

## 10. 遗留与注意

1. **本地跑 TSan 必须带这个 shim**（即通过 CMake 构建）；直接手写 g++ 命令编译测试、或单独编译某个探针，GCC 11 下仍会出现该类误报。
2. 若将来基线升到 GCC ≥ 12 或改用 Clang，可删掉 shim 与那段 CMake 条件（届时 TSan 自身能正确建模 clockwait）。
3. 本次**没有修改任何测试或库的并发逻辑** —— 只新增一个测试侧的符号转发文件。
4. 上一轮基于「竞争真实存在」判断写下的 `code-plan/TSan竞争待修清单.md` 已删除，由本文档取代。

## 11. 关联文件

- `.github/workflows/linux.yml`（`sanitize-thread` job 的注释说明）
- `tests/tsan_cond_clockwait_shim.cpp`（新增）
- `tests/CMakeLists.txt`（条件注入）
- 排查探针未入库，结构见本文 §4
