# XCPliteV1.2 Slave 测试程序 —— 实施记录

对应计划：`code-plan/XCPliteV1.2_Slave测试程序_实施计划.md`（2026-10-23 批准）。
本记录覆盖 P1–P4 全部实施、验收矩阵实测结果、实施期发现的问题与对计划的偏离。

---

## §0 交付清单

| 文件 | 性质 | 说明 |
| --- | --- | --- |
| `examples/xcp_slave_demo_v12/CMakeLists.txt` | 新增 | 显式源列表构建 `xcplite_v12` 静态库（禁止 `add_subdirectory`，见 §3.1），注入 `OPTION_CAL_SEGMENTS` 等编译定义 |
| `examples/xcp_slave_demo_v12/main.cpp` | 新增 | CLI、生命周期、A2L 生成/后处理/自校验 |
| `examples/xcp_slave_demo_v12/slave_model.hpp` / `.cpp` | 新增 | 纯 C++ 变量模型（**不含**任何 XCPlite/libxcp 头，可被单测直接编译） |
| `examples/xcp_slave_demo_v12/slave_registry.hpp` / `.cpp` | 新增 | 事件、A2L 注册、标定段、事件触发 |
| `examples/xcp_slave_demo_v12/README.md` | 新增 | 构建、CLI、变量模型、标定页、A2L 后处理、5 条已知限制 |
| `examples/CMakeLists.txt` | 修改 | 末尾 `add_subdirectory(xcp_slave_demo_v12)` |
| `tests/xcp_slave_demo_v12_model_test.cpp` | 新增 | 模型层单测 12 项 |
| `tests/CMakeLists.txt` | 修改 | 新增独立目标 `XcpSlaveV12ModelTest`（**未**按计划 §8 注入 `XcpliteIntegration`，见 §3.6） |

未修改 `thirdparty/XCPliteV1.2` 任何文件（计划 §11 第 1 条）。

---

## §1 构建与运行（实测）

```bash
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_EXAMPLES=ON
cmake --build cmake-build-release --target xcp_slave_demo_v12 -j 4
```

- 目标产物：`cmake-build-release/examples/xcp_slave_demo_v12/xcp_slave_demo_v12`。
- 首次构建曾报 `Missing variable is: CMAKE_C_COMPILE_OBJECT`：根工程 `project(... LANGUAGES CXX)` 未启用 C，
  该子目录内 `enable_language(C)` 解决。
- 标定能力注入验证（`--log-level 3` 启动日志）：
  `Options=(..., CALSEG_LIST, ..., COPY_CAL_PAGE, ...)`，且 `MAX_CTO=248 MAX_DTO=1024`。

---

## §2 验收矩阵实测结果（计划 §7）

联调工具：临时探针 `.tmp/probe/v12probe.cpp`（**未入库**，仅供本次验收；重建命令见 §2.7）。

| # | 需求 | 实测结果 | 判定 |
| --- | --- | --- | --- |
| 1 | UDP 传输 | `CONNECT 成功：PL=1.1 TL=1.1 MAX_CTO=248 MAX_DTO=1024 resource=0x05`；A2L 声明 `PROTOCOL_LAYER 0x102` 与 `XCP_ON_UDP_IP 0x104 5600` | 通过（PL 1.2 / TL 1.4 以 A2L 与启动日志 `Version=1.2` 为准，见 §3.5） |
| 2 | TCP 传输 | 同 UDP，`XCP_ON_TCP_IP 0x104 <port>`；帧 `[LEN][CTR][Packet]` 解析正确，CTR 连续递增 | 通过 |
| 3 | DAQ | 事件 0（`meas_fast_10ms`）动态 DAQ：收到 25–27 帧/次，`counter` 严格递增，首 ODT 带 32 位时间戳，`sine` 随波形变化 | 通过 |
| 4 | POLL | `SHORT_UPLOAD` 读 `fast.counter`（357→368 递增）、`fast.sine`（-1.000 与 DAQ 同刻值一致） | 通过 |
| 5 | A2L | `--generate-a2l --verify-a2l`：事件数 2、`INSTANCE fast/slow` + `STRUCTURE_COMPONENT` 展开、`MEMORY_SEGMENT cal` + `SEGMENT 0` + `PAGE 0/1`、`IF_DATA` 含 `XCP_ON_*` 与 `DAQ`（DYNAMIC/PRESCALER_SUPPORTED/TIMESTAMP_SUPPORTED） | 通过 |
| 6 | 标定 | 写 `cal.gain` 1.000→2.500 读回 2.500，`sine` 幅值按 gain 变化（观测到 2.079/2.452 等）；页切换：参考页读 1.000、工作页读 2.500、`COPY_CAL_PAGE`(1→0) 后工作页读 1.000 | 通过 |
| 7 | 传输互斥 | CLI `--transport` 二选一，同进程无法表达"双传输"，故"第二次 Init 失败"路径不可达；改为实测**双进程**（UDP 5597 + TCP 5598）同时在线且各自探针全通过 | 部分通过（见 §3.4） |

