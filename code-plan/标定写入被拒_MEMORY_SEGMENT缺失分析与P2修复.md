# 标定写入被拒：A2L 缺少 MEMORY_SEGMENT 的分析与 P2 修复

> 状态：分析已完成（探针实测），**P2 修复已实施**（见 §6 实施结果）。
> 报障来源：`apa_complex::algo_status_info.apa_fusion_status.period_switch` 的「独立标定流程」页显示
> 「写入资格：拒绝：无法唯一关联有效的 XCP MEMORY_SEGMENT/PAG」。

## 0. 结论速览

1. **不是链路问题，是 libxcp 的写入前置条件过严**。该 ECU 可正常读、页机制可用、`DOWNLOAD` 被接受。
2. 直接原因是这份 A2L **一个 `MEMORY_SEGMENT` 都没有**，`SymbolInfo::memory_segment` 全为空，
   `tools/xcp_master/symbol_policy.hpp:174` 于是对**全部 2558 个 CHARACTERISTIC** 返回同一句话——
   不是"有些变量"，是这份 A2L 的标定量**一个都写不了**。
3. 即使放宽第一层，仍会被第三层拦住：运行时强制 `GET_PAGE_INFO`，而该 ECU 明确不支持该命令，
   且 A2L 的 `OPTIONAL_CMD` 里**本来就没声明**它。
4. 已批准并实施 **P2**：按 A2L `OPTIONAL_CMD` 决定是否查询页命令；无段页事实时按单页处理；
   仅在活动页非 0（疑似参考页）时才拒绝。

## 1. 报障与现场

用户截图：`独立标定流程 · apa_complex::algo_status_info.apa_fusion_status.period_switch`，
类别 `CHARACTERISTIC/VALUE`、基础类型 `UBYTE`、当前值 `1`、允许范围 `0 … 1`、状态 `POLL`，
写入资格 `拒绝：无法唯一关联有效的 XCP MEMORY_SEGMENT/PAG`。

注意：**能读、能 POLL**，说明连接、寻址、A2L 解析都正常，问题只在写资格判定。

## 2. 探针实测

### 2.1 静态事实（`apa_complex.a2l`，91 245 118 字节）

| 项目 | 数量 |
| --- | --- |
| `/begin MEASUREMENT` | 195 998 |
| `/begin CHARACTERISTIC` | 2 558 |
| `/begin MEMORY_SEGMENT` | **0** |
| `/begin SEGMENT` | **0** |
| `/begin PAGE` | **0** |
| `/begin IF_DATA` | 196 827 |
| 含 `READ_WRITE` 的行 | 195 998（**全部是 MEASUREMENT**，CHARACTERISTIC 一个都没有） |

`PROTOCOL_LAYER`：`0x102 … 248 1024 BYTE_ORDER_MSB_LAST ADDRESS_GRANULARITY_BYTE`；
`XCP_ON_TCP_IP`：`0x104 5559 ADDRESS "192.168.2.137"`；
`OPTIONAL_CMD` 含 `DOWNLOAD SHORT_DOWNLOAD GET_CAL_PAGE SET_CAL_PAGE COPY_CAL_PAGE`，
**不含** `GET_PAGE_INFO` / `GET_SEGMENT_INFO` / `GET_PAG_PROCESSOR_INFO`。

### 2.2 静态判定复现（`.tmp/probe/cal_probe`，直接调 `symbol_policy.hpp`）

```
module=apa_complex name=algo_status_info.apa_fusion_status.period_switch
addr=0x526CBF80 ext=1 size=1 read_write=true
memory_segment = <缺失>
[WriteUnsupported] ''
[StaticWriteUnsupported] '无法唯一关联有效的XCP MEMORY_SEGMENT/PAG'
```

`[WriteUnsupported]` 为空 → `read_write` 这一关是**过**的，卡在段页关。

### 2.3 运行时能力与可写性（`.tmp/probe/ecu_probe`，目标 `192.168.2.137:5559`）

