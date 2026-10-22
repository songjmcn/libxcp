# XCP Master 独立 Slave 标定与 DAQ 并行修复实施记录

依据 `code-plan/xcp_master_独立Slave标定与DAQ并行修复计划.md` 实施。

## 问题现象与根因

主站在 DAQ 运行期间无法对独立标定段（`cal.*`）执行标定，出现两类表现：

1. **准入被拒**：标定命令在 TUI 中被判定为不可用，用户看到"不适用周期采集"等提示。
2. **写入被拒**：即使进入标定表单，写入仍被拒绝，提示"DAQ 仅支持 MEASUREMENT ｜ 可写标量候选"。

源码核查后确认为**两个独立缺陷**，需分别修复：

### 缺陷一：标定准入错误地耦合了 DAQ 运行状态

`tools/xcp_master/main.cpp` 中，标定入口的可见性条件里混入了 DAQ 相关判断，导致 DAQ 一旦运行，标定路径即被整体关闭。标定与 DAQ 本应是**正交能力**——标定走静态内存写通路，DAQ 走周期采样通路，二者在 XCP 协议层互不排斥。

### 缺陷二：A2L 未给段相对寻址的标定段打 READ_WRITE

`thirdparty/XCPlite/src/a2l.c:1101-1113`（`A2lCreateInstance_`）的条件是**逐位白名单枚举**：

```c
if (
#ifdef XCP_ENABLE_ABS_ADDRESSING
    XcpAddrIsAbs(addr_ext) ||
#endif
#ifdef XCP_ENABLE_APP_ADDRESSING
    XcpAddrIsApp(addr_ext) ||
#endif
#ifdef XCP_ENABLE_DYN_ADDRESSING
    XcpAddrIsDyn(addr_ext) ||
#endif
    false) { // Measurements in absolute and dynamic mode allows write access
    fprintf(gA2lFile, " READ_WRITE");
}
```

**该白名单不含 `XcpAddrIsSeg`。** 而当前默认构建为 `XCPLITE__CASDD`（`thirdparty/XCPlite/src/xcp_cfg.h:159-167`，`OPTION_CAL_SEGMENTS_ABS` 未定义时的默认分支）：

```c
#define XCP_ENABLE_SEG_ADDRESSING
#define XCP_ADDR_EXT_SEG 0x00
```

标定段走 SEG 扩展（`0x00`），恰好落在被漏掉的那一位，于是 A2L 中的 `cal` 实例**缺少 `READ_WRITE`**，主站据此认为该变量不可写（`SymbolWriteUnsupportedReason` 返回"A2L 未标记为可写"），静态标定资格被拒。

**关键事实：SEG 段物理上完全可写。** `thirdparty/XCPlite/src/xcplite.c` 为 SEG 开了专门读写分支：

- `xcplite.c:509-518`（写）：

```c
#ifdef XCP_ENABLE_SEG_ADDRESSING
    // EXT == XCP_ADDR_EXT_SEG calibration segment memory access
    if (XcpAddrIsSeg(local.mta_ext)) {
        uint8_t res = XcpCalSegWriteMemory(local.mta_addr, size, data);
```

- `xcplite.c:568-577`（读）：走 `XcpCalSegReadMemory`，结构对称。

因此 `a2l.c` 未打标记属于**实现遗漏**，而非"该段不可写"的设计。已有跨进程测试 `tests/xcp_slave_demo_integration_test.cpp:1041` `CrossProcessCalibrationChangesDaqOutput` 在本次改动前即通过，实证 SEG 写入真实生效。

## 修改范围

- `tools/xcp_master/tui_model.hpp`：新增纯函数 `CalibrationCommandPermitted(bool terminal_too_small, VariableModalPage page, bool has_symbol, const std::string& policy_reason)`，把标定准入判定从 `main.cpp` 的控件可见性条件中提取为可单测的独立谓词。**签名中不含 DAQ 参数**——这一刻意设计使"把 DAQ 状态重新耦合进标定准入"必须修改签名，从而被测试立即发现。
- `tools/xcp_master/main.cpp`：三处标定控件可见性条件改为调用上述谓词（纯提取，行为不变）；修正标定相关显示文案，消除"不适用周期采集"/"可标定（可标定表单读写）"等歧义表述。
- `examples/xcp_slave_demo/main.cpp`：新增 `PatchCalibrationInstanceWriteAccess`，在生成模式下为独立标定段的 `INSTANCE` 声明补齐 `READ_WRITE`。仅对**唯一**同时满足「名字 `cal` + 类型 `CalibrationParameters` + 描述匹配」的声明做局部补齐，定位不唯一即失败返回，绝不改动 `meas_*` 或其他段。函数幂等：已带 `READ_WRITE` 视为命中。
- `tests/xcp_master_tui_model_test.cpp`：新增 6 项准入谓词回归，覆盖终端过小、非标定页、无符号、策略拒绝、以及 DAQ 运行状态下标定仍然被允许。
- `tests/xcp_slave_demo_integration_test.cpp`：在 `GeneratedA2lPropagatesFixedEventsToMeasurementLeaves` 中补充两条断言，钉死 `cal.basic.real_32` 的 `read_write == true` 与 `SymbolWriteUnsupportedReason` 为空。
- `tests/a2l_golden_test.cpp`：仅补充注释，说明该语料保持 `rw=0` 的理由及与示例的分工。

