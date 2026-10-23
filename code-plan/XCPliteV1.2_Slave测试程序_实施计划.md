# XCPliteV1.2 Slave 测试程序 —— 实施计划

- 状态：**待批准**（批准后才开始编码，遵循 AGENTS.md「设计方案批准前禁止实现」）
- 需求来源：用户指令 m02837 —— 「基于 thirdparty/XCPLiteV1.2 代码作为 slave 端的测试程序，需要支持 TCP/UDP，支持 XCP 的 DAQ 模型+POLL 模式。支持生成 A2L，支持标定。」
- 已裁决（m02908 / m02914 / m02963）：
  - **落点 = A**：新建 `examples/xcp_slave_demo_v12/`，与现有 V2.2 示例并存、互不干扰，面向「演示 + 联调」，不默认进 ctest。
  - **传输形态 = A**：`--transport udp|tcp` 启动时二选一；需同时提供两种则起两个进程（不同端口）。
  - **测试策略 = B**：只写模型层单测（`slave_model` 纯 C++ 部分）；端到端由使用者用 `xcp_master` 手工联调，§7 矩阵即为联调清单。
  - **标定页面 = A**：首期即支持双页（参考页 / 工作页）与 CAL page 切换，不启用 `OPTION_CAL_SEGMENTS_SINGLE_PAGE`。

---

## §0 目标与定位

提供一个**基于 XCPlite V1.2（协议层 PL 0x0102 / 传输层 TL 0x0104）的 XCP 从站测试程序**，作为 libxcp Master 的对手端，用于验证 libxcp 在**旧版本协议**上的互操作能力：

| 能力 | 说明 |
| --- | --- |
| 传输 | UDP 与 TCP 各自可运行（以太网传输层 TL 1.4） |
| DAQ | 动态 DAQ：Master 在运行时分配 ODT、写 DAQ 条目、按事件触发；首 ODT 带 32 位时间戳 |
| POLL | 通过短上传（SHORT_UPLOAD）轮询同一批测量量，与 DAQ 值一致 |
| A2L | 生成可被 libxcp 桥接层解析的 A2L（事件、DAQ、测量、标定段、传输层端点齐全） |
| 标定 | 标定段参数可被 DOWNLOAD 写入、可读回、**且被算法真实消费**（输出随之变化），支持 CAL page 切换 |

**核心价值**：仓库现有 `examples/xcp_slave_demo` 与 `tests/xcp_test_slave` 都链接 **XCPlite V2.2.x**；本程序提供 PL 1.2 的真机对手端，补齐「低版本 ECU 兼容」的验证面。

---

## §1 已核证事实（全部读源码确认，作为设计依据）

### 1.1 两份 XCPlite 副本并存

| 路径 | 版本 | 现状 |
| --- | --- | --- |
| `thirdparty/XCPlite` | 2.2.2（`CMakeLists.txt:4`） | 已作为子项目构建，现有示例/测试全部依赖它 |
| `thirdparty/XCPliteV1.2` | 1.x 线（PL 0x0102 / TL 0x0104） | 未跟踪参考副本，**当前不参与构建** |

- PL：`thirdparty/XCPliteV1.2/src/xcp_cfg.h:28`
- TL：`thirdparty/XCPliteV1.2/src/xcptl_cfg.h:24`

### 1.2 V1.2 与 V2.2 的 API 差异（决定不能复用现有代码）

| 项 | V1.2 | V2.2（现有代码依赖） |
| --- | --- | --- |
| `XcpInit` | `void XcpInit(const char *name, const char *epk, bool activate)`（`inc/xcplib.h:498`） | 第三参为 mode 枚举（`XCP_MODE_LOCAL`） |
| A2L 文件名查询 | **无** `A2lGetFilename`（全树 grep 无命中） | 有 |
| `XcpEthServerInit` | `bool XcpEthServerInit(const uint8_t *address, uint16_t port, bool use_tcp, uint32_t measurement_queue_size)`（`inc/xcplib.h:36`） | 同名同参 |
| `A2lInit` | `A2lInit(addr, port, useTCP, mode)` | 一致 |
| 标定段开关 | 默认**关闭**（见 1.5） | 默认开启 |