```
CONNECT 成功：PL=1.1 TL=1.1 MAX_CTO=248 MAX_DTO=1024 resource=0x05
① SHORT_UPLOAD 读取成功：1 字节 = 0x01
② GET_PAG_PROCESSOR_INFO：nullopt（Slave 回 ERR_CMD_UNKNOWN）
③ GET_CAL_PAGE(Ecu)       ：segment0 当前 page=0
③ GET_CAL_PAGE(Xcp)       ：segment0 当前 page=0
④ GET_SEGMENT_INFO(mode0,sel=0)：nullopt（ERR_CMD_UNKNOWN）
⑤ GET_PAGE_INFO(seg0,page0)：nullopt（ERR_CMD_UNKNOWN）
⑥ 回写前： 0x01
⑥ SHORT_DOWNLOAD/N_DOWNLOAD 同值写入：ECU 已接受（无异常）
⑥ 回写后： 0x01（与原值一致=是）
```

要点：
- `resource=0x05` → CAL/PAG 资源**已声明**，页机制存在（`GET_CAL_PAGE` 真的返回了 page=0）。
- `DOWNLOAD` 被接受（同值回写 0x01→0x01，值未改变，语义无影响）。
- 三个"信息查询类"页命令（`GET_PAG_PROCESSOR_INFO`/`GET_SEGMENT_INFO`/`GET_PAGE_INFO`）全部 `ERR_CMD_UNKNOWN`。

## 3. 三层卡点（代码定位）

| 层 | 位置 | 判定 | 本 ECU |
| --- | --- | --- | --- |
| 第一层 静态段页 | `tools/xcp_master/symbol_policy.hpp:172-174` | `!memory_segment.has_value() \|\| !has_xcp_segment` → 拒绝 | **卡住** |
| 第二层 活动页 | `tools/xcp_master/main.cpp:2495-2497` | `GetCalPage` 返回 `nullopt` → 「无法查询XCP活动页」 | 通过（page=0） |
| 第三层 页权限 | `tools/xcp_master/main.cpp:2498-2501` | `GetPageInfo` 返回 `nullopt` → 「无法查询XCP PAGE_PROPERTIES」 | **必卡**（命令不支持） |

另有重复实现：`tools/xcp_master/main.cpp:1756-1765`（界面「写入资格」行）内联了与第一层相同的判定。

## 4. 为什么这是缺陷

1. **段页是可选能力**。XCP 写标定量只需要 `DOWNLOAD`/`SHORT_DOWNLOAD` + 地址(+扩展)；
   `MEMORY_SEGMENT`/`PAGE` 只与标定页机制（`SET_CAL_PAGE`/`COPY_CAL_PAGE`）有关。
   把"必须有段页声明"当前置条件，等于把不用页机制的 ECU 全部判成不可标定。
2. **无视 A2L 的 `OPTIONAL_CMD` 声明**。A2L 明说"我没有 `GET_PAGE_INFO`"，工具仍每次写入都去问，
   拿 `ERR_CMD_UNKNOWN` 后判"拒绝"。应当以声明为准，而不是靠失败来推断。
3. **`CALIBRATION_ACCESS` 被整条忽略**（独立缺陷，**已于后续批次修复**，见 §8 与
   `code-plan/标定可写性_CALIBRATION_ACCESS参与判定_实施记录.md`）：`thirdparty/a2l-sdk/src/liba2l_export.cpp:839`
   的 `s.read_write = !c.ReadOnly();` 只看已废弃的 `READ_ONLY`，不看 ASAP2 V1.4 起取而代之的
   `CALIBRATION_ACCESS`。于是 `CALIBRATION_ACCESS NO_CALIBRATION` / `OFFLINE_CALIBRATION`
   的标定量仍被判为可写。

   > 更正：本文早期版本称「ASAP2 缺省是 READ_ONLY，缺省即可写属解析缺陷」。核对
   > `docs/ASAM_AE_MCD-2_MC_BS_V1-7-0.pdf` §4.7.32（CHARACTERISTIC 原型只允许 `READ_ONLY`，
   > 无 `READ_WRITE`）后确认：CHARACTERISTIC 缺省**本就是可写**，`!c.ReadOnly()` 符合规范；
   > 真正的问题是只认 `READ_ONLY`、不认 `CALIBRATION_ACCESS`。
   >
   > 用户裁决（2026-10-24）：解析器缺省值**保持现状、不改**；复核记录见
   > `code-plan/标定可写性_CALIBRATION_ACCESS参与判定_实施记录.md` §4.1。