未修改 SDK、公共 API、第三方源码、按键映射，未新增对外接口。

## 双现状设计决策

`tests/a2l_golden_test.cpp:1209-1210` 对 `kDefaultCalParams.cal_factor` 断言 `rw=0`，注释写明"A2L 元数据层不伪造读写位"；而本次在 `examples/xcp_slave_demo` 中主动补齐该位。二者看似矛盾，实为**同一事实的两种应对**：

| | 职责 | 应对 |
|---|---|---|
| `tests/a2l_golden_test.cpp` | 对上游输出的**忠实快照**（回归基线），记录 `xcp_test_slave` A2L 语料 | 保留 `rw=0`，不掩盖上游事实 |
| `examples/xcp_slave_demo/main.cpp` | 交付给主站使用的**示例程序**与端到端验证载体 | 主动补齐，纠正虚假的能力缺失 |

经用户确认（保留双现状），在两处注释中互相引用说明差异理由，未改动任何断言值。**不建议**将 `xcp_test_slave` 也改为补标记：那会改变既有 42 符号基线语料的语义，波及 `A2lGolden` 大量断言，代价大而收益不明。

## 验证

### 构建

`cmake --build cmake-build-logging-release --target XcpliteIntegration A2lGolden XcpMasterSymbolPolicyTest -j 4`：AppleClang Release 构建通过。

### 回归结果

| 套件 | 结果 |
|---|---|
| `A2lSmoke` | 111 checks, 0 failures |
| `A2lE2E` | 10/10 通过 |
| `A2lLogTimestampTest` | 2/2 通过 |
| `MeasurementAdapterTest` | 16/16 通过 |
| `A2lGolden` | 40 通过，2 跳过（`ParseLargeFileUnderBudget`、`Utf16LeBomExploratory`，均为既有跳过项） |
| `XcpMasterSymbolPolicyTest` | 74/74 通过（原 68 + 新增 6 项准入谓词回归） |
| `XcpliteIntegration` | 见下方全量回归 |
| `libxcp_tests` | 1 项失败，**与本改动无关**，见下 |

### 变异测试：证明新增断言有效

为确认 `tests/xcp_slave_demo_integration_test.cpp:899-902` 的新增断言**真的能捕获回归**，做了两次注入式变异：

1. **第一次（设计错误，已作废）**：只注入"跳过示例自校验"，补丁仍然生效，故断言如期通过——这次变异**方向错了**，无法证明任何事。
2. **第二次（有效）**：同时注入"禁用补丁本体 + 跳过自校验"，使 A2L 真正缺失 `READ_WRITE`，结果两条断言**精确失败在预期位置**：

```
tests/xcp_slave_demo_integration_test.cpp:899: Failure
标定叶子缺少 A2L READ_WRITE 权限
tests/xcp_slave_demo_integration_test.cpp:900: Failure
标定叶子未通过写入策略：A2L 未标记为可写
```

恢复文件后断言重新通过。这证明断言**在 A2L 权限一旦退化时必然报警**，而非恒真。

变异代码已完全清除，`git diff` 中无任何残留（`grep "变异测试临时改动"` 无输出）。

## 未覆盖与既有问题

### 既有缺陷（非本改动引入，未修复）

`libxcp_tests` 中 `UdpTransportClose.CloseWhileBlockedReturnsPromptly` **稳定失败**。核查结论：

- 该测试文件与其依赖**均未被本次改动触及**（`git diff --name-only` 无 `udp_transport`）。
- 失败根因是**测试自身的边界疏漏**。`tests/udp_transport_test.cpp:868-878`：

```cpp
cfg.receive_poll_interval_ms = 1000;  // 放大轮询周期，确认 Close 最迟在一个轮询周期内退出
transport.Close();
EXPECT_LT(elapsed.count(), 1000)      // 严格小于
```

注释声明"最迟在一个轮询周期内退出"（即 ≤1000ms），断言却写严格小于 `<1000`。当 `Close()` 恰好耗时一个完整轮询周期时断言必然失败，获得 `actual: 1000 vs 1000`。

- 连跑 5 次：4 次失败、1 次通过；唯一通过那次耗时 1016ms，**反而超过 1000ms 阈值**，说明该用例的计时方式本身不稳定（断言只计 `Close()`，实测耗时受环境调度影响）。
- 属**确定性时序边界缺陷**，与本次修复正交。按 AGENTS.md"针对已完成代码不要重构、无明确指示不添加接口"，本轮**不修改**，仅记录。

### 未覆盖

本轮未用真实终端手工复验 TUI 标定交互（此前跨进程 TUI 验证因沙箱禁 pty 受阻，用户已改选手动验证并确认 `find cal` 可列出 `cal.*`、共 13 个变量可标定）。未验证跨平台构建。