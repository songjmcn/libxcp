# XCP 1.3.0 审核记录（批次 4 前置）：struct / class 成员命名分类是否彻底

> ## ✅ 处置结果（批次 4 已按本记录收口文档）
>
> 本记录正文（§1–§5）描述的是**改动前**的状态，作为审核证据原样保留；实际处置见
> `code-plan/XCP_1.3.0_实施记录_批次4_命名规则按struct与class分流.md`。采纳裁决：
> **D1 按 `struct`/`class` 关键字判定**、**D3 本批次只改文档**（代码分批改）、
> **D5 审计脚本移到 `scripts/`**（复核时发现 `.gitignore` 已含 `scripts` 条目，故**实际不入库**，
> 已按"本地工具 + 文档写明复核锚点"处理）；默认项 D2′（§10 补 `raw_error_code`/`raw_event_code`）、
> D2（夹具成员保留豁免）一并执行。
>
> 处置后核对（同一检测器，自检 5/5）：文档侧 `struct` 成员 **64 字段全部裸名**、
> `m_<snake>_` 归零；`class` 成员 59 字段仍全部 `m_<snake>_`、未受影响；
> 产品/测试代码 `git diff` 为空，Release 构建零警告、253 项测试全绿。
> **§1 表中"代码 0 处改动"由此从"缺口"变为"经批准的预期中间态"**，量化待办
> （A 18 字段/189 处、B 39 字段/377 处、class 44 处）已登记在设计文档 §2.2.1.3。
>
> 📌 **后续更正指引（批次 5 收尾复查）**：上段的 64 / 59 / "自检 5/5" 与 "class 44 处"
> 均由**当时的检测器**测得，该检测器后经证实有 4 个盲区（漏记 `private:` 后首条成员等），
> 修正后文档侧为 **130 条 = struct 64 + class 66**，class 侧待改为 **47 条声明**。
> 本文作为历史记录保留原文不改，准确口径以设计文档 §2.2.1.3 与
> `XCP_1.3.0_实施记录_批次5_成员命名规则落地到代码.md` §3.7 为准。
>
> 记录内 `L145`、`L883` 等行号指**改动前**版本；本批次后 §2.2.1 区新增约 47 行，
> 后续定位请优先使用章节号。

- **审核对象**：用户对 `code-plan/XCP_1.3.0_详细设计_类接口与头文件.md` 的未提交修改
  （`git diff --numstat` = 19 增 / 19 删，改动集中在 §7 与 §10 两个 `cpp` 块）
- **新规则**：`struct` 成员用可读裸名（`remote_host`），**不再**用 `m_xxx_`；`class` 成员保持 `m_<snake>_`
- **本轮性质**：纯审核，**未改动任何产品代码、未改动设计文档**；仅新增 3 个可复跑的审计脚本
- **日期**：2026-02-10

---

## 1. 结论

**不彻底，且当前处于"文档自相矛盾 + 文档与代码矛盾"的状态。** 缺口分三层：

| 层 | 缺口 | 严重度 |
|---|---|---|
| **规则本体** | §2.2.1 命名规则表 L145 原文未改，仍是「普通成员变量 = `m_` + 小写蛇形 + 末尾 `_`」，**完全没有 struct/class 分流**。新裁决目前只以两个示例代码块的形式存在，不成其为规则 | 🔴 阻断 |
| **文档内部** | 文档共 17 个 struct / 62 个字段：仅 **7 个 struct / 21 字段**已是裸名，**10 个 struct / 41 字段**仍是 `m_<snake>_`；另有 4 处旧名残留散在注释、§18.3、附录 B.1 | 🟠 高 |
| **文档 ↔ 代码** | **代码 0 处改动**。被改的 6 个 struct 在头文件里仍全是 `m_` 前缀（A 类，18 标识符 / 189 处引用）；其余 9 个 struct 文档与代码都还是 `m_`（B 类，39 标识符 / 377 处引用） | 🔴 阻断 |

一句话：**改的是两个示例，没改规则，也没改代码。**

---

## 2. 逐结构体清单（扫描器实测，非目测）

### 2.1 文档侧：17 个 struct