## 5. P2 方案（已实施，见 §6）

### 5.1 语义表

| A2L 段页事实 | A2L `GET_CAL_PAGE` | A2L `GET_PAGE_INFO` | 判定 |
| --- | --- | --- | --- |
| 有（`memory_segment` + `has_xcp_segment`） | 任意 | 声明 | 严格校验（现状不变，含静态 PAGE 声明 + 运行时 PAGE_PROPERTIES） |
| 有 | 任意 | 未声明 | 只查活动页；页 0 → 放行并记注；页非 0 → 拒绝 |
| 无 | 未声明 | 未声明 | 直接放行并记注「A2L 未声明段页/页命令，按单页写入」 |
| 无 | 声明 | 未声明 | 查活动页；`nullopt` → 放行并记注；页 0 → 放行并记注；页非 0 → 拒绝并提示切页 |
| 无 | 任意 | 声明 | 查活动页 + `GET_PAGE_INFO`，按返回的 `xcp_write_access` 严格判定 |

### 5.2 「活动页 0 = 工作页」的依据与边界

- 依据：`thirdparty/XCPliteV1.2/src/xcpLite.h:468-469`
  `XCP_CALPAGE_DEFAULT_PAGE 1 // FLASH page`、`XCP_CALPAGE_WORKING_PAGE 0 // RAM page`。
- 边界：XCP 标准**不定义页号语义**，0=工作页只是 XCPlite 的约定。因此该推断只用于"放行"，
  且必须在界面注里写明「未取证」，不得据此拒绝**页 0** 的写入，但可以拒绝**页非 0**（保守方向，
  失败方向安全：最多要求用户显式切页）。ECU 侧仍有 `ERR_WRITE_PROTECTED` 兜底。

### 5.3 改动清单

1. `tools/xcp_master/symbol_policy.hpp`
   - 新增 `struct CalibrationPageFacts`：`declares_get_cal_page` / `declares_get_page_info` / `declares_set_cal_page`。
   - 新增 `MakeCalibrationPageFacts(const std::vector<std::uint8_t>&)`：从 A2L `OPTIONAL_CMD` 命令码构造。
   - 新增 `HasCalibrationPageFacts(const SymbolInfo&)`：段页事实是否可用。
   - 新增 `CalibrationPageNote(const SymbolInfo&, const CalibrationPageFacts&)`：界面注文案（无事实/无权限查询时非空）。
   - 放宽 `SymbolStaticWriteUnsupportedReason`：无段页事实时**不再拒绝**（交运行时判定）。
2. `tools/xcp_master/main.cpp`
   - `CliApp` 构造多收一个 `CalibrationPageFacts`，在 `main()` 里从 `bridge->XcpInfo()->protocol_layer.optional_commands` 构造。
   - `ModalWriteUnsupportedReason()` 改用统一策略函数；缺段页事实时返回空并附注。
   - `CheckCalibrationPageWriteLocked()` 按 §5.1 重写。
   - 「写入资格」行下新增「页判定依据：…」行（`ModalCalibrationPageNote()`），不用它污染拒绝文案。
3. `tests/xcp_master_symbol_policy_test.cpp`：新增用例覆盖 §5.1 的五种组合与命令码解析。

### 5.4 不改的

- `thirdparty/a2llib` 的 `read_write` 缺省语义（独立缺陷，需单独批准）。
- `GET_SEGMENT_INFO`/`GET_PAG_PROCESSOR_INFO` 的使用（本仓当前不依赖）。
- ECU 侧写保护兜底逻辑（保持接收 `ERR_WRITE_PROTECTED` 的原路径）。