### 2.1 页切换语义（源码依据，写入 README）

- `thirdparty/XCPliteV1.2/src/xcpLite.h:468-469`：`XCP_CALPAGE_DEFAULT_PAGE 1`（FLASH/参考页）、
  `XCP_CALPAGE_WORKING_PAGE 0`（RAM/工作页）——**与常见约定相反**。
- `thirdparty/XCPliteV1.2/src/xcpLite.c:707` 读、`:798` 写都走 `c->xcp_access` 指向的页；写非工作页直接
  `DBG_PRINTF_ERROR("attempt to write default page ...\n")` 并返回 `CRC_ACCESS_DENIED`。
- `:954` `CopyCalPage` 仅接受 `srcSeg == dstSeg && srcPage == DEFAULT(1) && dstPage == WORKING(0)`。
- 首次探针用 `kEcu` 切页 + 段号 1，分别得到 "write default page" 与
  `unsupported or invalid calseg copy operation`——因为 V1.2 的段号即 `CalSegList` 索引，只有段 0 合法。

### 2.2 TCP 联调注意事项

`libxcp` 的 `XcpMaster::Connect()` 在 TCP 上会先发 `GET_VERSION`（`0xC0`），V1.2 回 `ERR_CMD_UNKNOWN`
（原因见 §3.5），日志出现 `XCP RX ERR cmd=0xC0 code=ERR_CMD_UNKNOWN`，**不影响**后续会话建立。

### 2.3 GET_VERSION 实测

探针调用 `XcpMaster::ProbeVersion()` → `outcome=UnimplementedCommand`（枚举值 1），`response` 为空。
判定：这不是本程序的缺陷，而是 V1.2 上游的编译期裁剪（§3.5），故计划 §7 第 1/2 项的版本断言改为
「A2L 声明 + 启动日志」。

### 2.7 探针重建命令（会话结束后手动重建）

```bash
g++ -std=c++20 -O1 -I include -I thirdparty/a2l-sdk/a2lbridge/include -I thirdparty/a2l-sdk/include \
    -I cmake-build-release/_deps/glog-src/src -I cmake-build-release/_deps/glog-build \
    .tmp/probe/v12probe.cpp cmake-build-release/libxcp.a \
    cmake-build-release/thirdparty/a2l-sdk/a2lbridge-build/liblibxcp_a2lbridge.a \
    cmake-build-release/thirdparty/a2l-sdk/libliba2l.dylib \
    cmake-build-release/_deps/glog-build/libglog.a -Wl,-rpath,@executable_path -o .tmp/probe/v12probe
```

必须链 glog（否则 `Undefined symbols: google::LogMessage::stream()`），且 `liba2l` 只能用 dylib
（`a2llib-build/liba2l.a` 缺 `liba2l::CreateDoc`）。

---

## §3 实施期发现与对计划的偏离

