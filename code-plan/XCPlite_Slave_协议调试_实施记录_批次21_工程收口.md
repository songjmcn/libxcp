# XCPlite 对手端协议调试 实施记录 — 批次21（工程收口）

> 日期：2026-07-31 · 分支 `feat_add_xcp_slave_tests` · 计划见
> `code-plan/XCPlite_Slave_协议调试_后续路线_批次18-21_计划.md` §4（原 §5 系笔误）。
> 范围：21-1 POSIX 验证、21-2 GET_ID→UPLOAD 拉 A2L、21-3 runs 治理+端口压测、
> 21-4 对手端调试指南、21-5 SDK Debug 销账。全路线批次 18→19→20→21 至此收口。

## 1. 任务完成总表

| 任务 | 结论 | 关键证据 |
| --- | --- | --- |
| 21-3 runs 治理+端口压测 | ✅ | `PruneStaleRunDirs`（mtime 1h，fixture :242-267）+ `Stop()` 自回收（:332-337）+ 端口基数 pid 混入（:274-275）；`ctest -R Xcplite -j8` 连续两轮 28/28 PASS，run 目录零残留（+35 行随本 docs 收口提交） |
| 21-2 GET_ID/A2L 上传 | ✅ | 四件套（codec/parser/executor/master）+ mock 4 用例 + E2E `FetchA2lViaUploadMatchesDiskFile` 逐字节全等（345ms）；全量 ctest **394/394**（1 已知 SKIP `Ag1_Cto8`）；提交 8d912e9（10 文件 301 行） |
| 21-4 调试指南 | ✅ | `docs/XCPlite_对手端协议调试指南.md`（拓扑/开关/9 条实然方言/负响应定位表/排障五步/治理） |
| 21-1 POSIX 实跑 | ⛔ 环境阻塞 | `wsl.exe -l -q` → `Wsl/E_ACCESSDENIED`（exit -1，2026-07-31 实测）；本机无 POSIX 运行环境，MSYS2 ucrt64 系 Windows-PE 非 POSIX。夹具 POSIX 分支（fork/exec/SIGTERM，`tests/xcplite_slave_fixture.cpp`）代码在树，恢复路径=真实 Linux/macOS runner 跑 `Xcplite*` 全套 |
| 21-5 SDK Debug prepared | ✅ 销账 | Debug+Release 双 prepared 树实证存在（批次18-E 交付复核） |

## 2. 21-2 核证与实现（实然推翻纸面）

- **CRO 实然两字节** `[0xFA][IDT]`，无规范 §7.5.1.6 的 MODE/reserved（对手端
  `xcp.h:524` `CRO_GET_ID={COMMAND,IdentificationType}`；短包检查 `xcplite.c:2136`
  在类型 switch 之前）。编码器按实然实现并在注释标注差异。
- **IDT 4（ASAM_UPLOAD）上报 MODE=0x00 + LENGTH=文件大小**
  （`xcplite.c:2169-2175`）；`ApplXcpGetId` 的 LENGTH 恒 0 分支为死代码；
  `openFile` 失败→LENGTH=0→master 报 `UnsupportedFeature`（不死等）。
- **FILE MTA 纯顺序读、无 fseek**（`xcpappl.c:575-596`）：`FetchA2lViaUpload`
  必须按 LENGTH 连续分块（单块 ≤ min(MAX_CTO-1, 255)）；中途失败对端 closeFile，
  恢复=重发 GET_ID 重开文件整读。
- CRM 无 PID 前缀、字节级语义自 `res.data` 起算（`xcp.h:519-520` 宏 +
  `PositiveResponse.data` 已剥 0xFF，`response_parser.hpp`）：
  `transfer_mode=res.data[0]`、`length=LE32(res.data[3..6])`、DATA 自 `res.data[7]`。
- Slave 无需任何开关改动：default 配置自带 `OPTION_ENABLE_A2L_UPLOAD`
  （`xcplib_cfg.h:168`）；slave stderr 实证 ready 横幅含 `a2l=xcp_test_slave.a2l`。

## 3. 测试资产

- mock 四用例 `tests/xcp_daq_test.cpp`：`XcpGetIdUpload`
  `FetchA2lSplitsIntoSequentialUploadChunks`（15/15/10 分块，GetId×1 Upload×3）、
  `MidStreamUploadFailurePropagates`、`InlineModeRejectedWithoutUpload`、
  `ZeroLengthRejected`。
- E2E `tests/xcplite_daq_test.cpp:475` `FetchA2lViaUploadMatchesDiskFile`：
  真对手端整文件上传 vs `slave_.A2lPath()` 盘上字节全等。
- 全量门禁：`ctest --test-dir cmake-build-xcplite -C Release -j4` →
  **394/394 passed, 0 failed**（SKIP：`Ag1_Cto8` 已知）。

## 4. 提交链与波折留痕

- HEAD 链：a4565ed(18) → fe3eaa4(19) → f732ccd(20) → 75d2d2b(20 回写) →
  **8d912e9**(21-2 代码) → **本记录+指南+21-3 夹具**（docs 收口提交，哈希见
  `git log` 本批第二条；**路线计划回写（§4 横幅+状态列+§7 轮次行）并入同一收口提交**。
- **幻影提交教训**：早前一轮上下文压缩曾把尚未落盘的记录/指南/回写记成
  "已提交 d5e8810"；本轮 `git log`/`Test-Path` 四路核实该对象不存在、文件未落盘，
  遂以本文为准重建。留痕规则：凡"已提交"表述，必须先经 `git log` 验证后才可写入
  计划/记录。
- 勿提交：`docs/XCP_1.3.0_功能简介与页码速查.md`（无关工作区改动，始终排除）。

## 5. 路线全局收口状态

- 批次18 结构叶子展开 ✅；批次19 Seed&Key 19-0 核证不通过停在设计 ✅；
  批次20 DAQ 实时采集 ✅；批次21 工程收口 ✅（21-1 阻塞已如实记录，非代码缺陷；394/394 门禁系 Windows 面实证，非 POSIX 编译面证据）。
- 范围外（维持排除）：STIM、PACKED_MODE、时间同步、修改 thirdparty 源码。