## 6. 实施结果

### 6.1 改动落点（行号为改动后实测）

| 文件 | 位置 | 内容 |
| --- | --- | --- |
| `tools/xcp_master/symbol_policy.hpp` | `:157` | `struct CalibrationPageFacts`：`declares_get_cal_page` / `declares_set_cal_page` / `declares_get_page_info` |
| 同上 | `:164` | `A2lDeclaresCommand(const std::vector<std::uint8_t>&, CommandCode)`：在 OPTIONAL_CMD 命令码里查表 |
| 同上 | `:172` | `MakeCalibrationPageFacts(optional_commands)`：一次解析出三个能力位 |
| 同上 | `:185` | `HasCalibrationPageFacts(symbol)`：`memory_segment.has_value() && has_xcp_segment` |
| 同上 | `:191` | `CalibrationPageNote(symbol, facts)`：三种附注文案 |
| 同上 | `:218` | `SymbolStaticWriteUnsupportedReason()` 的段页关：无事实时 `return {}`（放行） |
| `tools/xcp_master/main.cpp` | `:435` | `CliApp` 构造新增末参 `CalibrationPageFacts`（默认 `{}`，不影响其它测试调用） |
| 同上 | `:3279` | 成员 `calibration_page_facts_` |
| 同上 | `:3635-3641` | `main()` 从 `bridge->XcpInfo()->protocol_layer.optional_commands` 构造并传入 |
| 同上 | `:1763-1765` | `ModalWriteUnsupportedReason()`：无段页事实时放行（返回值语义仍是「空=可写」） |
| 同上 | `:1779` | `ModalCalibrationPageNote()`：附注访问器 |
| 同上 | `:673-677` | 标定模态新增「页判定依据：…」行（灰色） |
| 同上 | `:2508-2543` | `CheckCalibrationPageWriteLocked()` 按 §5.1 重写 |

`OPTIONAL_CMD` 无需改三方库：`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp:82`
已暴露 `ProtocolLayerInfo::optional_commands`，`a2l_dto_map.cpp:451` 已从 DTO 映射。

### 6.2 关键实现取舍

1. **无段页事实 + 无页命令声明 → 一次网络往返都不做**，直接放行。理由：Slave 已经通过
   `OPTIONAL_CMD` 声明"我没有页命令"，再去试探只会每次写入多两个 `ERR_CMD_UNKNOWN` 往返。
2. **有段页事实仍保持严格**：静态 PAGE 声明 + `GET_CAL_PAGE` + `GET_PAGE_INFO` 三关不变，
   避免把 XCPlite/`xcp_slave_demo_v12` 这类真声明了双页的从站放开写保护。
3. **无段页事实且声明了 `GET_CAL_PAGE`**：查一次活动页；`nullopt`（声明与实际不符）或页 0 放行，
   页非 0 拒绝并提示切页——保守方向是"可能多要求一次切页"，不会写错页。
4. **`CalibrationPageNote` 不参与拒绝判定**：只做展示。曾误把它拼进 `ModalWriteUnsupportedReason()`
   的返回值（`"放行（…）"`），会使调用方把非空串当拒绝，已改回空串并单独成行。

### 6.3 客户 A2L 实测（端到端取数）

`.tmp/probe/optional_probe ~/Downloads/apa_complex.a2l`：

```
optional_commands=28
declares_get_cal_page=1 declares_set_cal_page=1 declares_get_page_info=0
```

→ 命中 §5.1 第 4 行：无段页事实 + 声明 `GET_CAL_PAGE` + 未声明 `GET_PAGE_INFO`。
再结合此前 `ecu_probe` 的实测（`GET_CAL_PAGE` 返回 `segment0 page=0`、`DOWNLOAD` 被接受），
`period_switch` 的判定链变为：静态放行 → 查活动页得 0 → 放行。

### 6.4 测试

