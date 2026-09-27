# XCPlite Slave 协议调试 —— 批次19 核证记录：Seed&Key 机制不通过，设计停止

- 日期：2026-07-29
- 任务出处：`XCPlite_Slave_协议调试_后续路线_批次18-21_计划.md` §2（19-0：核证表 + "不通过则本批停在设计"）
- 结论：**19-0 不通过 → 批次19 停在设计，19-1～19-4 取消**。真实对手端 Seed&Key 闭环在"不改 `thirdparty/XCPlite`"约束下不可行；Phase2-03 按本记录 §2 的覆盖口径关闭。

## 1. 核证表（文件:行 + 结论）

| # | 证据点 | 位置 | 结论 |
|---|---|---|---|
| V1 | 死代码开关 | `thirdparty/XCPlite/src/xcplite.c:2195-2196`（`/* Not implemented, no shared.ProtectionStatus checks */` + `#if 0`）→ `:2224`（闭合 `#endif`） | `CC_GET_SEED`(:2198)/`CC_UNLOCK`(:2210) 处理器整体位于 `#if 0` 死代码内；`:2197` 的 `#ifdef XCP_ENABLE_SEED_KEY` 嵌套其下。**任何编译宏都无法激活**——override 头路线失效 |
| V2 | 上游自述 | `xcplite.c:25`（文件头特性清单） | 明示 `Seed & key is not supported`，V1 非笔误而是上游设计 |
| V3 | 配置开关面 | `thirdparty/XCPlite/src/xcp_cfg.h:342-343` | 唯一编译开关 `// #define XCP_ENABLE_SEED_KEY`（默认注释）；经 `XCPLITE_CFG_OVERRIDE` 定义后被 V1 的 `#if 0` 挡住，无副作用可验 |
| V4 | 回调挂点 | `inc/xcplib.h:554-1059`、`src/xcplite.h:502-627`、全树 grep `ApplXcp*` | `ApplXcpGetSeed`/`ApplXcpUnlock` 仅在死代码 `xcplite.c:2202/:2212` 被引用，**全树无声明、无实现、非弱符号**（应用侧 `extern "C"` 自实现也无法改变 V1）。计划中假设的 `ApplXcpCompareKey` 不存在；死代码实际签名形态为 `uint8_t ApplXcpUnlock(key*, len)`→资源位掩码、`uint8_t ApplXcpGetSeed(resource, seed*)`→seed 长度 |
| V5 | 保护状态来源 | `xcplite.c:2195`（注释）、`:2229`（`CRM_GET_STATUS_PROTECTION = 0;`）、`:2081`（`CRM_CONNECT_RESOURCE = RM_DAQ \| RM_CAL_PAG;`） | GET_STATUS 保护字节**硬编码 0**；CONNECT 的 `RM_DAQ\|RM_CAL_PAG` 是能力声明而非保护位；命令分发处无 `ProtectionStatus` 检查实现（V1 注释原话） |
| V6 | 命令编码参考 | `thirdparty/XCPlite/src/xcp.h:34-35`（`CC_GET_SEED 0xF8`/`CC_UNLOCK 0xF7`）、`:533-539`（CRO/CRM 宏） | 编码与规范一致，与我方 `include/libxcp/protocol_types.hpp:71-72`（`GetSeed=0xF8`/`Unlock=0xF7`）吻合——master 侧协议正确性不依赖对手端可编译性即可断言 |

## 2. 处置（覆盖口径）

1. **对手端实然已钉**：批次17 `tests/xcplite_write_test.cpp` → `UnlockAgainstXcpliteReportsCmdUnknown` 断言真实 Slave 对 GET_SEED/UNLOCK 回 `ERR_CMD_UNKNOWN`（"钉住事实 + 注释指回核证"纪律，§5 全局约束 4）。
2. **Master 协议能力覆盖不变**：分段 GET_SEED、`ExecuteGetSeed`/`ExecuteUnlock`、`XcpMaster::Unlock(resource, SeedKeyCalculator)`（`include/libxcp/xcp_master.hpp:33/:210`）、错 Key 会话失败语义，由仓内 loopback 套件覆盖：`tests/seed_key_test.cpp`（:368/:402/:499/:515 用 `TestKeyAlgorithm`）、`tests/xcp_udp_loopback_test.cpp:720/:751`、`tests/udp_test_slave.cpp:252`（`key[i]=seed[i]^0x5A^i`）。
3. **未来若要"真实可配置栈走通 Seed&Key"**：需要换用真正编译了 seed&key 处理的对手端实现（本 vendored XCPlite 上游即 `#if 0`），或按例外流程显式批准修改 thirdparty（现行 §5 约束 1 禁止）。当前无此需求，不开口子。

## 3. 对验收映射的影响（已回写路线计划 §6）

- 原 Phase2-03"受保护资源修改（Seed&Key）"闭环口径：**"真实对手端 ERR_CMD_UNKNOWN 钉住（实然）+ loopback 协议闭环（应然）"**，替代原计划的"真实对手端解锁写"。
- 批次20（DAQ 实时）与批次21（工程收口）不受本批结论影响，按路线计划继续推进。

## 4. 变更记录

| 轮次 | 内容 |
|---|---|
| B19-1 | 19-0 机制核证（V1-V6）不通过，批次19 停在设计；19-1～19-4 取消；§6 验收映射改判；下一步批次20 |
