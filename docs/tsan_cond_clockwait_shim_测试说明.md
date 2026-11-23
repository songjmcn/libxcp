# TSan 时钟转发补丁（`pthread_cond_clockwait` shim）测试说明

> 被测文件：`tests/tsan_cond_clockwait_shim.cpp`（补丁）
> 测试文件：`tests/tsan_cond_clockwait_shim_test.cpp`（本说明的说明对象）
> 用例套件：`TsanCondClockwaitShim`（5 条，仅 Linux/glibc 构建注册）
> 生效范围：`LIBXCP_SANITIZER=thread` 且编译器为 GCC < 12

## 1. 这个补丁为什么存在

CI 的 `sanitize-thread` job 在 Ubuntu 上用系统 GCC（11.4）跑 ThreadSanitizer。自从把 CI 从 GCC 13 降到 GCC 11 后，该 job 稳定失败：**122 条用例全部 PASSED，进程退出码却是 66**，日志里 100 多条 `data race` / `double lock of a mutex`，全都在教科书式的 `std::mutex` + `std::condition_variable` 代码上。

根因不在被测代码，而在工具链缺一块：

| 事实 | 证据 |
| --- | --- |
| glibc ≥ 2.30 提供 `pthread_cond_clockwait` | glibc 头文件 `pthread.h` |
| libstdc++ 11 在 `_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT` 存在时用它实现 `wait_until` / `wait_for` | `/usr/include/c++/11/condition_variable` |
| **GCC 11 的 `libtsan` 没有为该函数提供拦截器** | `nm -D libtsan.so` 中无对应符号 |

后果是：`wait_until` 在等待期间**释放互斥量、醒来后重新获取**这一步对 TSan 不可见，于是它把此后所有对互斥量的访问都判成"未经排他保护"。

我用 40 行教科书式 mutex+condvar 探针做过矩阵验证：

| 工具链 / 用法 | 结果 |
| --- | --- |
| GCC 11.4，`-O2` / `-O0`，`condition_variable` | ❌ 3 条误报（含物理上不可能的 `double lock of a mutex`） |
| GCC 11.4，只有 mutex、无 condvar | ✅ 干净 |
| GCC 11.4，纯 pthread `pthread_cond_timedwait` | ✅ 0 条 |
| GCC 11.4，纯 pthread `pthread_cond_clockwait` | ❌ 3 条 |
| GCC 13.3（仅作对照，**不用于 CI**） | ✅ 干净 |

结论：误报完全由 `clockwait` 这一条路径引起。升级 CI 编译器可以绕过，但用户明确要求 **必须留在 GCC 11**，所以改为在测试二进制里补上缺失的那一层。

## 2. 补丁做了什么

在测试可执行文件里定义一个与 libc 同名的 `pthread_cond_clockwait`，它在**运行时优先于 libc 版本被链接**，内部把调用换算后转给 `pthread_cond_timedwait`——后者是 libtsan 一直有拦截器的老接口。

```cpp
int pthread_cond_clockwait(pthread_cond_t*, pthread_mutex_t*, clockid_t clock_id,
                           const timespec* abstime);
```

换算规则（完整实现见 `tests/tsan_cond_clockwait_shim.cpp`）：

1. `abstime == nullptr` → `return EINVAL`（不猜、不吞）；
2. 读取 `clock_id` 的当前时刻失败 → `return EINVAL`；
3. 计算 `delta = *abstime - now(clock_id)`（相对时长，跨时钟可比）；
4. **用 `CLOCK_REALTIME` 的当前时刻 + `delta` 构造新死线**，`tv_nsec` 溢出进位；
5. 转给 `pthread_cond_timedwait(cond, mutex, &new_deadline)`。

第 4 步是这个补丁唯一需要"选边"的地方，理由见下一节。

## 3. 为什么死线必须用 `CLOCK_REALTIME` 构造

`pthread_cond_timedwait` **不接收时钟参数**，它解释 `abstime` 时用的是**条件变量创建时绑定的时钟**（`pthread_condattr_setclock`，缺省 `CLOCK_REALTIME`），与调用方传进来的 `clock_id` 无关。

libstdc++ 11 的行为（`/usr/include/c++/11/condition_variable:72-75` 与 `:200`，已核对）：

- 构造 condvar 时**无条件**用 `CLOCK_MONOTONIC`；
- `wait_until(steady_clock)` 把剩余时长换算成 **`CLOCK_REALTIME` 绝对时刻**再调 `clockwait`。

也就是说，实际调用永远是"`clock_id` = REALTIME，cond 绑定 = MONOTONIC"这个组合。因此**按 `clock_id` 取 now 来构造死线是正确的**。

早期有一版实现改成"用同一个 `clock_id` 取 now"，理由是"库应该保证 cond 绑定时钟与 `clock_id` 一致"——**这个推理是错的**，实测直接推翻：