因此**不能改造现有 slave**（无法通过编译期开关切换，且同名 C 符号会冲突），必须独立目录 + 独立静态库。

### 1.3 传输单例约束（决定「二选一」）

`thirdparty/XCPliteV1.2/src/xcpEthServer.c:76-121`：

```c
if (gXcpServer.isInit) { DBG_PRINT_WARNING("XCP server already running!\n"); return false; }
XcpEthTlInit(addr, port, useTCP, true /*blocking rx*/, gXcpServer.TransmitQueue);
```

服务端为全局单例，`useTCP` 在初始化时定死，同进程无法再起第二种传输。

### 1.4 A2L 生成规则与三个必须处理的坑

| 事实 | 出处 | 影响 |
| --- | --- | --- |
| 文件名 `%s.a2l`（WRITE_ALWAYS）/`%s_%s.a2l`（WRITE_ONCE，带 EPK） | `src/a2l.c:1712-1718` | 无 `A2lGetFilename`，须自行按同名规则定位文件 |
| 无条件输出 `/include "XCP_104.aml"` | `src/a2l.c:486` | V1.2 不附带该文件，须在工作目录生成 AML 空壳 |
| `TRANSPORT_LAYER` 段仅在 `addr != NULL && addr[0] != 0` 时写出 | `src/a2l.c:628` | 必须绑定具体地址（`127.0.0.1`），**不能绑 `0.0.0.0`**，否则 A2L 无端点 |
| 输出写入**当前工作目录** | 无路径参数 | `--output-dir` 需先切换工作目录（或用 `current_path`） |

### 1.5 标定段默认关闭（**本计划的关键发现**）

`thirdparty/XCPliteV1.2/src/main_cfg.h` 中：

```c
// Enable calibration segment management
// #define OPTION_CAL_SEGMENTS          <- 注释状态
// #define OPTION_CAL_SEGMENT_COUNT 8   <- 注释状态
```

而 `src/xcp_cfg.h:40-45` 依赖它：

```c
#ifdef OPTION_CAL_SEGMENTS
#define XCP_ENABLE_CALSEG_LIST
#if OPTION_CAL_SEGMENT_COUNT > 0
#define XCP_MAX_CALSEG_COUNT OPTION_CAL_SEGMENT_COUNT
#endif
#endif
```

未开启时 `XCP_ENABLE_CALSEG_LIST` 未定义，标定段列表 / 段寻址能力缺失；且 `src/xcp_cfg.h:156-157` 明确 `#error "XCP_ENABLE_SEG_ADDRESSING requires XCP_ENABLE_CALSEG_LIST"`。**不开启就没有真正的标定段支持。**

对比：V2.2 的 `thirdparty/XCPlite/src/xcplib_cfg.h:85` 是 `#define OPTION_CAL_SEGMENTS`（默认开启），这正是现有示例能直接标定的原因。

**解决方案（不改三方库）**：`#include "main_cfg.h"` 是引号形式，编译器优先搜索**包含者所在目录**，故无法用自带头文件覆盖 `src/main_cfg.h`。但可用**编译定义**注入（`-D` 先于任何头文件生效，且 `src/main_cfg.h` 中同名行为注释，不会重定义冲突）：

```cmake
target_compile_definitions(xcplite_v12 PRIVATE
        OPTION_CAL_SEGMENTS
        OPTION_CAL_SEGMENT_COUNT=8)
```

- 首期**不启用** `OPTION_CAL_PERSISTENCE`（持久化 .BIN），保证每次启动状态确定；如后续需要掉电保持再加。
- 首期**不启用** `OPTION_CAL_SEGMENTS_ABS`（保持段相对寻址 + 地址扩展，与 V2.2 示例的注册方式一致）。