### 3.1 不能 `add_subdirectory(thirdparty/XCPliteV1.2)`（计划 §1.7 已验证，实施沿用）

`thirdparty/XCPliteV1.2/CMakeLists.txt:120` 的 `add_library(xcplib ...)` 与 V2.2 目标同名，
本目录自带 `xcplite_v12` 目标 + 显式 10 个 `.c` 源列表，并 `enable_language(C)`。

### 3.2 **偏离**：地址模式由「绝对寻址」改为「事件相对寻址 ext=3」

计划 §5.3 第 2 步写的是 `A2lSetAbsoluteAddrMode`。实测 DAQ 净荷全零，源码定位：

- `thirdparty/XCPliteV1.2/src/xcp_cfg.h:202` 在 `#ifdef XCP_ENABLE_APP_ADDRESSING` **之外**无条件定义
  `#define XCP_ADDR_EXT_APP 0x01`，而 CASDD 模式（`:110-120`）下 `XCP_ADDR_EXT_ABS` 也是 `0x01` → 撞车。
- `thirdparty/XCPliteV1.2/src/xcpLite.c:2052` 的 DAQ 采样循环：
  `if (app_handled || (e_ext == XCP_ADDR_EXT_APP)) { memset(dst, 0, n); }` → ext==1 的每个 ODT 条目被填零。
- 对照 MTA 路径 `xcpLite.c:1252` 走 `XcpAddrIsAbs()` 分支正常，因此表现为「POLL 正常、DAQ 全零」。

改法（不改第三方源码）：注册用 `A2lSetRelativeAddrMode_i(event_id, base)`（`inc/a2l.h:318` →
`__i(event_id, 1, base)`，即 `addr_ext = XCP_ADDR_EXT_DYN + 1 = 3`）；触发用
`XcpEventExt_Var(event_id, 2U, nullptr, base)`（base 落在 index 3）。**不能用** `DaqTriggerEventExt_i`
宏：其 `trg__AASD__##name` 用的是字面量 `name` 而非参数，多处调用共用同名静态句柄。

改后 A2L：`INSTANCE fast ... 0x0 ECU_ADDRESS_EXTENSION 3`、`INSTANCE slow ... 0x10000 ECU_ADDRESS_EXTENSION 3`
（高位即事件号），DAQ 解码成功且 `counter` 递增。

### 3.3 **补充**：A2L 后处理补 `READ_WRITE`

计划未提。V1.2 在段相对模式下**不输出** `READ_WRITE`，而 `tools/xcp_master/symbol_policy.hpp:137`
对可写符号要求 `A2L 未标记为可写` 之外的状态，缺失会导致主站拒绝标定写入。
故新增 `PatchCalibrationInstanceWriteAccess()`：只改命中唯一的一条 `INSTANCE` 声明行，写
`<file>.patch.tmp` → 回读比对 → `std::filesystem::rename` 原子替换；匹配数 0 或多于 1 都返回退出码 4。
实测行：`/begin INSTANCE cal "从站标定段（参考页/工作页）" CalibrationParameters 0x80000000 ECU_ADDRESS_EXTENSION 0 READ_WRITE /end INSTANCE`。

### 3.4 未覆盖：计划 §7 第 7 项「同进程重复 Init 失败」

`thirdparty/XCPliteV1.2/src/xcpEthServer.c:76-121` 的单例保护（`if (gXcpServer.isInit) { DBG_PRINT_WARNING(...); return false; }`）
在源码上存在，但本程序 CLI 是二选一，无法构造第二次 Init。已用「双进程同时在线」实测替代，
并在 README「已知限制」第 4 条写明单实例约束。**该路径没有自动化或手工用例覆盖**。

### 3.5 GET_VERSION 在 V1.2 上被编译期裁掉（不可修，不改三方库）

