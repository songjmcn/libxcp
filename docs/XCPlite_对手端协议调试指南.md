# XCPlite 对手端协议调试指南

> 适用：libxcp 以 `thirdparty/XCPlite` 作为真实对手 Slave 做 XCP 1.3 UDP/IP 协议调试。
> 依据全部来自批次 16–21 的实然核证（行号为当前树实测）；纸面规范与对手端实然冲突时，
> 一律以对手端为准（"实然方言"）。上位计划见
> `code-plan/XCPlite_Slave_协议调试_后续路线_批次18-21_计划.md` 及各批次实施记录。

## 1. 拓扑与报文流

- 单一 UDP socket 同时收 CRO 与发 DTO/CRM（`thirdparty/XCPlite/src/xcplite.c`，
  `XcpMainFunction :3627-3782` 收包→解析→分发）。
- Master 侧 `UdpTransport::OnPacketReceived` 按首字节分派：
  `0xFF`=RES、`0xF7`=ERR、`0xFE`=CMD 回显过滤、`0xFD`/`0xFC`=ACQ/BITCREF、
  `0x00–0xFB`=DTO（`src/response_parser.hpp` 默认分支）。
- XCPlite 的 DTO 首字节（PID）是 **ODT 序号回显**，不是 CAN 直通式的任意标识；
  解析 envelope 前必须先与建表时的 ODT 顺序对照。
- 事件泵：Slave `main.cpp` 循环 `DaqCreateEvent(testev)` + `XcpMainFunction`
  （批次 17 引入），Master 不轮询即可收到动态表触发的 DTO。

## 2. 构建开关（CMake）

| 开关 | 作用 | 位置 |
| --- | --- | --- |
| `LIBXCP_BUILD_XCPLITE_SLAVE=ON` | 编译对手端与 Xcplite 集成套件（`LIBXCP_BUILD_EXAMPLES=ON` 时自动跟随置 ON） | 根 `CMakeLists.txt` XCPlite 段 |
| （无开关）A2L 解析/生成链 | 必编组件，随主工程无条件构建（需本机 Boost） | 根 `CMakeLists.txt` |
| `XCPLITE_CONFIGURATION=default` | 对手端配置选型 | 根 `CMakeLists.txt` |
| （自带）`OPTION_ENABLE_A2L_UPLOAD` | IDT 4 A2L 上传门 | `thirdparty/XCPlite/src/xcplib_cfg.h:168`（default 配置无需额外定义） |
| （死）`XCP_ENABLE_SEED_KEY` | 上游注释停用 | `thirdparty/XCPlite/src/xcp_cfg.h:342-343`，且分发在 `#if 0`（`xcplite.c:2196`），任何宏无法激活 |
| `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=D:/project/libxcp/cmake-build-release/_deps/googletest-src` | gtest 固定源目录（免在线拉取失败） | 首次 configure |

构建/测试范式（MSVC VS generator **必须串行**，勿加 `-j` 给 cmake --build）：

```powershell
cmake -S . -B cmake-build-xcplite -DCMAKE_BUILD_TYPE=Release `
  -DLIBXCP_BUILD_XCPLITE_SLAVE=ON `
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=D:/project/libxcp/cmake-build-release/_deps/googletest-src
cmake --build cmake-build-xcplite --config Release --target libxcp_tests
cmake --build cmake-build-xcplite --config Release --target XcpliteIntegration
ctest --test-dir cmake-build-xcplite -C Release -j4
```

基线：全量 ctest **394/394**（唯一 SKIP：`Ag1_Cto8`，已知环境性）；
`clang-format.exe --dry-run -Werror` 0 告警（VS 既有 C4244 噪音
`tests/xcplite_a2l_read_test.cpp:270/:291` 不属门禁）。

## 3. 实然方言速查（与规范纸面不一致处）

1. **GET_ID CRO 无 MODE/reserved 字节**：`[0xFA][IDT]` 两字节即完整 CRO
   （对手端 `xcp.h:524` `CRO_GET_ID={COMMAND,IdentificationType}`；
   规范 §7.5.1.6 的四字节形态在 XCPlite 里 mode 字节就是 IDT）。
2. **GET_ID CRO 短包检查在类型 switch 之前**（`xcplite.c:2136`）；
   类型 switch 仅 `0x00/0x01/0x02/0x04/0x05/0x0A3…`，未知→`ERR_OUT_OF_RANGE`。