### 1.6 DAQ 能力

- A2L 声明 `DYNAMIC 0 <max> 0 OPTIMISATION_TYPE_DEFAULT ADDRESS_EXTENSION_FREE IDENTIFICATION_FIELD_TYPE_RELATIVE_BYTE GRANULARITY_ODT_ENTRY_SIZE_DAQ_BYTE 0xF8 OVERLOAD_INDICATION_PID` + `PRESCALER_SUPPORTED` + `TIMESTAMP_SUPPORTED 0x1 SIZE_DWORD`（`src/a2l.c:236-243`）。
- `XCP_MAX_DAQ_COUNT 1024`（`src/xcp_cfg.h:298`），`XCP_ENABLE_DAQ_PRESCALER`（`:313`），`OPTION_DAQ_EVENT_COUNT 256`、`OPTION_DAQ_MEM_SIZE 32 KiB`（`src/main_cfg.h`）。
- 时间戳：首 ODT 恒带 32 位（`src/xcpLite.c:1496` `#define ODT_TIMESTAMP_SIZE 4`，`:2015` 「First ODT always has a 32 bit timestamp」）。
- **PL 0x0102 边界**：无 `GET_DAQ_CLOCK_MULTICAST`（1.3+）、无 `PACKED_MODE`/`START_STOP_SYNCH` 的 1.4 语义；libxcp Master 在 PL 1.2 下必须按能力集降级（B-T1 已建立 `CapabilitySet`，本程序正是它的真机检验面）。

### 1.7 CMake 目标冲突

`thirdparty/XCPliteV1.2/CMakeLists.txt:113/120`：

```cmake
set(xcplib_SOURCES src/xcpAppl.c src/xcpLite.c src/xcpEthServer.c src/xcpEthTl.c
    src/xcpQueue32.c src/xcpQueue64.c src/a2l.c src/persistence.c src/platform.c src/xcpLog.c)
add_library(xcplib ${xcplib_SOURCES})
```

目标名 `xcplib` 与 V2.2 完全同名 ⇒ **禁止 `add_subdirectory(thirdparty/XCPliteV1.2)`**，须自建静态库目标 `xcplite_v12`，且该库只能链接到本程序（同一可执行文件内不能同时链接两份，C 符号重名）。

### 1.8 可复用的成熟经验（避免重复踩坑）

`examples/xcp_slave_demo/`（932 + 147 + 293 行）已解决：AML 空壳生成、`--generate-a2l` 模式、输出目录与越界校验、A2L 定稿（`A2lFinalize`）、通过 `libxcp::a2lbridge` 自校验 A2L、信号处理与优雅退出。本程序**沿用其结构**，只替换 V1.2 的 API 调用。

---

## §2 交付物

### 2.1 目录结构

```
examples/xcp_slave_demo_v12/
├── CMakeLists.txt        # 自建 xcplite_v12 静态库 + 程序目标 + 配置注入
├── main.cpp              # CLI、生命周期、事件线程、信号处理、A2L 生成/退出
├── slave_model.hpp/.cpp  # 测量与标定变量、确定性算法（不依赖 XCP 头）
├── slave_registry.hpp/.cpp # A2L/DAQ 注册单一来源（事件、测量、标定段、typedef）
└── README.md             # 用法、两种传输、A2L 生成、与 V2.2 示例的区别
```

### 2.2 文件职责

| 文件 | 职责 | 关键约束 |
| --- | --- | --- |
| `main.cpp` | 参数解析、`XcpInit`/`A2lInit`/`XcpEthServerInit` 顺序、事件线程、信号、A2L 生成模式 | 必须**先 A2L 注册 → 再 Server Init**；绑 `127.0.0.1` |
| `slave_model` | 波形/计数器/结构体/标定参数与算法 | 纯 C++，可单测；标定参数**必须**参与计算 |
| `slave_registry` | 单一来源地向 A2L 与 DAQ 注册变量 | 注册顺序决定 A2L 顺序，需稳定 |
| `CMakeLists.txt` | 构建 V1.2 静态库与本程序 | 不改 `thirdparty/XCPliteV1.2` 任何文件 |

