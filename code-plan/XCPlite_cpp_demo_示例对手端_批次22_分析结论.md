# 批次22 分析结论 — examples/xcp_master_udp 示例对手端（只分析，未做任何修改）

> 分析日期：2026-09-29。依据：计划 `code-plan/XCPlite_cpp_demo_示例对手端计划.md`（v2.1）逐条对照 + 当日实测（构建、实跑、ctest）。本文档为本轮唯一新增文件；示例源码、CMake、既有计划/记录文件均未改动。

## 1. 总体结论

**代码与测试门禁已实质完成并全绿；但按计划 §5 DoD 严格核对，尚有 4 项未收口：写回读断言（当前静默空转）、examples/README.md、实施记录批次22、计划状态行回写；全部改动未提交。**

- 零参数实跑（run-dir=xcplite_runs\round10\xcp_master_udp_run）：EXITCODE=0，9 项断言全过。
- `ctest -R ExampleXcpMasterUdp`：单独 PASS（4.96 s）。
- 全量 ON（cmake-build-xcplite，LIBXCP_BUILD_EXAMPLES=ON）：**395 项，0 FAIL**，1 not-run（Ag1_Cto8，既有已知 SKIP）→ 旧 Xcplite 29 例零回归 + 新示例绿。满足 DoD「全量 ≥394、无新增 FAIL」。
- 全量 OFF（cmake-build-release-off）：本轮重建后 **358 项，0 FAIL**（Ag1_Cto8 仍 not-run）；该缓存无 `LIBXCP_BUILD_EXAMPLES` 条目 → 新门在 OFF 形态完全隔离，示例不进构建。计数与计划「352/352」之差为旧构建目录测试清单未刷新（批次20/21 新增测试后未重配置），非本批次引入回归。

## 2. 工作区盘点（git status 实测）

