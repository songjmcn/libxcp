# XCP 测量标定差距收口计划 —— P0 实施记录

## 1. 范围

本批按 `code-plan/XCP_测量标定差距收口与互操作验证计划.md` P0 执行：复核当前标定未提交修改和关键协议语义；更新 XCPlite 集成计划的 A2L/Seed&Key 口径；使用独立新目录完成 Windows Release OFF/ON 构建和全量测试。本批**没有改协议代码、没有覆盖/回滚既有工作区改动、没有修改 thirdparty**。

## 2. 文档口径变更

修改 `code-plan/XCPlite_Slave_协议调试集成计划.md`：

- Phase1-06 由易变运行时 A2L 的逐字节静态文件基线，改为 `tests/data/a2l/xcplite_structleaf_corpus.a2l` 结构语义语料 + 现场 `RuntimeConsistencyNoErrors`；明确变量/结构注册变化时同步结构断言，未来仅对已证稳定字段增加规范化快照。
- 原 Phase2-03 的“XCPlite Seed&Key 正向闭环”改成 XCPlite `ERR_CMD_UNKNOWN` 实然负例 + UdpTestSlave/Mock 的 Master 协议闭环；真实受保护写回移出当前 XCPlite DoD，待有独立支持 Seed&Key 的对手端再立计划。
- 更新 Phase1/Phase2 DoD、风险、交付清单及变更记录，避免把计划不支持的功能误写成 PASS。

事实依据：`code-plan/XCPlite_Slave_协议调试集成计划_复核记录.md` §3 说明结构语料替代及运行时一致性；`code-plan/XCPlite_Slave_协议调试_实施记录_批次19_SeedKey设计停止.md` §1 核证 GET_SEED/UNLOCK 位于 `#if 0` 死代码，宏 override 无法激活。无 thirdparty 修改。

## 3. 未提交标定修改复核及发现

- 当前工作区已有标定修改：`code-plan/XCP_1.3.0_变量标定实现计划.md`、`code-plan/XCP_1.3.0_变量标定修改说明.md` 与命令类型/编码/解析/执行、MemoryAccess、Session、XcpMaster、测试变更。本批保留这些现有改动，不把计划和修改说明当成已提交，也不重复实现。
- `docs/XCP_1.3.0_document.md:2028-2034` 将 MODIFY_BITS 列为 Calibration Optional，说明 16-bit AND/XOR mask 作用于 MTA 的 32-bit 值且不修改 MTA；`:2036-2135` 将分页命令列为 Optional。工作区的实现说明包含对应 9 命令，Release 测试有 Codec/Parser/Mock 集成覆盖；本批没有据此宣称真实分页 Slave 互操作已完成。
- **P3 安全待办（本批只记录，不改代码）**：`src/command_executor.cpp:434-452` 在 SYNCH 恢复后会再次发送 MODIFY_BITS。该位运算含 XOR，Slave 可能已执行命令但响应丢失，重复执行会反转相关位。因此不能仅以“恢复后重建 MTA”证明重试安全。当前 `tests/xcp_master_integration_test.cpp:884-914` 的 `DropNthCall` 在 Mock 分派/执行命令前直接返回空响应（`:201-206`），未模拟“副作用已发生、仅响应丢失”；其期望最终值只验证前置丢包情形。需按总计划 P3 设计并确认“结果未知/读回对账”等安全策略，再考虑实现或补测。
- Seed&Key 方面，Master 的算法和 loopback 覆盖不改变 XCPlite 对手端不支持的事实；真实锁定拒写→正确解锁写回仍待独立对手端。

## 4. 构建及测试

Windows 本机，Visual Studio 2022 生成器，两个全新隔离 build 目录；均执行 CMake configure、`cmake --build ... --config Release --clean-first`、全量 CTest（`-j 1`）。A2L ON 使用既有准备根 `build/liba2l-prepared/msvc-x64-release`，未修改该目录或 SDK。

| 配置 | 选项 | 结果 |
|---|---|---|
| OFF | `LIBXCP_BUILD_TESTS=ON`、`LIBXCP_BUILD_A2L=OFF`、`LIBXCP_BUILD_XCPLITE_SLAVE=OFF`；目录 `build-p0-off` | Release 构建成功；CTest **447/447 通过，0 失败**；1 个已知 skip：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`；总测试时间 19.15s |
| ON | `LIBXCP_BUILD_TESTS=ON`、`LIBXCP_BUILD_A2L=ON`、`LIBXCP_LIBA2L_ROOT=build/liba2l-prepared/msvc-x64-release`、`LIBXCP_BUILD_XCPLITE_SLAVE=ON`；目录 `build-p0-on` | Release 构建成功；CTest **491/491 通过，0 失败**；相同 1 个已知 skip；包含 XCPlite Protocol/Write/DAQ/A2L/Measurement 及 A2L isolation 测试；总测试时间 36.59s |

本结果只代表当前 Windows 工作区和上述配置。Linux/macOS POSIX 实跑仍未验证；也不代表真实 ECU/CANape 或分页功能互操作。测试日志中的新增标定用例全绿，但 MODIFY_BITS 丢响应副作用语义尚未覆盖，见 §3。

## 5. 结论与后续

P0 基线/文档收口完成：OFF 与 ON Release 门禁均绿；Phase1-06、Phase2-03 口径已同步。总计划 P1（测量元数据与 profile）尚未实施；P2 仍需用户选定可保护的独立 Slave/ECU 与安全测试地址；P3 必须优先处理 MODIFY_BITS 等非幂等操作的超时语义；P4 的 Linux/macOS 与非自有对手端互操作仍待验证。