---

## §3 构建方案

### 3.1 静态库目标

```cmake
add_library(xcplite_v12 STATIC ${XCPliteV12_SOURCES})   # 显式源列表，10 个 .c
target_include_directories(xcplite_v12 PUBLIC
        "${PROJECT_SOURCE_DIR}/thirdparty/XCPliteV1.2/inc"
        "${PROJECT_SOURCE_DIR}/thirdparty/XCPliteV1.2/src")  # main_cfg.h 在 src/
target_link_libraries(xcplite_v12 PUBLIC Threads::Threads)
```

- 源文件用相对路径拼接 `${PROJECT_SOURCE_DIR}/thirdparty/...`（遵循「禁止写死绝对路径」）。
- 目标名 `xcplite_v12` 唯一，不与 V2.2 的 `xcplib` 冲突。
- 库内文件以 C 编译（`.c`，CMake 自动按扩展名分派）；本程序用 C++20。

### 3.2 配置注入

```cmake
target_compile_definitions(xcplite_v12 PRIVATE
        OPTION_CAL_SEGMENTS            # V1.2 默认关闭，标定必需
        OPTION_CAL_SEGMENT_COUNT=8)
```

理由见 §1.5。若日后需要开启持久化，再追加 `OPTION_CAL_PERSISTENCE`。

### 3.3 与构建开关的关系

- 沿用 `LIBXCP_BUILD_EXAMPLES` 门（`examples/` 只在该门为 ON 时进入），在 `examples/CMakeLists.txt` 末尾追加 `add_subdirectory(xcp_slave_demo_v12)`。
- **不依赖 `LIBXCP_BUILD_XCPLITE_SLAVE`**（那是 V2.2 子项目的门）；本目标自带 V1.2 库。
- 不注册 ctest（与落点约定一致），但保留被 `tests/` 按需启动的能力（把可执行文件路径注入测试）。

### 3.4 平台细节

- Windows：`target_link_libraries(... ws2_32)`（与现有示例一致）。
- MSVC：`/utf-8`；C 源需 `/std:c11` 或默认（V1.2 为 C99 风格，避免开 `/WX`）。
- macOS/Linux：`Threads::Threads`，V1.2 `platform.c` 依赖 `pthread`/`clock_gettime`。

---

## §4 运行契约（CLI）

```
xcp_slave_demo_v12 [选项]

  --transport udp|tcp          传输方式（默认 udp）
  --bind <ipv4>                绑定地址（默认 127.0.0.1；禁 0.0.0.0，见 §1.4）
  --port <n>                   端口（默认 5555）
  --queue-size <bytes>         测量队列容量（默认 64 KiB）
  --a2l-mode write-always|write-once   A2L 生成模式（默认 write-always）
  --generate-a2l               仅生成 A2L 与 AML 空壳后退出，不启动服务
  --output-dir <dir>           A2L 输出目录（默认当前目录；内部切换工作目录）
  --overwrite                  允许覆盖已存在的 A2L（write-always 隐含允许）
  --verify-a2l                 生成后用 libxcp::a2lbridge 解析并打印事件/测量/标定摘要
  --log-level <0..4>           XCPlite 日志级别（默认 3）
  --max-runtime-ms <n>         运行指定毫秒后自动退出（0=不限制）
  --ready-token <token>        服务就绪后打印该 token 到 stdout，供父进程探测
  --help                       打印用法
```

退出码：`0` 正常；`2` 参数错误；`3` 初始化失败（XcpInit/A2lInit/Server Init）；`4` A2L 生成或自校验失败。

---

## §5 变量模型与 A2L 注册

### 5.1 事件