| struct | 文档章节 | 文档字段风格 | 代码字段风格 | 判定 |
|---|---|---|---|---|
| `UdpTransportConfig` | §7 | ✅ 8 裸名 | ❌ 8 个 `m_` | **A 冲突** |
| `PositiveResponse` | §10 | ✅ 2 裸名 | ❌ 2 个 `m_` | **A 冲突** |
| `NegativeResponse` | §10 | ✅ 2 裸名 | ❌ 3 个 `m_` | **A 冲突 + 文档少 1 字段** |
| `EventPacket` | §10 | ✅ 2 裸名 | ❌ 3 个 `m_` | **A 冲突 + 文档少 1 字段** |
| `ServicePacket` | §10 | ✅ 2 裸名 | ❌ 2 个 `m_` | **A 冲突** |
| `DtoPacket` | §10 | ✅ 2 裸名 | ❌ 2 个 `m_` | **A 冲突** |
| `CommandTimeouts` | §12 | ✅ 3 裸名 | ✅ 3 裸名 | 一致（上批次"豁免"，新规则下转为正常合规） |
| `ConnectResponse` | §3.9 | ❌ 9 个 `m_` | ❌ 9 个 `m_` | B 待办 |
| `GetStatusResponse` | §3.9 | ❌ 8 | ❌ 8 | B 待办 |
| `GetCommModeInfoResponse` | §3.9 | ❌ 6 | ❌ 6 | B 待办 |
| `SessionParameters` | §3.11 | ❌ 4 | ❌ 4 | B 待办 |
| `XcpAddress40` | §3.10 | ❌ 2 | ❌ 2 | B 待办，⚠️ **唯一带成员函数的 struct**（`Advance()`，`protocol_types.hpp:292`），见 D1 |
| `UdpHeader` | §6 | ❌ 2 | ❌ 2 | B 待办 |
| `UdpFrame` | §6 | ❌ 1 | ❌ 1 | B 待办 |
| `UdpFrameView` | §6 | ❌ 2 | ❌ 2 | B 待办 |
| `FaultInjection` | §15.2 | ❌ 6 | ❌ 6 | B 待办（测试设施） |
| `CommandResult` | §12 | ❌ 1 | —（未实现） | C 仅文档，改名零成本 |

> **附带发现的既有偏差（与命名无关，但在同一块内）**：
> 代码 `NegativeResponse` 有 `m_raw_error_code_`、`EventPacket` 有 `m_raw_event_code_`
> （批次 2 为"保留 Slave 返回的未知错误码原值"引入），文档 §10 **缺这两个字段**。
> 本轮若只改名不补字段，§10 会看起来一致、实际仍少两个字段。

### 2.2 代码侧：class 成员（新规则要求保持 `m_<snake>_`）