`thirdparty/XCPliteV1.2/src/xcpLite.c:3277-3292` 的 `case CC_LEVEL_1_COMMAND:` / `case CC_GET_VERSION:`
整体位于 `#if XCP_PROTOCOL_LAYER_VERSION >= 0x0104` 内，而 V1.2 是 `0x0102`
（`thirdparty/XCPliteV1.2/src/xcp_cfg.h:28`）→ 整个 Level-1 命令族被编译掉，`0xC0` 落到
`default: error(CRC_CMD_UNKNOWN)`。A2L 的 `OPTIONAL_CMD` 列表也相应不含 `GET_VERSION`。
因此版本证据只能来自：A2L `PROTOCOL_LAYER 0x102` + `XCP_ON_UDP_IP/TCP_IP 0x104`、启动日志 `Version=1.2`、
`CONNECT` 响应里的 PL/TL 主版本字段。

### 3.6 **偏离**：模型单测用独立目标，未注入 `XcpliteIntegration`

计划 §8 原打算把 `xcp_slave_demo_v12_model_test.cpp` 注入 `XcpliteIntegration`。实际改为独立目标
`XcpSlaveV12ModelTest`（只编译 `slave_model.cpp`，不链 `xcplite_v12`、不链 libxcp），原因：
`XcpliteIntegration` 已链接 V2.2 的 `xcplite`，同进程再链 V1.2 会有同名符号冲突。
`slave_model.hpp` 因此被约束为**不包含任何 XCPlite/libxcp 头**（约束写在文件头注释里）。

### 3.7 A2L 每次启动必须重生成

事件相对地址的编码是 `(event << 16) | offset`，offset 相对**进程内**测量块对象基址，
而基址随 ASLR 变化 → `--a2l-mode write-once` 仅用于验证文件生成策略，不能跨进程复用 A2L 做联调。

---

## §4 测试

### 4.1 新增单测（`tests/xcp_slave_demo_v12_model_test.cpp`，12 项）

```
ctest --test-dir cmake-build-release -R XcpSlaveV12ModelTest --output-on-failure
→ 100% tests passed, 0 tests failed out of 12
```

覆盖：默认标定中性、构造即出首帧、`tick` 推进与相位回绕、方波半周期翻转、`run=0` 冻结、
`gain`/`offset` 变换、`limit` 对称限幅（含负 limit 与 `last_diagnostic=-1`）、`corrections[i]` 到达
`samples[i]`、慢速块八种基础类型派生、`stats`/`history` 一致性、同序列确定性、标定快照往返。

首跑 1 项失败：相位回绕用例把循环写了 64 次（期望 tick=64、相位 0，实得 tick=65、相位 1），
改为 `kPhaseCount - 1` 次后转绿。

### 4.2 全量回归（macOS Release）

```
ctest --test-dir cmake-build-release -j 4 → 958 项，3 failed，53.8 s
```

3 项均为本次改动前既有问题：`a2l_perf_gen`（需 Python 3.10+）、`ParseLargeFileUnderBudget`
（依赖上者 fixture）、`ExampleXcpIntegrationDemoCppDemo`。

---

## §5 已知限制（与 README 一致）

1. DQ 条目必须按事件分开配置（A2L 中 `fast`/`slow` 分别绑定事件 0/1，跨事件装载被拒）。
2. 不能用绝对地址模式（§3.2 的 APP/ABS 撞车；本程序用 ext=3 绕开）。
3. A2L 的 `MEMORY_SEGMENT` 段号为 `0`，页命令的段号与之一致（V1.2 段号即索引，EPK 段占用保留编号）。
4. 单实例：进程级单例，同进程只能一种传输、一个端点。
5. 端到端（DAQ/POLL/标定/切页）**无自动化用例**，仅本次探针验收；探针未入库。

---

## §6 结论

计划 §7 的 7 项中 6 项通过，第 7 项（同进程重复 Init 失败）因 CLI 二选一而不可达，以双进程并行替代验证
并在 README 记录约束。与计划的 3 处偏离（相对寻址、A2L `READ_WRITE` 后处理、单测目标独立）均已在上文给出
源码级依据；未修改任何第三方源码。