| 项 | 结果 |
| --- | --- |
| `XcpMasterSymbolPolicyTest` 全量 | **106 passed**（原 102，新增 4） |
| 新增用例 | `RelaxesStaticWriteGateWithoutMemorySegment`、`KeepsStaticWriteGateWithReadOnlyPages`、`ParsesCalibrationPageFactsFromOptionalCommands`、`ExplainsCalibrationPageDecisionBasis` |
| 修改用例 | `StaticWritePolicyUsesOnlyA2lMetadata`：原断言"无 MEMORY_SEGMENT 必须报错"，改为断言放行（P2 故意反转的契约） |
| `ctest -E "^Example" -j 4` | **962 项 / 0 failed**（2 项为既有 Skip） |
| `ctest -R "^Example" -j 1` | 13/14 通过；唯一失败 `ExampleXcpIntegrationDemoCppDemo` 为既有问题 |

**注意（环境陷阱）**：`ctest -j 4` 跑全量时 5 个 Example 用例里有 4 个报
「端口 5555 已被占用（疑似已有 XCP slave 在跑）」——它们是并行抢占同一个默认端口，
串行重跑即全部通过，与本次改动无关。

### 6.5 未验证项

- **交互界面未人工验证**：`xcp_master` 需要真实 TTY，本环境 `pty.openpty()` 报
  `out of pty devices`。建议在带端子的机器上复核「页判定依据：A2L 未声明 MEMORY_SEGMENT：
  按 0 号段查询活动页，活动页 0 视为工作页」是否显示，并确认 `period_switch` 的
  「写入资格」变为可写、`申请写入` 能走到确认页。
- **客户 ECU 上未做真实标定写入**：`ecu_probe` 已证明 `DOWNLOAD` 同值回写被接受，
  但走完整工具链（含页查询与确认页）写入仍未执行。

### 6.6 未做项

- `CALIBRATION_ACCESS` 未参与可写性判定（§8）——**已在后续批次修复**，见
  `code-plan/标定可写性_CALIBRATION_ACCESS参与判定_实施记录.md`。
- `GET_SEGMENT_INFO` / `GET_PAG_PROCESSOR_INFO` 的声明驱动查询。

## 7. 复现命令

```bash
# 静态判定
.tmp/probe/cal_probe ~/Downloads/apa_complex.a2l \
  "apa_complex::algo_status_info.apa_fusion_status.period_switch"
# 运行时能力（只读 + 同值回写；第 ⑥ 步会向 ECU 写回读到的同一字节）
.tmp/probe/ecu_probe 192.168.2.137 5559 1 526CBF80 1
# 可写性语义对照（.tmp/probe/rwcase2.a2l 含四种声明）
.tmp/probe/rw_probe .tmp/probe/rwcase2.a2l \
  m::c_plain m::c_readonly m::c_nocal m::c_offline
```

## 8. 附带发现（已修复）：`CALIBRATION_ACCESS` 被忽略

> 状态更新：本条已在后续批次按方案 A 修复，改动与验证见
> `code-plan/标定可写性_CALIBRATION_ACCESS参与判定_实施记录.md`。
> 下文保留当时的取证过程与三方案对比，作为决策依据存档。

### 8.1 规范依据（`docs/ASAM_AE_MCD-2_MC_BS_V1-7-0.pdf`）

| 位置 | 原文要点 |
| --- | --- |
| §4.7.32 CHARACTERISTIC（p.73） | 原型里可选属性**只有** `[-> READ_ONLY]`，没有 `READ_WRITE` |
| §4.7.85 INSTANCE（p.145） | 可选属性为 `[-> CALIBRATION_ACCESS]` + `[-> READ_WRITE]`，没有 `READ_ONLY` |
| §4.7.114 READ_ONLY（p.187） | “indicate that an adjustable object cannot be changed (but can only be read)” |
| §4.7.115 READ_WRITE（p.188） | “mark a **measurement** object to be writeable” |
| §4.7.28 CALIBRATION_ACCESS（p.69） | `NO_CALIBRATION` = 不可改、只能读；`OFFLINE_CALIBRATION` = 可刷写但不可标定；**“It substitutes the READ_ONLY attribute since ASAM-MCD-2 MC V1.4”** |