| 事件 | 周期 | 触发方式 | 用途 |
| --- | --- | --- | --- |
| `fast` | 10 ms | 专用线程 `DaqTriggerEvent(fast)` | DAQ 高频波形、计数器 |
| `slow` | 100 ms | 专用线程 | 全类型覆盖、结构体、数组、时间戳场景 |

### 5.2 变量

- **fast**：`counter`(uint32)、`sine`(float)、`square`(float)、`saw`(float)、`samples[8]`(float[8])
- **slow**：8 种基础类型各一（u8/u16/u32/u64/s8/s16/s32/float/double 中取 8）、`stats` 结构体、`history[8]`、`event_tick`(uint32)
- **标定段 `params`**：`gain`(float)、`offset`(float)、`limit`(float)、`run`(uint8)、`corrections[8]`(float[8])

**关键设计**：`sine/square/saw` 的输出必须经 `params.gain`/`offset`/`limit` 变换后落到可测量变量，`run` 控制波形是否推进，`corrections[i]` 参与数组元素修正。否则标定只验证「写成功」，验证不到「写进去真的起作用」。

### 5.3 A2L 生成流程

1. `XcpInit(project, epk, true)`（V1.2 第三参为 bool）。
2. 注册：`DaqCreateEvent(fast)` / `DaqCreateEvent(slow)` → `A2lSetAbsoluteAddrMode(fast)` → 测量量 `A2lCreateMeasurement*` → 标定段 `A2lSetSegmentAddrMode` + `A2lCreateParameter` → `A2lTypedefBegin/End`。
3. `A2lInit(bind_addr, port, use_tcp, A2L_MODE_WRITE_ALWAYS | A2L_MODE_AUTO_GROUPS)`。
4. `A2lFinalize()`，随后写入 `XCP_104.aml` 空壳（内容取 `thirdparty/XCPlite/XCP_104.aml`，路径经 CMake 注入，不写死）。
5. 文件名按 §1.4 规则拼 `<project>.a2l`。
6. 可选 `--verify-a2l`：用 `libxcp::a2lbridge` 解析并打印计数，作为 A2L 自校验。

### 5.4 标定段访问模式（双页 + CAL page 切换）

沿用 V2.2 示例已验证的顺序：先 A2L 段相对注册，运行期用 `XcpLockCalSeg` 读取一致快照；**不启用** `OPTION_CAL_SEGMENTS_ABS`，避免地址扩展语义与 libxcp 侧期望不一致。

**双页是库默认行为**：`thirdparty/XCPliteV1.2/src/xcp_cfg.h:222-225` ——

```c
#ifndef OPTION_CAL_SEGMENTS_SINGLE_PAGE
#define XCP_ENABLE_CAL_PAGE
```

只要**不**定义 `OPTION_CAL_SEGMENTS_SINGLE_PAGE`，`XCP_ENABLE_CAL_PAGE` 即生效，库内置 `SET_CAL_PAGE` / `GET_CAL_PAGE` / `COPY_CAL_PAGE` 处理（`src/xcp_cfg.h:234-247`）。因此本程序**无需额外配置**即可支持页切换，实施时只需：

1. 标定段创建时给出**默认值（参考页）**，工作页初始化为同一份副本；
2. `params.run` 等可写参数从**当前活动页**读取（`XcpLockCalSeg` 返回当前页指针），保证页切换后算法立刻用新页数据；
3. 在 §7 第 6 项中验证：写工作页 → 读回新值 → 输出变化 → 切回参考页 → 读回默认值 → 输出复原。

---

## §6 线程与生命周期

```
main
 ├─ 解析参数 → 注册模型/A2L → XcpEthServerInit(addr, port, use_tcp, queue)
 ├─ 线程 fast：sleep 10ms → 更新模型 → DaqTriggerEvent(fast)
 ├─ 线程 slow：sleep 100ms → 更新模型 → DaqTriggerEvent(slow)
 └─ 主线程：等待终止信号 / --max-runtime-ms → 停止线程 → XcpDisconnect/结束
```

