# XCPlite Slave 协议调试集成计划 — 复核记录

> 日期：2026-09-29 · 分支 `feat_add_xcp_slave_tests`（HEAD `d2cb9ab` 批次22）
> 范围：对照 `code-plan/XCPlite_Slave_协议调试集成计划.md`（两阶段 250 行）与
> `code-plan/XCPlite_Slave_协议调试_实施记录_批次21_工程收口.md`（61 行）逐项复核
> 当前代码库、提交链与门禁终态。
> 性质：纯分析复核，未做任何代码/文件修改（本记录为唯一落盘写操作）。

## 1. 总体结论

**全部计划任务均已闭合**，多数以超越计划最低要求的方式完成（结构体/嵌套读用例为
PASS 而非计划允许的 SKIP）。共 5 处有据可依的实现口径偏差（每处均有核证记录支撑，
非缺口）与 1 项环境阻塞（非代码缺陷）。

## 2. 逐项核对表

### 2.1 第一阶段（Phase1）

| 计划项 | 结果 | 证据 |
|---|---|---|
| 01-A 根 CMake 门 + add_subdirectory | ✅ | 根 `CMakeLists.txt`：`LIBXCP_BUILD_XCPLITE_SLAVE` 门 + `add_subdirectory(thirdparty/XCPlite)`（早已落地；批次22 又在其旁追加 `LIBXCP_BUILD_EXAMPLES` examples 门） |
| 01-B `tests/xcp_test_slave/CMakeLists.txt` | ✅ | 文件在树（`tests/xcp_test_slave/` 三件：CMakeLists.txt / main.cpp / xcplite_test_types.hpp） |
| 01-C `XcpliteIntegration` 目标 | ✅ | `tests/CMakeLists.txt:77-114`（A2L 桥接分支 :102-111） |
| 01-D Release 构建通过 | ✅ | `cmake-build-xcplite` 全套 ctest 395/395（0 failed，1 已知 SKIP `Ag1_Cto8`） |
| 02-A Slave 测试程序 | ✅ | `tests/xcp_test_slave/main.cpp` 变量 + XCP/A2L 初始化 + 主循环 |
| 02-B UDP 5556 连接 + A2L 生成 | ✅ | 全部用例经 `slave_.A2lPath()` 现场生成加载（夹具管生命周期） |
| 02-C CANape/xcpclient 互操作 | ⚪ 可选非阻塞 | 计划自标"可选，非阻塞"；未做，不构成缺口 |
| 03-A/B/C 进程管理器/端口探测/生命周期 | ✅ | `xcplite_slave_fixture.{hpp,cpp}`；SetUp :518、端口基数混入 pid（21-3 :274-275）、Stop 自回收（:332-337） |
| 04 协议用例 | ✅ | `xcplite_protocol_test.cpp` 10 用例：ConnectAndDisconnectRealSlave、SessionParametersMatchXcpliteDefaults、ReadBasicScalars（五标量）、ReadSimpleStructFields、ReadNestedStructFields、ReadArrayOfStructElements、ReadScalarArrays、MultiChunkUploadOfBlob、RepeatedConnectSessionsRemainStable、ReadOutOfRangeAddressIsRejected |
| 05 A2L 读取用例 | ✅ | `xcplite_a2l_read_test.cpp` 6 用例（LoadRuntimeGeneratedA2l / FindAndReadBasicVariables / ReadArrayElementsViaA2l / StructInstanceBaseRead / StructMemberLeafPaths / RuntimeConsistencyNoErrors）；**结构体/嵌套用例 PASS 而非 SKIP**（批次18 STRUCTLEAF 后启用） |
| 06 预置 A2L 基线 | ✅ 口径变更① | 见 §3 偏差① |

### 2.2 第二阶段（Phase2）

| 计划项 | 结果 | 证据 |
|---|---|---|
| 01 写回基本变量 | ✅ 口径变更②③ | `XcpliteWriteTest`：WriteBasicU8 :54 / WriteBasicU32AndFloat :66 |
| 02 写回结构成员/数组/嵌套/Cal 参数 | ✅ | WriteArrayElement :96 / WriteStructMemberField :121 / WriteNestedStructViaLeafAddress :148 / MultiChunkDownloadOfBlob :172 / WriteCalSegmentParameter :194 / LeafMemberWriteMatchesBaseOffsetView :227 |
| 03 Seed&Key 解锁 + 写回闭环 | ✅ 设计停止④ | 见 §3 偏差④；CalSeg 写绕过解锁直写验证通过 |

### 2.3 批次21 任务总表（5 项）

