# 发布门 TSan 专项：CI 并发检查接入记录

## 1. 背景与决策

发布门审查确认了 5 个 P0，其中 B-01（告警回调内二次加锁 → `std::terminate`）、
B-02（发送路径与 `Close()` 竞态）、B-03（并发 `Close()` 双 join）都是典型的数据竞争/
生命周期缺陷，而当时 811 个单测对其零覆盖——靠人工用例无法保证并发路径长期不退化。

**决策（用户 m01055）**：采用方案 A——在 Linux CI 增加 ThreadSanitizer 构建并运行并发相关
用例。放弃的两个备选：

- 方案 B（Windows 上 ASan）：只能抓 use-after-free，抓不到数据竞争，且 MSVC 无 TSan；
- 方案 C（不接入）：成本最低但覆盖依靠手工用例，无法防退化。

## 2. CMake 支持

新增缓存变量 `LIBXCP_SANITIZER`（`CMakeLists.txt:140-171` 附近）：

| 取值 | 效果 |
|---|---|
| 留空（默认） | **不注入任何标志**，构建产物与改动前一致 |
| `thread` | `-fsanitize=thread -fno-omit-frame-pointer -g` |
| `address` | `-fsanitize=address`（同上附加标志） |
| `undefined` | `-fsanitize=undefined`（同上附加标志） |

实现要点：

- 编译与链接**都注入**——sanitizer 运行时库由链接阶段决定，只加编译标志会在链接时报未定义符号；
- 在顶层目录调用 `add_compile_options`/`add_link_options`，通过目录属性继承给 `libxcp`、
  `libxcp_tests`、`XcpliteIntegration` 等所有目标；
- **MSVC 命中时直接 `FATAL_ERROR`**：MSVC 不支持对应命令行开关，若静默忽略会产出"看似带
  检查、实际没有"的构建，比直接失败更危险。

## 3. CI job

在 `.github/workflows/linux.yml` 新增 `sanitize-thread` job（与 `build-linux` 并行）：

- 工具链：GCC 13 + Ninja（与主 job 一致），`boost 1.86.0` 复用同一安装步骤；
- 构建类型 **RelWithDebInfo**：TSan 报告需要可读行号，Release 的 `-O3` 会让栈帧失真；
- `LIBXCP_BUILD_TOOLS=OFF`、`LIBXCP_BUILD_EXAMPLES=OFF`：该 job 只做并发检测，不重复验证
  工具与示例；
- `LIBXCP_ENABLE_LOG=OFF`：glog 自身在 TSan 下有独立竞争报告，会掩盖被测代码的真实问题；
- 用例范围 `--gtest_filter=UdpTransportLifecycle.*:TcpEndToEndTest.*:*Transport*:*EventPoller*:*Socket*`：
  全量用例在 TSan 下会被第三方库的既有竞争噪声淹没，先收敛到 transport / 事件轮询 / socket
  抽象层；
- `TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=7"`：一次运行内收集
  全部竞争，而不是遇到第一条就退出。

**为什么只加 Linux**：MSVC 没有 ThreadSanitizer；macOS 的 clang TSan 可用但 runner 更慢、
且本项目 macOS 侧已有独立 workflow，后续需要时可按同一 job 复制过去（CMake 选项已平台无关）。

## 4. 验证

| 项 | 结果 |
|---|---|
| `python yaml.safe_load(linux.yml)` | YAML OK |
| MSVC 下 `-DLIBXCP_SANITIZER=thread` | 按预期 `FATAL_ERROR`：`LIBXCP_SANITIZER=thread 需要 GCC 或 Clang 工具链；MSVC 不支持 ThreadSanitizer 等同类命令行开关` |
| MSVC 下默认（留空）配置 | 配置成功，产物不注入任何标志 |
| TSan 实跑（2026-11-02，WSL2 Ubuntu 24.04 + GCC 13.3 + Boost 1.86，Boost 由 apt 的 1.83 改为源码安装的 1.86） | 全量构建 229/229 成功；并发 filter 下 110 条用例**全部通过**。首跑（Boost 1.83 + 手工 `boost_process` shim）报 6 条数据竞争，全部位于 `tests/` 测试代码（`src/` 零命中）；按 §4.1 修复后，在 Boost 1.86 环境下复跑为 **0 警告、EXIT=0** |

### 4.1 TSan 首跑实测命中（已定位，均在测试代码）

首跑 6 条报告全部落在 `tests/`，`src/` 零命中，但**根因不是"测试辅助类缺少互斥"**，而是两类问题：

| 位置 | 读写双方 | 真因 | 报告数 |
|---|---|---|---|
| `tests/tcp_transport_test.cpp` 多处用例体 | 写：接收线程 `OnTransportClosed`（`tests/tcp_transport_test.cpp:78`，**持锁**）；读：主线程 `~RecordingListener`（`:64`） | **对象生命周期错配**：`TcpTransport transport(cfg);` 声明在前、`RecordingListener listener;` 声明在后，析构顺序相反 ⇒ listener 先销毁，transport 在其析构时才 `Close()` 并 join 接收线程，此时回调落在已析构的 listener 上（use-after-free）。`RecordingListener` 的读侧其实**已经加锁**（`closed()`/`close_reason()` 均持 `m_mutex_`） | W1–W3 |
| `tests/recovery_test.cpp` `ScriptedSlave` | 写：mock transport 回调线程 `operator()`（`++m_counts_[cmd]`、`m_order_.push_back`）；读：主线程 `Count()` / `Order()` | 测试替身**完全无锁**，且 `Order()` 返回成员引用，拷贝过程与写入并发 | W4–W6 |