- POLL 无需额外线程：XCPlite 的阻塞接收线程直接服务 SHORT_UPLOAD。
- 波形算法只用整数计数与确定性公式（不依赖墙钟），保证 DAQ 与 POLL 在同一时刻可对齐断言。
- 退出顺序：置停止标志 → join 事件线程 → `XcpEthServerShutdown()`（`inc/xcplib.h:39`，内部按序调用 `XcpDisconnect()` 与 `XcpEthTlShutdown()`，见 `src/xcpEthServer.c:125-146`）。

---

## §7 验收矩阵（需求 → 手工联调清单）

> 因测试策略取「只做模型层单测」，本表即**使用者手工联调的步骤清单**，README 逐条给出可复制命令。每项验证结果记入实施记录。

| # | 需求 | 验收动作 | 判定 |
| --- | --- | --- | --- |
| 1 | UDP 传输 | master `--transport udp` 连接 | CONNECT 成功，GET_VERSION 报 **PL 1.2 / TL 1.4** |
| 2 | TCP 传输 | master `--transport tcp --tcp-profile asam-v1.4` | 同上；帧 `[LEN][CTR][Packet]` 解析正确 |
| 3 | DAQ | 动态分配 2 事件 × 4 变量，启动后收 DTO | 每个 DTO 按布局解码后值随波形变化；首 ODT 带 32 位时间戳；与模型同刻值一致 |
| 4 | POLL | SHORT_UPLOAD 读同样的 4 个变量 | 与同期 DAQ 值一致（同刻采样断言） |
| 5 | A2L | `--generate-a2l` + 桥接层解析 | 事件数=2、测量量齐全、标定段可解析、`IF_DATA` 含端点与对齐 |
| 6 | 标定 | DOWNLOAD 写 `gain` → 读回 → 观察输出 → 切页 | 读回值等于写入值；`sine` 幅值按 gain 变化；`SET_CAL_PAGE` 切回参考页后读回默认值、输出复原 |
| 7 | 传输互斥 | 同进程重复 Init | 第二次 Init 失败并给出明确错误（不静默） |

---

## §8 测试策略（已裁决：只做模型层单测）

| 层 | 位置 | 内容 |
| --- | --- | --- |
| 单元 | `tests/xcp_slave_demo_v12_model_test.cpp`（新增） | 模型算法确定性：同一 tick 序列产生同一输出；`gain`/`offset`/`limit` 生效；`run=0` 冻结波形；`corrections[i]` 参与数组修正 |
| 手工 | `README.md` 的 §7 清单 | UDP/TCP 各跑一遍：CONNECT、GET_VERSION(PL1.2/TL1.4)、POLL、DAQ、A2L 解析、标定写读回与页切换 |

接线方式（沿用仓库既有约定，`tests/CMakeLists.txt:144-149` 已有同款做法）：

```cmake
# tests/CMakeLists.txt 的 XcpliteIntegration 段内
if(LIBXCP_BUILD_EXAMPLES)
  target_sources(XcpliteIntegration PRIVATE
          xcp_slave_demo_v12_model_test.cpp
          "${PROJECT_SOURCE_DIR}/examples/xcp_slave_demo_v12/slave_model.cpp")
  target_include_directories(XcpliteIntegration PRIVATE
          "${PROJECT_SOURCE_DIR}/examples/xcp_slave_demo_v12")
endif()
```

- `slave_model` **不得**包含任何 XCPlite / libxcp 头文件（否则单测会被迫链接 `xcplite_v12`，与 `XcpliteIntegration` 已链接的 V2.2 `xcplite` 符号冲突）。此约束写进 `slave_model.hpp` 文件头注释。
- 单测目标只依赖纯 C++ 模型，**不需要** `xcplite_v12`，因此 `LIBXCP_BUILD_EXAMPLES=OFF` 时也不会被请求。
- 不新增 `XcpliteIntegration` 的端到端用例（裁决项），故不注入 `XCP_SLAVE_V12_EXECUTABLE`。
- 本程序不注册为 ctest 目标。