| shim 实现 | `wait_until(steady_clock)` | `wait_until(system_clock)` |
| --- | --- | --- |
| `CLOCK_REALTIME` 死线（当前实现） | 80 ms 正常 | 80 ms 正常 |
| `clock_id` 死线（已废弃） | **0 ms 立即返回 → 静默假绿** | 80 ms 正常 |
| 无 shim（原生 `clockwait`） | 80 ms 正常 | 80 ms 正常 |

这也是本测试文件存在的关键理由：那种写法不会让测试变红，而是让**所有等待类测试瞬间通过且不报错**。

### 3.1 已知边界（不要"修"它）

若调用方给的死线是 `CLOCK_MONOTONIC` 而 cond 绑定的是 `CLOCK_MONOTONIC`，当前实现会把 REALTIME 死线交给它解释，剩余时间变成两个时钟的秒数差——本机实测约 **1 795 267 540 秒（约 56.9 年）**，表现为**挂起**。

| 写法 | libstdc++ 真实序列 | 原生 MONOTONIC + MONOTONIC |
| --- | --- | --- |
| REALTIME 死线（当前） | ✅ 正确 | ❌ 永久挂起 → CI 超时**显式失败** |
| `clock_id` 死线（已废弃） | ❌ 立即返回 → **静默假绿** | ✅ 正确 |

**为什么接受这个边界**：仓库里没有任何 `clockwait` 调用方使用 MONOTONIC 死线（libstdc++ 全部走 REALTIME；`src/command_executor.cpp:213/543/881` 的 `wait_until(steady_clock)` 也经 libstdc++）。两条路都做不到"全对"，我们选**错的时候吵得响**的那条。

**为什么不做"保险丝告警"**：想 hook `pthread_cond_init` 记录 cond 绑定时钟，链接直接失败——glibc 的真实入口是 `__pthread_cond_init` / `__pthread_cond_destroy`，**均不导出**；`pthread_cond_t` 又是不透明类型，标准 API 没有 `pthread_cond_getclock`。而"判断 `clock_id == CLOCK_MONOTONIC` 就告警"是假保险丝（libstdc++ 传的恰恰是 REALTIME，永不触发）。所以最终选择**把边界写进文档 + 用回归用例钉住正确路径**，不要无用构件。

彻底消除只剩两条路，都不属于"发版热修"范围：`LD_PRELOAD` + `dlsym(RTLD_NEXT)` 维护 cond→clock 映射，或改用已修复的 libtsan。

## 4. 测试用的是什么，没测什么

**测的是工程补丁的正确性**，不是 libxcp 的功能。补丁一旦行为不端，整个 TSan 门禁的结论就是假的，所以这里的断言只锁定对用户可见的两条承诺：

1. **不挂起**——死线到期必须返回，耗时设上限；
2. **不提前返回**——死线未到且无人通知时必须真的阻塞。

**没测什么，以及为什么**：

- 不测"补丁能处理所有时钟组合"。补丁只服务于 libstdc++ 的真实调用序列；测不存在的组合属于验证已知边界，已经写在 §3.1。
- 不测补丁内部的换算公式。曾有过一条 `MonotonicDeadlineWouldBeDeferredByYears`，用与实现相同的公式复算并断言"会推迟十几亿秒"——它**测的是缺陷本身而不是契约**，等于给已知边界刻碑，反而会误导后来者以为挂起是被认可的预期行为，已删除。
- 不在非 TSan 构建里跑。Windows/MSVC 不需要该补丁，该文件在那里编译为空翻译单元。

## 5. 用例清单

| 用例 | 断言 | 失败意味着 |
| --- | --- | --- |
| `SteadyClockWaitActuallyWaits` | 80 ms 死线 → `timeout`，耗时 ∈ [40, 500) ms | **假绿**：补丁把等待变成空转 |
| `SteadyClockWaitWakesOnSignal` | 20 ms 后 `notify_one` → `no_timeout`，耗时 < 80 ms | 补丁吞掉唤醒或根本没阻塞 |
| `SystemClockWaitActuallyWaits` | 80 ms REALTIME 死线 → `timeout`，耗时 ∈ [40, 500) ms | REALTIME 路径同样退化 |
| `ExpiredDeadlineReturnsImmediately` | 已过期死线 → `timeout`，耗时 < 500 ms | 过期死线被当成远期 |
| `WaitForActuallyWaits` | `wait_for(80ms)` → `timeout`，耗时 ∈ [40, 500) ms | `wait_for` 走同一条路径也失效 |

常量：`kWaitMs = 80`（目标等待时长）、`kHangLimitMs = 500`（挂起判定上限）。上界取 500 ms 而非紧贴 80 ms，是为了容忍 CI 上的调度抖动。