| 分类 | 数量 | 说明 |
|---|---|---|
| ✅ `m_<snake>_` 合规 | 68 | **产品 class 与测试 class 均合规**：产品侧 `CommandExecutor` 17、`UdpTransport` 10、`Session` 4、`XcpException` 4、`XcpMaster` 3、`MemoryAccess` 1；测试侧 `UdpTestSlave` 16、`MockTransport` 5、`RecordingListener` 5、`RawEndpoint` 1、`RawSender` 1 |
| ✅ `s_<snake>_` 静态成员 | 3 | `WinsockSession`，合规 |
| ❌ `m_<snake>`（缺尾下划线） | 44 | **全部在 tests/**：`FakeSlave` 13、`MockXcpSlave` 13、`EventRecorder` 6、`TransportObserver` 4、`ScriptedSlave` 5、`RecordingEvents` 3。示例 `m_ag`、`m_max_cto`、`m_memory`——既不合 class 规则也不合 struct 裸名，是批次 1 大小写不敏感替换脚本的残留 |
| ⚠️ `<snake>_`（仅尾下划线） | 4 | 夹具 `UdpTestSlaveCommands` 的 `slave_`/`ep_`/`ctr_`/`res_`，上批次豁免条目 |

**结论**：class 侧产品代码没问题，问题集中在测试夹具类的 44 处缺尾下划线——**这批不在用户本次改动范围内，也没被文档记录为待办**。

---

## 3. 改名影响面（A/B 类，已按标识符去重）

### A 类：文档已裸名、代码仍 `m_`（18 标识符，189 处引用）

| 字段 | include | src | tests | code-plan | 合计 |
|---|---|---|---|---|---|
| `m_data_` | 4 | 29 | 28 | 1 | **62** |
| `m_local_port_` | 1 | 4 | 10 | 0 | 15 |
| `m_error_code_` | 2 | 9 | 2 | 1 | 14 |
| `m_event_code_` | 1 | 3 | 7 | 0 | 11 |
| `m_remote_host_` | 1 | 3 | 5 | 1 | 10 |
| `m_local_host_` | 1 | 4 | 3 | 1 | 9 |
| `m_receive_poll_interval_ms_` | 1 | 3 | 3 | 2 | 9 |
| `m_remote_port_` | 1 | 3 | 2 | 3 | 9 |
| `m_max_frame_packet_size_` | 1 | 5 | 2 | 0 | 8 |
| `m_max_datagram_size_` | 1 | 5 | 2 | 0 | 8 |
| `m_additional_info_` | 1 | 3 | 3 | 0 | 7 |
| `m_raw_error_code_` | 1 | 3 | 2 | 0 | 6 |
| `m_strict_remote_port_` | 1 | 1 | 3 | 1 | 6 |
| `m_command_` | 2 | 1 | 1 | 0 | 4 |
| `m_info_` / `m_pid_` / `m_service_code_` | 各 1 | 各 1 | 各 1 | 0 | 各 3 |
| `m_raw_event_code_` | 1 | 1 | 0 | 0 | 2 |

### B 类：文档与代码都还是 `m_`（39 标识符，377 处引用）

Top 若干：`m_header_` 23、`m_byte_order_` 21、`m_ctr_` 17、`m_max_cto_` 17、`m_xcp_packet_` 16、`m_connect_` 15、`m_max_dto_` 15、`m_short_upload_available_` 14、`m_comm_mode_info_` 13、`m_len_` 13、`m_status_` 12、`m_address_` 12、`m_ctr_offset_n_` 15 ……（余略）

---

## 4. 🔴 关键风险：本轮改名**禁止全局文本替换**

审计实测出两组同名冲突，若按"一条 sed 走天下"改会静默违规：

| 字段名 | 同时出现在 | 全局替换的后果 |
|---|---|---|
| `m_error_code_` | `struct NegativeResponse`（A，要转裸名）**和** `class XcpException`（class，**必须保持** `m_`） | 把 class 成员一起改名 → **违反新规则的 class 半边**，且测试照样全绿，无人发现 |
| `m_data_` | `PositiveResponse`/`ServicePacket`/`DtoPacket`（A，转裸名）**和** `UdpFrame`（B，暂未动） | B 类字段被提前改掉 → 制造新的半吊子不一致 |

⇒ 必须**按 struct 作用域（声明行号区间）精确改**，改完用 §6 的扫描器复核分类计数。

**次级撞名**：`m_max_cto_`→`max_cto`、`m_max_dto_`→`max_dto` 与既有其他 struct 成员同名，同批改没问题，但分批改时会让"新名"与"旧名"混在一处，属可读性陷阱。

---

## 5. 文档里需要一起收口的"旧裁决"（现在已是错话）

这些不是遗漏，是**被本轮改动证伪的既有断言**，不改文档就会留下假记录：

| 位置 | 原文（摘要） | 现状 |
|---|---|---|
| §2.2.1 L159-162 | 「`CommandTimeouts` 因此成为公开头文件中**唯一**不遵循 `m_<snake>_` 的结构体（**其余 7 个均遵循**）」 | ❌ 已假。文档现有 7 个 struct 用裸名、10 个用 `m_` |
| §2.2.1.1 L176 | 「`protocol_types.hpp`、`response_parser.hpp`、`udp_transport_config.hpp`… 的**全部结构体字段均为** `m_<snake>_`」 | ❌ 与改后的 §7/§10 直接冲突 |
| §2.2.1 豁免表 L155 | 把"纯值语义聚合参数对象"登记为**豁免** | 新规则下它不是豁免，是默认合规 → 该条应撤销并入主规则 |
| §2.2.1.2 L220 | 「采用'以代码现状为准，改设计文档'，**不做任何批量重命名**」 | 本轮方向已反转为"以文档新规则为准，改代码"，裁决口径需重写 |
| §7 L883 | 注释仍写「要求收到的包来自 `m_remote_port_`」 | ❌ 同块内残留旧名（相邻的 `m_remote_host_` 已改，这行漏了） |
| §18.3 L2291 | `UdpTransportConfig::m_strict_remote_port_` | ❌ 旧名残留 |
| 附录 B.1 L2363-2364 | `cfg.m_remote_host_ = "127.0.0.1";` / `cfg.m_remote_port_ = slave_port;` | ❌ 与新 §7 矛盾，示例代码不成立 |

---

## 6. 审核方法与自我纠错（防幻觉留痕）

沿用批次 3 教训：**零命中必须先证明检测器能命中已知正例**。本轮三版检测器全部被自检抓出缺陷：

1. **v1（PowerShell）作废**：条件链里 `-notmatch` 成功时会清空 `$Matches`，成员名取成空串 → 131 条全判为 `other`。
2. **v2（PowerShell）作废**：`namespace X {` 的 `{` 也被计入深度，"类体直属成员"判据只在 struct 声明行本身偶然成立一次 → `UdpTransportConfig` **命中 0**（自检当场报 FAIL，避免了又一次"零命中=通过"）。
3. **v3（Python，逐字符作用域栈）又修 4 处假判定**：
   - 花括号初始化 `m_running_{false}` 被拆开 → 假成员名 `m_running_false`；
   - `std::function<void(int)>` 这类尖括号内的 `(` 被误判为函数声明 → 漏字段；
   - `m_ag`（有 `m_`、缺尾下划线）被误归 `bare_snake` → 掩盖了 §2.2 那 44 处真实偏差；
   - `struct SocketImpl;` 前向声明被当成成员 → `UdpTransport` 多出 1 个假字段。
4. 中途一版 B 类统计我**凭记忆手写字段名**（`m_packet_`、`m_offset_`），跑出 0 命中——那是我的名单错，不是项目无此字段；已改为扫描器驱动、**零手工名单**。
5. 最终自检：`UdpTransportConfig` 8/8、`ConnectResponse` 9/9、`CommandTimeouts` 3/3、`UdpTransport` 10、`Session` 4 全部命中，5/5 OK。

可复跑脚本（已移到 `scripts/`；**经核实 `.gitignore` 含 `scripts` 条目，用户裁决为不入库**，见 §2.2.1.2 末"工具可得性说明"）：

| 脚本 | 用途 | 是否入库 |
|---|---|---|
| `scripts/tools_audit_member_naming.py` | struct/class 成员风格审计，内置 5 条自检 | ❌ 本地工具 |
| `scripts/tools_audit_rename_churn.py` | 改名引用面统计 + 跨类同名冲突 + 新名撞名检查 | ❌ 本地工具 |
| `scripts/tools_audit_diff.py` | 文档 vs 代码逐 struct 字段集合对比 | ❌ 本地工具 |

> 原第 4 个脚本 `check_struct_methods.py`（论证"哪些 struct 带成员函数"）已删除：其结论已固化进
> 设计文档 §2.2.1.1 并改附**可人工核对的 `文件:行号` 锚点**，不再依赖一个输出含假阳性的启发式脚本。

复跑命令（须在仓库根目录执行；脚本按自身位置解析依赖，不写死绝对路径）：
```bash
python scripts/tools_audit_member_naming.py doc code-plan/XCP_1.3.0_详细设计_类接口与头文件.md
python scripts/tools_audit_member_naming.py code include src tests
python scripts/tools_audit_diff.py
python scripts/tools_audit_rename_churn.py
```

---

## 7. 待用户裁决（未裁决前不动代码，遵循 AGENTS.md 硬性规则）

- **D1 struct/class 判定依据**：(a) 纯按关键字（`struct` → 裸名，`class` → `m_<snake>_`）。简单、可被扫描器自动校验；实测**只有 `XcpAddress40` 一个 struct 带成员函数**，其余全是纯数据载体，所以 (a) 的语义副作用极小。(b) 按语义"有行为即用 `m_`"——会永久留下需要人工判断的分歧面，且 `UdpHeader` / `UdpFrameView` 的编解码其实是**自由函数**（`EncodeUdpFrame` / `DecodeUdpDatagram`），按 (b) 判也没有差别。**我建议 (a)**。
- **D2 GoogleTest 夹具成员**：`slave_`/`ep_`/`ctr_`/`res_` 4 处，以及 tests 里那 44 处缺尾下划线，是"保留豁免"还是"统一 `m_<snake>_`"？**建议夹具成员保留裸尾下划线豁免**（`TEST_F` 体内高频直引用，`m_` 明显降低可读性），但那 44 处 `m_ag` 应补尾下划线。
- **D3 实施范围与批次**：只做 A 类（18 字段 / 189 处，收口你已改的 6 个 struct）？还是 A+B 一次做完（57 字段 / 566 处）？**建议 A 类一批、B 类一批**，B 类牵动 `protocol_types.hpp`（全仓 include），单独提交便于回滚。
- **D4 文档 §10 缺失的 `raw_error_code` / `raw_event_code` 是否本轮补齐**：**建议补**，否则"改完仍不对齐"。
- **D5 审计脚本去留**：留仓库根目录 / 移到 `scripts/` / 用完即删。→ **已裁决**：移到 `scripts/`；后因 `.gitignore` 第 16 行含 `scripts` 条目（外部改动，非本批次所加），复核时向用户确认，**裁决为不入库、仅作本地审计工具**，文档措辞已相应改为"需自备工具或按锚点人工核对"。

## 8. 建议执行顺序（批准后）

1. 改 §2.2.1 规则表：按 D1 拆 struct/class 两行；撤销被吸收的豁免条目；改写 §2.2.1.1、§2.2.1.2 的失效断言与裁决口径。
2. 补完文档 10 个 struct（41 字段）转裸名；顺手按 D4 补 2 个字段；清掉 §7 L883、§18.3 L2291、附录 B.1 残留。
3. 按作用域精确改 A 类 18 字段 / 189 处 → Release 构建 + ctest 全绿 + clang-format。
4. 复跑三个审计脚本，确认分类计数归零（`struct m_snake_` = 0、`class m_snake_noTail` = 0 或已登记豁免）。
5. 出批次 4 实施记录。