---

## §9 风险与已知边界

| # | 风险/边界 | 说明与对策 |
| --- | --- | --- |
| R1 | PL 1.2 能力缺失 | 无 `GET_DAQ_CLOCK_MULTICAST`、无 `PACKED_MODE`。若 libxcp Master 在 PL1.2 下仍发 1.3+/1.4 命令，本程序将回 `ERR_CMD_SYNTAX`——**这正是要暴露的缺陷**，实施时记录实测命令序列 |
| R2 | 单进程一种传输 | 硬约束（§1.3），需两种则两进程；README 明确写 |
| R3 | 标定段默认关闭 | 已用编译定义解决（§1.5），不改三方库；若注入失效会表现为标定命令返回不支持，Gate 中含该项验证 |
| R4 | A2L 端点缺失 | 绑 `0.0.0.0` 时 A2L 无端点（§1.4）；CLI 拒绝 `0.0.0.0` 并提示 |
| R5 | 仅 macOS 验证 | 与既有约定一致；Windows/Linux 代码路径按 AGENTS 跨平台要求编写，但不在本次验证范围 |
| R6 | V1.2 无 `A2lGetFilename` | 文件名规则按 §1.4 自行拼接；`write-once` 模式文件名含 EPK |
| R7 | 时间戳语义 | 首 ODT 带 4 字节 relative 时间戳；master 侧需按此配置 envelope（既有 TCP 互操作测试已用同一布局） |

---

## §10 分期实施与 Gate

| 阶段 | 交付 | Gate（全部满足才进下一阶段） |
| --- | --- | --- |
| **P1 构建骨架** | `CMakeLists.txt` + 最小 `main.cpp`（Init + Server + 空模型） | 编译通过；UDP 下 master 能 CONNECT 并 GET_VERSION 得到 PL 1.2/TL 1.4；**同时验证标定能力已编入**（`OPTION_CAL_SEGMENTS` 注入生效，§9 R3） |
| **P2 模型与 A2L** | `slave_model` + `slave_registry` + `--generate-a2l` + 模型单测 | A2L 生成成功且被桥接层解析；事件/测量/标定段齐全；模型单测全绿；POLL 可读到全部变量 |
| **P3 DAQ + TCP** | 事件线程 + 动态 DAQ；TCP 传输 | 手工按 §7 第 2、3、4 项验证并在实施记录留存命令与输出（DAQ 与 POLL 值一致） |
| **P4 标定 + 收尾** | 标定段消费链路（双页）+ README + 实施记录 | 手工按 §7 第 6、7 项验证（含页切换）；README 含完整联调步骤；实施记录落盘 |

每阶段结束写一次实施记录（`code-plan/XCPliteV1.2_Slave测试程序_实施记录_P<N>.md` 或合并为一份），含构建命令、测试输出、失败与修正。

---

## §11 明确不做的事

1. 不修改 `thirdparty/XCPliteV1.2` 任何文件（配置经编译定义注入）。
2. 不修改 `thirdparty/XCPlite`（V2.2）现有示例与测试。
3. 不做同进程双传输（§1.3）。
4. 不做标定持久化（`OPTION_CAL_PERSISTENCE`）、不做 A2L 上传/下载（`OPTION_ENABLE_A2L_UPLOAD` 保持库默认）。
5. 不改 libxcp 的 Master 侧代码；若本程序暴露 Master 缺陷，另行登记并按流程修正。
6. 不做 Windows/Linux 实机验证（仅保证代码可移植）。
7. 不写端到端自动化测试（裁决 B），`XcpliteIntegration` 不新增 V1.2 从站用例。

---

## §12 批准与开工