实测（TSan 构建，GCC 11.4）：`5 tests from 1 test suite ran. (264 ms total)` / `[  PASSED  ] 5 tests.`，各例耗时 80 / 20 / 80 / 0 / 80 ms。

## 6. 怎么跑

```bash
# 1) 配置 TSan 构建（Linux，GCC 11 即会触发 shim 注入）
cmake -B cmake-build-linux-tsan -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLIBXCP_SANITIZER=thread -DLIBXCP_ENABLE_LOG=OFF

# 配置输出应出现：
#   ThreadSanitizer: 注入 pthread_cond_clockwait 转发 shim（GCC 11 误报规避）

# 2) 构建
cmake --build cmake-build-linux-tsan --target libxcp_tests -j 4

# 3) 只看本补丁的用例
./cmake-build-linux-tsan/tests/libxcp_tests \
  --gtest_filter='TsanCondClockwaitShim.*'
```

**测试必须对着 TSan 构建树跑。** 用例只在 `LIBXCP_SANITIZER=thread` 时注册；对着普通构建树（如 `cmake-build-linux-ci`）跑会得到 `Running 0 tests from 0 test suites`——那**不是通过，是根本没测**。实测对照：

```
$ ./cmake-build-linux-ci/tests/libxcp_tests --gtest_filter='TsanCondClockwaitShim.*'
[==========] Running 0 tests from 0 test suites.     # ci 树：未注册
$ ./cmake-build-linux-tsan/tests/libxcp_tests --gtest_filter='TsanCondClockwaitShim.*'
[==========] 5 tests from 1 test suite ran. (264 ms total)
[  PASSED  ] 5 tests.                                # tsan 树：真正执行
```

**验证补丁真的链接进去了**（否则所有断言都在测 libc，等于没测）：

```bash
nm cmake-build-linux-tsan/tests/libxcp_tests | grep -w pthread_cond_clockwait
# 期望看到 T pthread_cond_clockwait（T = 已定义）
# 若看到 U pthread_cond_clockwait@GLIBC_2.34，说明 shim 没参与链接
```

反过来，非 TSan 构建里必须看不到它：

```bash
grep -c tsan_cond_clockwait_shim cmake-build-linux-ci/build.ninja   # 期望 0
nm cmake-build-linux-ci/tests/libxcp_tests | grep -w pthread_cond_clockwait
# 期望 U pthread_cond_clockwait@GLIBC_2.34（U = 走 libc，未注入 shim）
```

### 6.1 TSan 构建树的已知限制（WSL `/mnt/d`）

若 TSan 构建目录放在 `/mnt/d`（9p 挂载），`cmake --build` 会报 `ninja: no work to do` 而实际不重编。**先 `ninja -C cmake-build-linux-tsan -t clean libxcp_tests` 再构建**，或把 TSan 构建目录放到 ext4（如 `~/build-tsan`）。改代码后务必用 `nm` 复核符号是 `T` 而不是 `U`。

## 7. 注册条件与已知限制

注册条件写在 `tests/CMakeLists.txt`（`include(GoogleTest)` 之前）：

```cmake
if(LIBXCP_SANITIZER STREQUAL "thread"
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12)
  target_sources(libxcp_tests PRIVATE tsan_cond_clockwait_shim.cpp)
  message(STATUS "ThreadSanitizer: 注入 pthread_cond_clockwait 转发 shim（GCC 11 误报规避）")
endif()
```

因此：

- 非 Linux、Clang、GCC ≥ 12、非 TSan 构建：文件编译为空翻译单元，用例不注册；
- **MSVC/Windows 从不覆盖该补丁**——它在 Windows 上不参与任何构建，这与"Windows 不需要它"一致，但也意味着该补丁只在 Linux/glibc 上得到验证；
- glibc < 2.30 没有 `pthread_cond_clockwait`，此时 libstdc++ 走 `timedwait`，无需补丁（`#if defined(__linux__) && defined(__GLIBC__)` 之外的分支为空）。

## 8. 一次踩坑记录（为什么这文件里有 `#include <features.h>`）

补丁文件里必须先用

```cpp
#if defined(__linux__)
#include <features.h>
#endif
```

再判断 `#if defined(__GLIBC__)`。少了这一步，`__GLIBC__` 在预处理期尚未定义，整个补丁会被编译成空翻译单元——**静默失效**，而构建日志一切正常。

## 9. 相关文件

| 文件 | 作用 |
| --- | --- |
| `tests/tsan_cond_clockwait_shim.cpp` | 补丁实现 |
| `tests/tsan_cond_clockwait_shim_test.cpp` | 本说明对应的用例 |
| `tests/CMakeLists.txt` | 注入条件与用例注册 |
| `.github/workflows/linux.yml` | `sanitize-thread` job 的触发方式 |