# XCP V1.5 批次 C 实施记录（F-01 / F-02 / F-04）

- **实施日期**：2026-10-21
- **依据方案**：`code-plan/XCP_V1.5_规范符合性裁决与修复方案.md` 第 5 节批次 C
- **规范基准**：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（原始 PDF，唯一权威来源；Markdown 转换版不作为依据）
- **批次范围**：F-01（AG 地址推进单位）、F-02（AG=DWORD 的 DOWNLOAD 对齐字节）、F-04（GET_STATUS STORE_DAQ_REQ 位序）

---

## 1. 规范依据（原始 PDF）

| 项 | 规范原文位置 | 原文要点 |
|---|---|---|
| F-01 | PDF 第 112 页 Table 42 | "The address granularity indicates **the size of an element contained at a single address**. It is needed if the master has to do address calculation." |
| F-01 | PDF 第 113 页 Table 43 | `Address n` → `Byte 00 Byte 01`；`Address n+1` → `Byte 02 Byte 03`（WORD）。**地址每 +1 前进一个元素** |
| F-01 | PDF 第 113 页 正文 | "All length information which refers to the address range of the slave itself is based on the **AG (ELEMENTS)**. If the length information refers to the data stream (XCP Protocol), it is based on **bytes**." |
| F-02 | PDF 第 142 页 Table 82 | 数据段偏移列 ".. BYTEs Used for alignment, **only if AG >2**"；"**If AG = DWORD, 2 alignment bytes must be used**" |
| F-04 | PDF 第 116 页 Table 49 | 逐位定义：bit7 RESUME、bit6 DAQ_RUNNING、bit5 x、bit4 DAQ_CFG_LOST、bit3 CLEAR_DAQ_REQ、**bit2 STORE_DAQ_REQ**、**bit1 CAL_PAG_CFG_LOST**、bit0 STORE_CAL_REQ |

---

## 2. 修改范围

### 2.1 F-01 —— AG 地址推进单位

| 文件 | 改动 |
|---|---|
| `src/protocol_types.cpp` | `XcpAddress40::Advance`：`address + elements * AgToBytes(ag)` → `address + elements`；`ag` 形参保留但显式 `static_cast<void>(ag)` 并加注释说明"仅作语义说明，不参与地址推进" |
| `include/libxcp/protocol_types.hpp` | `Advance` 的 Doxygen 注释补充规范依据与"地址按元素计数"说明 |
| `src/memory_access.cpp` | `UploadChunked`/`DownloadChunked` 的地址推进随 `Advance` 自动修正；`ReadBytes` 的字节→元素换算（`byte_count / ag_bytes`）为既有正确实现，未改 |
| `tests/protocol_types_test.cpp` | 新增/更新 `AdvancesByElementsInWordAndDwordAg`、`AdvanceIsIndependentOfGranularity` |

**关键点**：`AgToBytes` 仍用于**字节长度换算**（容量、溢出检查、DTO 预算），这些用途是正确的，未改动。

### 2.2 F-02 —— AG=DWORD 的 DOWNLOAD 对齐字节

| 文件 | 改动 |
|---|---|
| `include/libxcp/protocol_types.hpp` | 新增 `DownloadAlignmentBytes(AddressGranularity)`：AG>2 返回 2，否则 0 |
| `include/libxcp/command_codec.hpp` | 构造函数新增 AG 形参；新增 `GetAddressGranularity()` / `DownloadAlignmentBytes()`；重写 `EncodeDownload` 的 `@note` 契约（原写"不在此校验"，现明确"只负责对齐字节"） |
| `src/command_codec.cpp` | 构造保存 AG；`EncodeDownload` 在 `AG>2` 时于 `[F0][SIZE]` 后插入 2 字节 0x00 对齐 |
| `include/libxcp/command_executor.hpp` | `EnsureCodec` 增加 AG 形参 |
| `src/command_executor.cpp` | `EnsureCodec` 按 AG 参与 codec 重建判定；**38 处调用点全部传入 AG**；`CheckResLength` 保持 `elements * AgToBytes(ag)`（本就正确） |
| `src/memory_access.cpp` | `MaxDownloadElements()` 容量口径改为 `(MAX_CTO - 2 - 对齐字节) / AG` |
| `tests/command_codec_test.cpp` | 新增 `DownloadAlignmentFollowsAddressGranularity`、`ShortDownloadNeverInsertsAlignment` |

**SHORT_DOWNLOAD 无需对齐**：固定头 8 字节，数据天然落在 4 字节边界（`EncodeShortDownload` 未改，新增用例锁定该事实）。

**XCPlite 互操作保护（用户选择"加保护"）**：XCPlite 走 `XCP_ENABLE_SEG_ADDRESSING`，AG 恒为 BYTE，`AG>2` 分支永不触发，DOWNLOAD 通路行为**逐字节不变**。该保护是**结构性的**（由 AG 分支天然实现），不依赖额外开关。

### 2.3 F-04 —— GET_STATUS 位序