结论：CHARACTERISTIC 的缺省是**可写**（无 `READ_ONLY` 即“可调整对象”），所以
`liba2l_export.cpp:813` 的 `!c.ReadOnly()` 本身符合规范；**真正的漏洞是只认 `READ_ONLY`、
不认自 V1.4 起取代它的 `CALIBRATION_ACCESS`**。

### 8.2 实测（`.tmp/probe/rwcase2.a2l` + `.tmp/probe/rw_probe`）

| 声明 | 规范语义 | libxcp 实测 `read_write` |
| --- | --- | --- |
| 无访问关键字 | 可写 | `1` ✅ |
| `READ_ONLY` | 只读 | `0` ✅ |
| `CALIBRATION_ACCESS NO_CALIBRATION` | 只读 | `1` ❌ |
| `CALIBRATION_ACCESS OFFLINE_CALIBRATION` | 不可在线标定 | `1` ❌ |
| CHARACTERISTIC 内写 `READ_WRITE` | 规范不允许 | 整份 A2L **解析失败**（`Parser error: syntax error ... Near: READ_WRITE`） |

### 8.3 代码病灶

- `thirdparty/a2llib/src/a2lparser.y:661` 把 `CALIBRATION_ACCESS` 存进 Characteristic，`:866` 存进 Instance；
- `thirdparty/a2llib/include/a2l/a2lenums.h:50-56` 提供 `CALIBRATION / NO_CALIBRATION /
  NOT_IN_MCD_SYSTEM / OFFLINE_CALIBRATION / UNKNOWN`；
- **但**全仓 `tools/ include/ src/ tests/ thirdparty/a2l-sdk/` 对 `CalibrationAccess` 零引用
  （`grep -rn` 无命中）：这个字段被解析后直接丢弃，桥接层 `SymbolInfo` 也没有对应字段。

### 8.4 影响与风险

- 现在没出事，只是因为手头的 A2L（客户 `apa_complex`、XCPlite V1.2/V2.2 生成的）**都不写
  `CALIBRATION_ACCESS`**（三份文件计数均为 0）。用 Vector/ETAS 等按新规范导出的 A2L 就会踩到：
  标定工程师在界面上看到“可写”，随后 ECU 回 `ERR_WRITE_PROTECTED`，或（更糟）真写进去。
- 反向风险也存在：CHARACTERISTIC 里写 `READ_WRITE` 的生成器会让**整份** A2L 加载失败
  （a2llib 是严格文法，没有容错分支）。

### 8.5 修复建议（需批准，本次未实施）

| 档 | 改动 | 影响面 | 风险 |
| --- | --- | --- | --- |
| A（推荐） | `liba2l_export.cpp:813` 改为按 `CALIBRATION_ACCESS` 判定：`UNKNOWN`/`CALIBRATION` → 沿用 `!ReadOnly()`；`NO_CALIBRATION`/`OFFLINE_CALIBRATION`/`NOT_IN_MCD_SYSTEM` → 不可写。`:898` 的 INSTANCE 同理 | a2l-sdk 一处 + 桥接层注释；不动 a2llib | 只在 A2L 真的声明了 `CALIBRATION_ACCESS` 时改变行为，现有样本全部不受影响 |
| B | 额外把 `CALIBRATION_ACCESS` 透到 `SymbolInfo`，由 `symbol_policy.hpp` 决定拒绝文案（如「A2L CALIBRATION_ACCESS=NO_CALIBRATION，禁止写入」） | 加 ABI 字段（需 ABI 版本 +1） | 破坏性 ABI 变更，收益仅是文案更准 |
| C | 让 a2llib 对 CHARACTERISTIC 里的 `READ_WRITE` 容错（当作可写并记告警） | 改文法/加错误恢复 | 与规范相悖；建议只在“整份文件解析失败”成为现实问题后再做 |

建议先做 A：一处判定、零 ABI 变更、对现有 A2L 行为完全中性。