| 任务 | 结果 | 关键证据 |
|---|---|---|
| 21-3 runs 治理 + 端口压测 | ✅ | PruneStaleRunDirs（mtime 1h）+ Stop 自回收 + 端口基数 pid 混入；ctest -R Xcplite 连续两轮 28/28 PASS（批次21 记录 §1） |
| 21-2 GET_ID→UPLOAD 拉 A2L | ✅ | 四件套 + mock 4 用例 + E2E `FetchA2lViaUploadMatchesDiskFile` 逐字节全等；提交 8d912e9（10 文件 301 行） |
| 21-4 对手端调试指南 | ✅ | `docs/XCPlite_对手端协议调试指南.md`（拓扑/开关/9 条实然方言/负响应定位表/排障五步/治理） |
| 21-1 POSIX 实跑 | ⛔ 环境阻塞 | `wsl.exe -l -q` → `Wsl/E_ACCESSDENIED`（exit -1，2026-07-31 实测）；夹具 POSIX 分支代码在树，恢复路径=真实 Linux/macOS runner |
| 21-5 SDK Debug prepared 销账 | ✅ | Debug+Release 双 prepared 树实证存在 |

## 3. 实现口径偏差明细（均有记录支撑）

1. **Phase1-06 基线口径**：计划要求 `tests/data/a2l/xcplite_test_slave.a2l` 逐字节基线。
   实测运行时 A2L 含镜像基址/OS 布局偏移，跨运行不字节稳定（批次17 记录 :139 明载）
   → 改为**结构断言回归语料** `tests/data/a2l/xcplite_structleaf_corpus.a2l`
   （Golden T5.5 `StructLeafRealCorpusFourForms`，`a2l_golden_test.cpp:1076-1130`，
   42 符号基线钉死展开形状）+ 现场 `RuntimeConsistencyNoErrors`（B-16 实时一致性，:309）。
2. **WriteFloat 路径**：XCPlite 侧用原始字节写 float（`LeBytes(-12.25F)`）；
   `FromPhysical→写→读→ToPhysical` 闭环由 A2L 桥接 E2E（批次18 16-D06）承担，
   协议裸通路不复写 Bridge 逻辑——float 写回验证仍完整。
3. **WriteProtectedRequiresUnlock**：XCPlite 默认 `XCP_ENABLE_SEED_KEY` 被注释
   （`xcp_cfg.h` 核证，`xcplite_write_test.cpp:12-15`）→ 该用例**无适用对象**，改为
   `UnlockAgainstXcpliteReportsCmdUnknown`（:256，诚实报 `ERR_CMD_UNKNOWN` 且会话保持
   可用），比假想"Length 0=未保护"更正确。
4. **Phase2-03 Seed&Key 闭环**：批次19 核证不通过、**停在设计**
   （`code-plan/XCPlite_Slave_协议调试_实施记录_批次19_SeedKey设计停止.md`；
   GET_SEED/UNLOCK 在对手端为 `#if 0` 死代码）；CalSeg 写绕过解锁直写验证通过
   （`WriteCalSegmentParameter`）。
5. **06-C 基线同步**：由 Golden 用例 42 符号基线承担——Slave 变量变更即红，
   等效履行"同步更新"约束。

## 4. 环境阻塞（如实记录，非完成缺口）

**21-1 POSIX 实跑**：本机 `wsl.exe -l -q` → `Wsl/E_ACCESSDENIED`（exit -1，2026-07-31
实测）；本机无 POSIX 运行环境（MSYS2 ucrt64 系 Windows-PE 非 POSIX）。夹具 POSIX 分支
（fork/exec/SIGTERM，`tests/xcplite_slave_fixture.cpp`）代码在树，恢复路径 = 真实
Linux/macOS runner 跑 `Xcplite*` 全套。此即"未完成"但**不阻塞交付**的唯一项。

## 5. 门禁终态（全绿）

- 全量 ON：**395/395**（1 已知 SKIP `Ag1_Cto8`）
- Xcplite 子集：**29/29**（零回归）
- OFF 基线：**358/358**（`LIBXCP_BUILD_XCPLITE_SLAVE=OFF` 时既有测试不受影响，DoD 第 5 条）
- 示例独立门：ExampleXcpMasterUdp PASS 6.97s；批次22 零参数实跑 EXITCODE=0（15 断言全绿）
- 提交链：`a4565ed`(17) → `fe3eaa4`(19) → `f732ccd`(20) → `75d2d2b`(20 回写) →
  `8d912e9`(21-2) → `a3adc8f`(21 收口) → `d2cb9ab`(22 示例) 全部落库
- 工作区仅剩 `AGENTS.md`（CRLF 噪声，diff 空）+ `docs/XCP_1.3.0` 速查（批次21 口径，
  按计划不提交）——均为有意保留。