三项裁决均已落定（见文首「已裁决」），本计划**仅待整体批准**。批准后按 §10 从 P1 开始实施，每阶段结束提交实施记录并汇报 Gate 结果。

**状态：已于 2026-10-23 批准并完成 P1–P4 实施**，实施记录见 `code-plan/XCPliteV1.2_Slave测试程序_实施记录.md`；与本文档的偏离汇总见 §13。

---

## §13 实施收口：与计划的偏离及更正

以下 6 条均为实施期以源码/实测证据推翻或补充计划的结论，**以实现与实施记录为准**。

1. **§5.3 第 2 步的地址模式不再适用**：`A2lSetAbsoluteAddrMode` 下 V1.2 的 DAQ 净荷恒为零——`thirdparty/XCPliteV1.2/src/xcp_cfg.h:202` 无条件把 `XCP_ADDR_EXT_APP` 定义为 `0x01`，与 CASDD 的 `XCP_ADDR_EXT_ABS=0x01` 撞车，`src/xcpLite.c:2052` 的采样循环对 `e_ext == XCP_ADDR_EXT_APP` 的条目 `memset(dst, 0, n)`。实际改用**事件相对寻址**（`A2lSetRelativeAddrMode_i` → `addr_ext=3`）配 `XcpEventExt_Var(event, 2, nullptr, base)`，未改第三方源码。
2. **§5.3 第 2 步的扁平 `A2lCreateMeasurement*`** 改为 `A2lTypedefBegin/End` + `A2lCreateTypedefInstance`：要表达结构体测量量（`FastMeasurements`/`SlowMeasurements`/`MeasurementStatistics`）与标定结构体（`CalibrationParameters`），生成的是 `TYPEDEF_STRUCTURE` + `INSTANCE` + `STRUCTURE_COMPONENT`，A2L 中无顶层 `MEASUREMENT`。
3. **新增 A2L 后处理（计划未提）**：V1.2 段相对模式不输出 `READ_WRITE`，而 `tools/xcp_master/symbol_policy.hpp:137` 要求可写符号带该标记，否则主站拒绝标定写入 → 生成后对标定段 `INSTANCE` 行补 `READ_WRITE`（唯一匹配、临时文件 + 回读 + 原子 `rename`，失败退出码 4）。
4. **§5.4 第 3 项页切换方向更正**：V1.2 的页号与常见约定相反，`DEFAULT`（参考/FLASH）为 **1**、`WORKING`（工作/RAM）为 **0**（`src/xcpLite.h:468-469`）；`COPY_CAL_PAGE` 只接受同段 `1 → 0`；写非工作页返回 `CRC_ACCESS_DENIED`（`src/xcpLite.c:798`）。实测「参考页 1.000 / 工作页 2.500 / 复制后 1.000」即为此语义。
5. **§7 第 1、2 项的版本断言无法用 GET_VERSION 完成**：V1.2 的 `case CC_GET_VERSION` 整段位于 `#if XCP_PROTOCOL_LAYER_VERSION >= 0x0104` 内（`src/xcpLite.c:3277-3292`），而它是 `0x0102`，故 `0xC0` 必回 `ERR_CMD_UNKNOWN`（实测 `ProbeVersion()` → `outcome=UnimplementedCommand`）。版本证据改为 A2L `PROTOCOL_LAYER 0x102` + `XCP_ON_*_IP 0x104` 与启动日志 `Version=1.2`。
6. **§8 的接线方式改为独立目标**：原计划把模型单测注入 `XcpliteIntegration`，实际新增独立目标 `XcpSlaveV12ModelTest`（只编译 `slave_model.cpp`），因为 `XcpliteIntegration` 已链接 V2.2 的 `xcplite`，同进程再链 V1.2 会同名符号冲突。

另：§7 第 7 项（同进程重复 Init 失败）因 CLI 二选一而不可达，改为实测双进程并存（UDP + TCP 各自探针全通过）并在 README 记录单实例约束；本次实施仅 macOS 验证（§9 R5）。