3. **A2L 上报 MODE=0x00 + LENGTH=文件大小**（`xcplite.c:2169-2175`），
   `ApplXcpGetId` 的 LENGTH 恒 0 分支是死代码；`openFile` 失败→LENGTH=0，
   master 应报"对端无上传能力"而非死等。
4. **FILE MTA 是纯顺序读，无 fseek**（`xcpappl.c:575-596` 直接 `fread`）：
   UPLOAD 必须按上报 LENGTH 连续分块（单块 ≤ MAX_CTO-1 且 ≤255）；
   中途失败对端已 `closeFile`，恢复手段=重发 GET_ID 重开文件再整读。
5. **CONNECT 不自动建表**：动态表前页表为空，走静态/预定义路径直接
   `ERR_PAGE_OUT_OF_RANGE(0x28)`。
6. **TEST_CHECKS 拒绝单列表 START**：改 `SELECT` + `START_STOP_SYNCH(StartSelected)`。
7. **时间戳仅 prepend 在事件首个 ODT**（`xcplite.c:3729/3735`），4 字节 1ns
   （TIMESTAMP_FIXED，`SET_DAQ_PROCESSOR` 的 `TIMESTAMP_MODE=0x0C` size_code=4）。
8. **FREE/ALLOC 无"运行中"门（D9 实测）**：运行中整表 FREE→ALLOC 重建是合法停流
   重建路径；FREE/ALLOC 与 DAQ 活动无关命令冲突才回 `ERR_DAQ_ACTIVE(0x11)`。
9. **SET_MTA 接受扩展 0xFD（FILE 段）**（`xcplite.c:618-622` 不校验 ext 值），
   ENHANCED_OFFSET 语义走该段。

## 4. 常见负响应定位表

| ERR 码 | 含义 | 对手端行号 | 典型场景与处置 |
| --- | --- | --- | --- |
| `0x28` PAGE_OUT_OF_RANGE | 页/表空 | `xcplite.c:2644/:2629` | CONNECT 后未建表就读→先走动态 ALLOC 全链 |
| `0x11` DAQ_ACTIVE | 与活动 DAQ 冲突 | `xcpappl.c:681` | 运行中动 START/STOP 语义命令→按 D9 用 FREE 重建或先停 |
| `0x20` CMD_UNKNOWN | 命令未编译/未实现 | `xcplite.c` 分发 default | 静态表路径命令、GET_SEED/UNLOCK（`#if 0` 死代码 `:2196-2224`）均属此类，勿重试 |
| `0x25` ACCESS_LOCKED | unlock 失败 | `xcplite.c:2219` | 对手端 Seed&Key 整体停用（批次19 核证），本仓库不编排 |
| `0x22` 短包语法错 | CRO 长度不足 | `xcplite.c:2136`（GET_ID 例） | 自查编码器：先读 `xcp.h` CRO 结构体，勿按规范默认形态 |
| `0x23` OUT_OF_RANGE | IDT/参数越界 | `xcplite.c:2186` | GET_ID 仅支持枚举内 IDT；换 IDT 或放弃 |
| UPLOAD 超限 | 游标越过 EOF→对端 closeFile | `xcpappl.c:575-596` | 重发 GET_ID 重开文件，从头顺序整读 |

## 5. 排障五步法

1. 抓 CRO 首字节确认命令码，与 `include/libxcp/protocol_types.hpp` 对照；
2. 到对手端 `xcplite.c` 分发 switch 找到该命令的实然检查（顺序、长度、门），
   规范文档只作背景；
3. 收到 ERR → 按 §4 表定位行号，读该行上下文确认触发条件；
4. 改 master 侧适配（编码器/编排），**不修改 thirdparty 源码**；
5. 补 mock 单测 + 对手端 E2E 双测试，全量 ctest 门禁后提交并在批次记录留行号证据。

## 6. 测试运行目录治理（21-3）

- Slave 工作目录在 `xcplite_runs/run_<port>/`；`Stop()` 即时回收自身目录
  （`tests/xcplite_slave_fixture.cpp:332-337`），`Start()` 前 `PruneStaleRunDirs`
  按时效（mtime > 1h）清扫崩溃/强杀残留（`:242-267`）。
- 端口基数 `45000 + pid%3000 + 游标*8`（`:274-275`），并发用例互不重叠。
- 残留判据：**端口有活进程=在用；无活进程且 mtime 旧=孤儿**，可删。
- 并行压测基线：`ctest -R Xcplite -j8` 连续两轮 28/28 PASS，run 目录零残留。