| 文件 | 改动 |
|---|---|
| `src/response_parser.cpp` | `kStoreDaqReqMask`：`0x02U` → `0x04U`（bit2）；**新增** `kCalPagCfgLostMask = 0x02U`（bit1） |
| `include/libxcp/protocol_types.hpp` | `GetStatusResponse` 新增 `cal_pag_cfg_lost` 字段；`store_daq_req` 注释由"bit2"校正为一致表述 |
| `src/response_parser.cpp` | 解析处同步填充 `resp.cal_pag_cfg_lost` |
| `tests/response_parser_test.cpp` | `DecodesAllStatusBits` 断言更新（`0xCA` 的 bit2 为 0，故 `store_daq_req` 应为 **false**）；**新增** `StoreDaqReqIsBit2PerSpec` 直接锁定 bit1/bit2 区分 |

**命名陷阱（实施时已区分）**：规范中 `STORE_DAQ_REQ` 兼指两物——
- GET_STATUS 的**位** bit2（本次修正对象）
- SET_REQUEST 的**模式参数** `STORE_DAQ_REQ_RESUME` / `STORE_DAQ_REQ_NO_RESUME`（与本次无关，未触碰）

---

## 3. 测试 mock 的同源缺陷（本轮最重要的发现）

### 3.1 现象

F-01 修正后，`AgMatrix/MemoryAccessAgTest` 与 `AgIntegration/XcpMasterAgTest` 的 AG>1 用例**由"全部通过"变为失败**。

### 3.2 根因

两个测试 mock 都把 XCP 地址当作**字节偏移**建模：

| mock | 原模型 | 症状 |
|---|---|---|
| `tests/memory_access_test.cpp` | `MemoryImage = std::map<Address, std::uint8_t>`（每地址 1 字节）；`read()` 步进 `elements * AgToBytes`；`m_mta_ += elements * AgToBytes` | 主站与 mock **一起按错误口径**推进，偏差互相抵消，AG>1 用例**虚假通过** |
| `tests/xcp_master_integration_test.cpp` | 同上（`std::map<Address, std::uint8_t>`，`read()`/DOWNLOAD/MODIFY_BITS 均按字节步进） | 同上 |

**这解释了 F-01 为何能在测试全绿的情况下长期存在**：测试与实现共享同一误解。

### 3.3 修正

两个 mock 统一改为**地址=元素、每地址承载 AG 字节**（`std::map<Address, Bytes>`）：

- `SetMemory` → 按元素装载，第 k 个元素位于 `address + k`
- `read()` → 按元素取，步进单位为 1 个地址
- DOWNLOAD / SHORT_DOWNLOAD / MODIFY_BITS → 按元素写入
- DOWNLOAD 解析时跳过 AG 决定的 2 字节对齐前缀
- UPLOAD / SHORT_UPLOAD 容量校验改为字节口径（`elements * AG`）
- `m_mta_` 推进改为 `+= elements`（AG 不参与）

修正后 **AgMatrix 24/24、AgIntegration 8/8 全部通过**，此时这些用例才**真正开始验证** F-01。

---

## 4. 验证结果

### 4.1 单元测试

| 套件 | 结果 |
|---|---|
| `libxcp_tests`（含 CommandCodec / ProtocolTypes / MemoryAccess / AgMatrix / AgIntegration / ResponseParser） | **476 passed / 1 failed** |
| `XcpMasterSymbolPolicyTest` | 74/74 passed |
| `MeasurementAdapterTest` | 16/16 passed |
| `A2lSmoke` | passed |
| `A2lGolden` | passed（40 passed / 2 skipped，跳过项为既有） |
| `A2lE2E` | passed |
| `a2l_gen` | passed |

### 4.2 唯一失败项（既有缺陷，非本批次引入）

`UdpTransportClose.CloseWhileBlockedReturnsPromptly` —— 早于本批次已记录，测试自身边界疏漏（`tests/udp_transport_test.cpp`，`receive_poll_interval_ms = 1000` 与 `EXPECT_LT(elapsed, 1000)` 严格小于冲突）；`git diff --name-only` 确认该文件及依赖**未被本批次触及**。按 AGENTS.md"不重构已完成代码、无明确指示不加接口"未修，仅记录。

### 4.3 F-02 对齐字节实测（独立小程序直连 libxcp.a）

```
AG=1 bytes=1  align=0  CTO=F0 01 11 22 33 44          ← 与修正前逐字节一致
AG=2 bytes=2  align=0  CTO=F0 01 11 22 33 44          ← 与修正前逐字节一致
AG=4 bytes=4  align=2  CTO=F0 01 00 00 11 22 33 44    ← 新增 2 字节对齐
```

符合原始 PDF 第 142 页 Table 82。

### 4.4 构建

`cmake --build cmake-build-logging-release -j 4` 全量构建 **exit 0**，无错误。警告均来自 `thirdparty/`，`src/`、`include/` 无新增警告。

### 4.5 XCPlite 集成套件（`XcpliteIntegration`）

**47 tests from 8 test suites，全部 PASSED，25.6 s。**

---