| 路径 | 状态 | 处置建议 |
|---|---|---|
| `examples/`（5 文件，untracked） | CMakeLists.txt、xcp_master_udp/{CMakeLists.txt, demo_process.hpp/cpp, xcp_master_udp.cpp(26262B/584行)} | 待提交（批次22 主体交付） |
| `CMakeLists.txt`（根，M） | +27 行 = `LIBXCP_BUILD_EXAMPLES` 门 + `add_subdirectory(examples)`，默认 OFF | 待提交 |
| `code-plan/XCPlite_cpp_demo_示例对手端计划.md`（untracked） | v2.1 已批准实施中 | 随批次提交 |
| `AGENTS.md`（M） | `git diff` 为空，仅 LF→CRLF 噪声 |  checkout 还原即可，不提交 |
| `docs/XCP_1.3.0_功能简介与页码速查.md`（M） | 批次21 记录明确：无关改动，**勿提交** | 保持不动 |
| `xcplite_runs\round10\`（untracked 产物） | cpp_demo_V201.a2l 15269B、cpp_demo_V201.bin 2264B、XCP_104.aml 58239B | 运行产物，不入库（run-dir 机制） |

## 3. 计划 DoD（§5）逐条核对

| DoD 项 | 状态 | 证据/缺口 |
|---|---|---|
| 零参数实跑 exit 0 | ✅ | round10 实测 EXITCODE=0 |
| ctest `ExampleXcpMasterUdp` 绿 | ✅ | 单独 PASS 4.96 s |
| 旧 29 例零回归 | ✅ | 全量 395 项 0 FAIL |
| 全量门禁 ≥394 无新增 FAIL | ✅ | 395/395（+1 SKIP 已知） |
| OFF 基线不变 | ✅（口径修正） | 重建后 358/358 0 FAIL；旧目录原清单 345/345；计数差为陈旧清单，OFF 隔离生效 |
| **写回读往返一致** | ❌ **未实现（静默空转）** | 详见 §4 关键缺口 |
| `--watch` 人工段落 | ⚠️ 待人工 | WatchLoop 已实现（kWatchSymbols={counter,temperature,speed}），未做人工长驻演示记录 |
| examples/README.md | ❌ 缺 | examples/ 磁盘仅 5 文件，无 README |
| 实施记录_批次22 | ❌ 缺 | code-plan/ 下批次17–21 俱在，批次22 无 |
| 计划状态行回写 | ❌ | 计划文件头部仍标「待批准（v2.1）」，实际已批准并实施 |
| 提交 | ❌ | 全部改动未提交（见 §2） |

## 4. 关键缺口：写回读断言实际未生效（分析要点）

现状（`examples/xcp_master_udp/xcp_master_udp.cpp`）：

1. `:559-560` 调用 `WriteBackProbe(check, master, db, "counter_max", 1234)` / `("delay_us", 2000)`。
2. 但 `cpp_demo_V201.a2l` 顶层可 `Find` 的对象只有 13 个（MODULE cpp_demo：`INSTANCE kParameters/SigGen1/SigGen2` + 10 个 `MEASUREMENT`）。**counter_max/delay_us 仅是 TYPEDEF_STRUCTURE `ParametersT` 的 STRUCTURE_COMPONENT**（`counter_max` off 0x0 U16、`delay_us` off 0x4 U32；实例 `kParameters` ECU_ADDRESS 0x80010000、SEG 寻址 ext=0、块大小 0x8）。桥接（B-12 口径）不把结构成员登记为顶层可 Find 名 → `db.Find` 返回 NotFound。
3. `Resolve()`（`:192-203`）失败时**只打 `[info]` 并返回 nullopt，不计入 FAIL**；`WriteBackProbe` 拿 nullopt 即返回 → 实跑日志出现「写回读」小节名但无写/读回动作，**9 项断言里没有任何一项覆盖写回读**。这就是 exit 0 却 DoD 未达成的原因——测试绿是「跳出来的」。
4. 计划 §1（v2.1 修订）其实已预判此点并定口径：**结构成员排除，写回读改用顶层 `VAL_BLK loop_histogram`**（main.cpp 注册 read_write=true）；但代码未落实该修订，仍试 counter_max/delay_us。注意 loop_histogram 为 128×U32=512B、且是主循环节拍统计量，写探值存在被下一循环覆盖的时序风险，选型需再斟酌。

可选收口路线（均未实施，仅分析）：
- **路线 A（推荐，改动最小）**：`Find("kParameters.counter_max")` 叶子全限定路径。桥接批次18 已做结构叶子展开（`a2l_bridge.cpp:73-102` BuildStructureLeaves；叶子 path=`实例名.成员名`），测试先例 `tests/xcplite_a2l_read_test.cpp:55-79`（`g_simple_struct.simple_i16`）。叶子带实例基址+offset 解析后的地址，标量叶子读写不受 B-12 限制。需实跑确认本 A2L（STRUCTURE_REF+INSTANCE 形态）叶子名是否同样生成、裸名唯一性是否成立。
- **路线 B**：`Find("kParameters")` 取实例基址 + 手工 offsetof（counter_max+0 / delay_us+4），先例 `tests/xcplite_write_test.cpp:191-217`（CalSeg 基址+offsetof、ext=0）。需确认桥接对 STRUCTURE/INSTANCE 是否返回 xcp_address 元数据。
- **路线 C**：按计划修订改 `loop_histogram`——但整块 512B 需分片 DOWNLOAD + 时序覆盖风险，不如 A/B。
- 无论何路线，应让 `Resolve` 失败**计入 FAIL**（或至少写回读小节产出一条真实往返断言），消除「静默跳过即绿」的假阳性面。

## 5. V1–V7 实证矩阵（本轮/批次22 实测汇总）

| 验证点 | 结果 | 实证值 |
|---|---|---|
| V1 生成落盘 | ✅ | run-dir 下 `cpp_demo_V201.a2l` 15269B + `cpp_demo_V201.bin` 2264B（WRITE_ONCE+FINALIZE_ON_CONNECT，bind 0.0.0.0 → 无 TRANSPORT_LAYER，F4 一致） |
| V2 跨运行连接 | ✅ | Run A/Run B 各自主启进程，CONNECT 均成（15 s 预算内） |
| V3 UPLOAD 全量 | ✅ | `FetchA2lViaUpload()` 与磁盘文件**逐字节一致**（15269B）；GET_ID 返回 2 字节 [0xFA][IDT]，CRM 语义自 res.data 起（批次21 既有口径） |
| V3′ 桥接加载 | ✅（经修复） | `Load(require_if_data_xcp=false)` 成功，符号总数 30。首轮失败根因：a2l_writer.c:474 生成 `/include "XCP_104.aml"`；修法=示例向 run-dir 预置 aml（compile-def `XCP_104_AML_FILE`，CMake 注入仓库 `thirdparty/XCPlite/XCP_104.aml`），未改 thirdparty |
| V4 复用（WRITE_ONCE） | ✅ | Run B 复用同一 A2L，进程日志「already exists with matching version, disabling A2L generation」 |
| V5 会话参数 | ✅ | MAX_CTO=248、MAX_DTO=1024、byte_order=0（MSB_LAST 小端）、addr_gran=1、resources=0x05 |
| V6 写回读 | ❌ | 见 §4；当前空转 |
| V7 线格式/动态符号 | ✅（根因已闭环） | Run B 四标量读成功：temperature=0x32、speed=2.71034、counter=124（300 ms 后 124→125 动态性✅）、sum=78.5623。根因：DYN 符号（ext 3..6）走 xcplite 异步队列，`queue32.c:229-237` 上游 TODO 把 fill 计入线长 → CRM 被 pad 至 4 对齐；示例侧 over-read 适配 `ReadMemoryAligned`（n′=4·⌈(1+n)/4⌉−1，即 n′≡3 mod 4，读后截前 n 字节，`:205-213`）。xcp_test_slave ext=1 同步直发（xcpethtl.c:250-253 不 pad）不受影响 → 旧 29 例零回归 |
| 附：物理换算实然 | 记录 | `ToPhysical("temperature")` 得 -2450：桥接口径 p=f+i·C+i·O（a2l_types.hpp:117），与 ASAM LINEAR 常规 p=C·i+O（raw 50→0）不一致。**属桥接既有语义（src 侧），示例不改 src，收口时 README/实施记录须如实钉此为已知差异** |

## 6. 约束符合性

- ✅ 未改 `thirdparty/XCPlite/`（aml 走预置复制，非 EMBED_AML/A2L_MODE 改造）。
- ✅ 未改 `src/`、`include/`、`tests/`（桥接换算差异、queue fill 线长均按「非目标」记录不修）。
- ✅ C++20、doxygen 中文注释、无绝对路径硬编码（exe/aml 路径全部 `$<TARGET_FILE>` / compile-def 注入）。
- ✅ POSIX fork/exec 路径未实现（demo_process.hpp 注明，受 21-1 WSL 环境既有阻塞），Windows-only 与计划 §6 一致。
- ⚠️ `Resolve()` 的 check 参数在成功路径未引用（C4100 级噪声，无害）。

## 7. 建议收口清单（待用户批准后执行，本轮不动）

1. 写回读改叶子路径（§4 路线 A，实跑验证 `kParameters.counter_max`/`kParameters.delay_us`；同时把解析失败改为计入 FAIL）。
2. 补 `examples/README.md`（两次运行工作流、--check/--watch/--no-writeback、run-dir 产物、aml 预置说明、V6/物理换算两条已知差异）。
3. 补 `code-plan/XCPlite_Slave_协议调试_实施记录_批次22_示例对手端.md`（含本轮根因链、OFF 358 口径修正、aml 修复、over-read 适配）。
4. 计划状态行回写（待批准 → 已实施+收口项）。
5. `--watch` 人工演示一次并记入实施记录。
6. 提交范围：`examples/` + 根 `CMakeLists.txt` + 计划/实施记录 + 本分析文档；排除 `docs/XCP_1.3.0` 速查与 `AGENTS.md`（CRLF 噪声还原）。