修复（只动测试代码，产品代码零改动）：

1. `tests/tcp_transport_test.cpp`：把 `RecordingListener listener;` 统一**前置**到 `TcpTransport transport(cfg);` 之前（脚本批量对调 26 处 + 手工 3 处），并在前 3 处加中文注释说明"析构顺序与声明顺序相反"的约束。
2. `tests/recovery_test.cpp`：`ScriptedSlave` 增加 `mutable std::mutex m_mutex_`，`SetResponse`/`SetOnceResponse`/`DropNthCall`/`DropFirstCalls`/`SetBusyFirstCalls`/`Count`/`ResetCounts`/`operator()` 全部在函数体首行加锁；`Order()` 由返回 `const std::vector<CommandCode>&` 改为**按值返回** `std::vector<CommandCode>`，避免调用方在其它线程仍追加记录时读到半拷贝。

复跑结果（2026-11-02，同一 WSL2 环境、同一 filter）：

| 项 | 结果 |
|---|---|
| `cmake --build ~/libxcp-tsan186 -j 22` | 全量构建 229/229 成功（Boost 1.86，原生 `boost_process` 组件，无需 shim） |
| TSan 并发 filter | **110 tests PASSED，`ThreadSanitizer: reported 0 warnings`，EXIT=0** |
| Windows `libxcp_tests.exe` | 818 passed，1 skipped（既有 `Ag1_Cto8`），1 既有 DISABLED |
| Windows `XcpliteIntegration.exe` | 61 passed |
| Windows `XcpliteV12InteropTest.exe` | 7 passed |

### 4.2 Boost 1.86 环境复测与不限 filter 的全量 TSan

- **环境变化**：WSL2 Ubuntu 24.04 的 Boost 由 apt 提供的 1.83 改为源码安装的 1.86（`/usr/local`，含官方 `boost_process` 组件）。原先为 1.83 手工搭的 `~/boost-shim` 与旧构建目录 `~/libxcp-tsan` 已删除，配置不再需要 `-Dboost_process_DIR`。
- 解析结果：`Boost_DIR=/usr/local/lib/cmake/Boost-1.86.0`、`boost_process_DIR=/usr/local/lib/cmake/boost_process-1.86.0`；全量构建 **229/229** 成功。
- 并发 filter 复跑：**110 tests，0 警告，EXIT=0**。
- 额外做了**不限 filter 的全量 TSan 运行**（CI job 不跑这一段）：818 tests，817 passed + 1 skipped，暴露出 8 条 filter 未覆盖的竞争，全部位于 `tests/tcp_test_slave.cpp`：
  - 主线程 `Stop()` 读 `m_socket_->connection`（`tests/tcp_test_slave.cpp:231`）与接收线程 `CloseConnection()` 写同一字段（`:315`）并发；
  - 修复：该字段改为 `std::atomic<NativeSocket>`；`CloseConnection()` 改用 `exchange(kInvalidSocket)`，保证并发关闭时只有一个线程真正 shutdown/close；`Stop()` 用局部快照，避免两次读取落到不同句柄。
  - 修复后全量 TSan：**818 tests，817 passed，0 警告，EXIT=0**。
- CI filter 追加 `TcpLoopbackTest.*`（覆盖 `tcp_test_slave.cpp` 的用例），过滤集由 110 增至 **117 tests，0 警告，EXIT=0**。

## 6. 首跑风险与预案

- 首次 TSan 运行可能报出此前未发现的竞争（含批次 1 未覆盖的 B-02 残余路径、`EventPoller`
  重建向量等），**这是该 job 的价值所在，不应为了让它变绿而放宽**；
- 若第三方代码（glog / gtest / Boost）产生噪声，先确认报告栈顶是否落在 `src/`；只有落在
  第三方库且确认与 libxcp 无关时，才增加 `TSAN_OPTIONS=suppressions=` 文件，并在该文件中
  写明被抑制的具体栈帧；
- 该 job **不设为发布硬门**（避免首次接入即阻塞合并），但一旦连续两轮为绿，应转为必需检查。

## 7. 改动文件

| 文件 | 说明 |
|---|---|
| `CMakeLists.txt` | 新增 `LIBXCP_SANITIZER` 选项、MSVC 保护、编译/链接标志注入 |
| `.github/workflows/linux.yml` | 新增 `sanitize-thread` job |
| `tests/tcp_transport_test.cpp` | 29 处 `RecordingListener` 声明前置到 `TcpTransport` 之前（修 TSan W1–W3 生命周期错配） |
| `tests/recovery_test.cpp` | `ScriptedSlave` 全方法加锁、`Order()` 改按值返回（修 TSan W4–W6） |
| `tests/tcp_test_slave.cpp` | 连接句柄改 `std::atomic<NativeSocket>` + `exchange` 关闭（修全量 TSan 8 条） |