## 4A. 阻塞性缺陷修复：集成测试基础设施（孤儿进程泄漏）

> 本节记录**非批次 C 范围**、但阻塞批次 C 回归验证的既有缺陷修复。
> 触发：用户报告"测试会卡住，先解决测试卡住的问题"。

### 4A.1 现象

`XcpliteIntegration` 整跑**无限期挂起**；逐套件隔离后确认**第一个套件即卡住**：
`XcpliteSlaveProcessTest.OccupiedPortDoesNotSatisfyReadinessAndRetriesNextCandidate`。
单跑该用例时表现为 **FAILED**（`XcpliteSlaveProcess::Start 失败: 端口 N 上的 Slave 未就绪或已退出`），
且事后遗留 **7 个 `xcp_test_slave` 孤儿进程**仍在监听 UDP 端口。

### 4A.2 根因（三处叠加，均在 `tests/xcplite_slave_fixture.cpp`）

**缺陷一（核心）：`Alive()` 具破坏性，污染 `exited` 标志**

```cpp
// 修复前：探测循环高频调用，waitpid 顺手收割并置 exited = true
const pid_t r = waitpid(pid, &status, WNOHANG);
if (r == pid) { exited = true; return false; }
return true;
```

探测循环 `while (...) { if (!Alive()) break; ... }` 每轮都调用它。一旦 `exited` 被置位，
失败路径的 `Kill()` 首行 `if (pid > 0 && !exited)` **整段短路**——明明仍存活的子进程永远不会被清理。

**缺陷二：`Kill()` 末尾无条件"洗白"状态**

```cpp
// 修复前：即便 kill 失败也标记已回收
pid = -1; exited = true;
```

调用方无从察觉回收是否真的成功。

**缺陷三：`Spawn()` 未重置 `exited`**

`fork` 成功后未写 `exited = false`，上一轮遗留的 `true` 让新一轮 `Kill()` 再次空转——
这正是**连续泄漏 7 个孤儿**（8 次重试各泄漏一个的可见部分）的成因。

**→ 泄漏累积效应**：孤儿进程持续占用 UDP 端口且**仍响应 XCP**。后续测试在端口探测时会连上
属于**已结束 run** 的旧进程，行为不确定，整跑遂表现为长时间不返回。

### 4A.3 修复

三处改动，仅限 fixture：

1. **`Alive()` 改为非破坏性**：只观察不收割，区分四种 `waitpid` 返回——
   `r == pid` → 已退出；`r == -1 && ECHILD` → 已不存在（视为退出）；
   `r == 0` → 仍运行；`r == -1 && EINTR` → 无法判定，按仍存活处理。
2. **`Kill()` 去掉 `exited` 短路**：只要 `pid > 0` 就执行
   "非阻塞回收 → SIGTERM → 2 s 超时 → SIGKILL → **阻塞 `waitpid` 确认**"，
   仅确认回收后才置 `pid = -1`。
3. **`Spawn()` 重置 `exited = false`**：杜绝跨轮状态污染。

另补 `#include <cerrno>`（新代码使用 `errno`）。
Windows 路径经核查**本无此缺陷**（`GetExitCodeProcess` 为非破坏性查询，且无 `exited` 式短路），未改动。

### 4A.4 验证对照

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 单用例 `XcpliteSlaveProcessTest` | FAILED，重试 8 次 | **PASSED**，仅重试 1 次（1015 ms） |
| 全量 `XcpliteIntegration` | 挂起不返回 | **47/47 PASSED**，25.6 s |
| 孤儿 `xcp_test_slave` | 7 个残留 | **无残留** |

日志形态变化印证根因：修复前 7 个子进程各打印 `ready` 却全被丢弃；
修复后为 `server init failed on port 51099`（预期占用）→ 立即顺延到 51103 → **一次成功**。

---

## 5. 未覆盖 / 待确认项

1. **`XcpAddress40::Advance` 的 `ag` 形参**：修正后不再参与计算，但保留形参以固化签名、避免调用点改动。若后续认为应删除，需同步改 3 处调用点（属接口变更，按 AGENTS.md 需明确指示后再做）。
2. **UPLOAD 侧对齐**（方案 §2.2 关联项）：UPLOAD 响应无需 Master 插对齐，属 Slave 侧行为；本轮仅确认现有路径无缺陷。
3. **DOWNLOAD 容量校验层级**：`CommandCodec` 只校验单字节字段上限 255，`(MAX_CTO-2-对齐)/AG` 的实际容量由 `MemoryAccess::MaxDownloadElements()` 分块层负责 —— 与方案 F-02d 的分层意图一致。
4. **XCPlite 端到端互操作**：`XcpliteIntegration` **47/47 PASSED**（§4.5），AG=BYTE 路径在结构上不受影响。
5. **集成测试基础设施**：孤儿进程泄漏已修复（§4A），属既有缺陷、非批次 C 引入。
6. **批次 D（F-09/F-10）、E1（F-08→F-06→F-07）、E2（F-05/F-12）、F（F-03+F-11）、B（GET_VERSION）尚未实施。**