# XCP 1.3.0 实施记录 · 批次 4：命名规则按 struct / class 分流（文档收口）

- **日期**：2026-02-10
- **本批次范围**：**仅设计文档 + 新增审计脚本**。产品代码与测试代码**零改动**（已用 `git diff` 与构建/测试双重确认）
- **依据**：用户对 `code-plan/XCP_1.3.0_详细设计_类接口与头文件.md` 的修改（§7、§10 共 19 行）与本次裁决
- **前置审核**：`code-plan/XCP_1.3.0_审核记录_struct与class成员命名分类.md`

---

## 1. 采纳的裁决

| 编号 | 议题 | 裁决 | 来源 |
|---|---|---|---|
| D1 | struct / class 判定依据 | **按 `struct` / `class` 关键字判定**，不按"有无成员函数/是否纯数据" | 用户选择（推荐项） |
| D2 | GoogleTest 夹具成员 | 保留 `<snake>_` 豁免，且**只覆盖夹具类本身**；tests 里的 helper class 不豁免 | 审核记录默认口径，用户未反对 |
| D2′ | §10 缺失的 `raw_error_code` / `raw_event_code` | 本批次补齐，使文档字段集合与代码一致 | 同上 |
| D3 | 实施范围 | **本批次只改文档**（规则表 + 剩余 10 个 struct + 清残留 + 重写已证伪断言），代码分批改 | 用户选择（推荐项） |
| D5 | 审计脚本 | 移入 `scripts/`；因 `.gitignore` 含 `scripts` 条目，复核后裁决为**不入库**（详见 §2.6） | 用户选择 + 后续复核改判 |

---

## 2. 设计文档改动清单

### 2.1 规则本体（最关键的缺口）

`§2.2.1` 命名规则表：

- 原单行「普通成员变量 → `m_` + 小写蛇形 + 末尾 `_`」**拆为两行**：
  - `class` 成员变量 → `m_<snake>_`
  - `struct` 成员变量 → 裸 `snake_case`，明确"不加 `m_` 前缀与末尾 `_`"
- 「布尔变量」「智能指针/容器」两行的示例**按类别分列**（原文示例 `m_connected_`、`m_transport_` 在新规则下类别不明）
- 新增 **§2.2.1.1 struct 与 class 的判定依据**：为什么可以只看关键字、有意接受的后果、class 侧清单、**唯一豁免**（夹具成员）、匿名 struct 与 `.cpp` 内私有 struct 的归属
- 新增 **§2.2.1.2 裁决历史**：显式声明批次 3 的两条裁决**已被推翻**并保留可追溯
- 新增 **§2.2.1.3 落地状态核对**：文档侧 / 代码侧两张表，含"禁止全局文本替换"的安全约束

**同时保留**（未因整块替换而丢失）：静态成员 `s_<snake>_`、全局变量 `g_<snake>_`、参数与局部变量 snake_case、`constexpr` 用 `k` + 大驼峰、宏全大写——替换脚本内置了"既有规则逐条保留"断言，缺失即中止。

### 2.2 struct 字段转裸名（10 个 struct / 41 字段）

| 章节 | struct | 字段数 |
|---|---|---|
| §3.9 | `ConnectResponse` | 9 |
| §3.9 | `GetStatusResponse` | 8 |
| §3.9 | `GetCommModeInfoResponse` | 6 |
| §3.11 | `SessionParameters` | 4 |
| §3.10 | `XcpAddress40` | 2 |
| §6 | `UdpHeader` | 2 |
| §6 | `UdpFrameView` | 2（含 1 处 `@note` 注释引用） |
| §6 | `UdpFrame` | 1 |
| §12 | `CommandResult` | 1 |
| §15.2 | `FaultInjection` | 6 |

加上一轮已改的 `UdpTransportConfig`(8) + 5 个 Packet(10) + `CommandTimeouts`(3)，文档侧 struct 现共 **17 个 / 64 字段全部裸名**。

### 2.3 清除旧名残留（6 处散文 / 示例引用）

| 位置 | 处理 |
|---|---|
| §7 `strict_remote_port` 的注释 | `m_remote_port_` → `remote_port`（同块内漏改） |
| §16.4 关闭语义 | `UdpTransportConfig::m_receive_poll_interval_ms_` → `receive_poll_interval_ms` |
| §18.2 D5 说明 | `FaultInjection` 增加 `m_ctr_offset_n_` → `ctr_offset_n` |
| §18.3 可选增强表 | `UdpTransportConfig::m_strict_remote_port_` → `strict_remote_port` |
| 附录 B.1 示例 | `cfg.m_remote_host_` / `m_remote_port_` / `m_local_host_` → 裸名 |
| §10 `PositiveResponse::command` 注释 | 与代码措辞统一（"由调用方传入的期望命令"） |

### 2.4 补齐文档缺失字段（D2′）

`§10` 的 `NegativeResponse`、`EventPacket` 各补 1 个字段，与代码对齐（`response_parser.hpp` 的
`NegativeResponse:30`、`EventPacket:40`）：

```cpp
struct NegativeResponse {
    std::uint8_t raw_error_code{0};       // 新增：保留 Slave 返回的未知码原值
    std::optional<ErrorCode> error_code;  // 类型修正：ErrorCode → std::optional<ErrorCode>
    Bytes additional_info;
};
struct EventPacket { /* 同理补 raw_event_code + std::optional<EventCode> */ };
```

### 2.5 改写文末收口说明（口径变更）

原文明确"与本文档存在差异的实现细节，**一律以代码为准**"——这与本批次方向相反，已改为**二分口径**：

- **命名风格**：以文档为准，代码分批跟进；当前差距量化登记在 §2.2.1.3（属已知待办，**不是**"以代码为准"的既成合规）
- **协议语义与实现细节**：仍以代码为准，差异就地标注批次

### 2.6 审计脚本（3 个，只读，**不入库**）

| 脚本 | 用途 | 本批次是否验证过 |
|---|---|---|
| `scripts/tools_audit_member_naming.py` | struct/class 成员风格审计，**内置 5 条已知正例自检**，自检不过则结论作废 | ✅ 文档侧 + 代码侧各跑，5/5 OK |
| `scripts/tools_audit_rename_churn.py` | 改名引用面统计 + 跨类同名冲突检测 + 新名撞名检测 | ✅ 跑通，报出 `max_cto`/`max_dto` 撞名 |
| `scripts/tools_audit_diff.py` | 文档 vs 代码 struct 字段**集合**比对 | ⚠️ **初版从未跑过就被留下**，移到 `scripts/` 后因按裸文件名导入依赖而 `FileNotFoundError`；本轮已重写并实测通过 |

三者均**只读**，不修改任何文件。执行文档改写的一次性脚本未保留（避免误重跑），其区间校验思路（改动前断言首尾行内容）已记入 §4 限制 3。

**不入库的实情**：`.gitignore` 第 16 行含 `scripts` 条目（外部改动，非本批次所加），`git check-ignore` 实测命中、`git ls-files scripts` 为 0。
经向用户确认，裁决为**保持不入库**，因此：

- D5 原意"移入 `scripts/` 并入库"只完成了一半（位置迁移完成，入库未成立）；
- 设计文档中所有对脚本的引用已改为**"本地工具、需自备"**，并在 §2.2.1.2 末补"工具可得性说明"，
  同时给出可人工核对的 `文件:行号` 锚点，避免对新克隆者形成死链；
- 第 4 个脚本 `check_struct_methods.py`（论证"哪些 struct 带成员函数"）已**删除**：结论已固化进
  §2.2.1.1 并附行号锚点，而该脚本输出含假阳性（把 `SetResponse`、`get` 等成员调用当成员函数），
  留作"判据"反而有害。

> `.gitignore` 中 `.vscode`、`__pycache__`、`scripts` 三条**均非本批次所加**（工作副本被外部更新）。
> 本批次未修改 `.gitignore`。

---

## 3. 验证

### 3.1 文档侧命名收敛（同一检测器，改前 / 改后）

| 指标 | 改前 | 改后 |
|---|---|---|
| `struct` 成员 = 裸 `snake_case` | 21 字段（7 个 struct） | **64 字段（17 个 struct）** ✅ |
| `struct` 成员 = `m_<snake>_` | 41 字段（10 个 struct） | **0** ✅ |
| `class` 成员 = `m_<snake>_` | 59 | 59（**未受影响**）✅ |
| `class` 成员 = 裸名 / 仅尾下划线 | 0 | 0 ✅ |
| 检测器自检 | — | 5/5 命中（`UdpTransportConfig` 8、`ConnectResponse` 9、`CommandTimeouts` 3、`UdpTransport` 10、`Session` 4） |

```
=== 扫描 1 个文件，命中成员声明 123 条 ===
  class  m_snake_       59
  struct bare_snake     64
```

### 3.2 代码零改动的回归证明

| 检查 | 结果 |
|---|---|
| `git diff --stat -- include src tests CMakeLists.txt` | **空**（产品/测试代码无任何改动） |
| `cmake --build cmake-build-release --config Release` | exit 0，4 个目标产出，**无 error / 无 warning C** |
| `ctest --test-dir cmake-build-release -C Release` | **100% tests passed, 0 failed out of 253**（1 项按设计跳过：`Ag1_Cto8`，AG=BYTE 时任意字节数均合法） |
| `clang-format --dry-run --Werror -style=file`（全部 37 个源文件） | exit 0，无告警 |

> 代码未改，构建与测试仍完整跑一遍，目的是把"我没碰代码"从口头断言变成可核对的证据。

### 3.3 文档字段集合 vs 代码字段集合（`tools_audit_diff.py`，本轮新增的验收判据）

| 指标 | 结果 | 含义 |
|---|---|---|
| 两侧同名 struct | 16 个（文档 17 / 代码 24） | 代码侧多出的是测试用 POD 载体（`AgCase`、`AgIntegrationCase`、`Case`、`Fixture`、`Harness`、`Rig`）与 `.cpp` 内 PIMPL（`SocketImpl`），不属于文档原型 |
| 字段名**完全一致**的 struct | 1 / 16 | 仅 `CommandTimeouts`（批次 3 已裸名） |
| 仅命名待改的字段 | **60**（按 struct 计；`m_data_` 等跨 struct 重名者各自计入） | 与 §2.2.1.3 的 A 18 + B 39 = 57 个**唯一标识符**一致，差额来自重名字段分属多个 struct |
| **字段集合真实差异** | **0** ✅ | 补齐 `raw_error_code` / `raw_event_code` 后，文档与代码的字段**集合**已完全对齐——差异纯属命名，无结构性偏差 |
| 仅文档存在的 struct | `CommandResult` | §12 设计但尚未实现，符合预期 |

> 这一项是本轮才补上的检查：§3.1 只证明"文档自身内部一致"，无法发现"文档字段集合与代码不同"。
> 之前的 `raw_error_code` 缺失就是靠通读代码发现的，而不是靠工具——现在有工具了。

### 3.4 本轮自查纠错（记录以免重犯）

| 错误 | 如何被发现 | 处置 |
|---|---|---|
| `tools_audit_diff.py` 未验证就留在 `scripts/`，移动后因按裸文件名导入而崩溃 | 本轮应"脚本是否还有用"之问逐个实跑 | 重写为按 `__file__` 解析依赖，并去掉死代码；实测通过 |
| 扫描器把 `struct UdpTransport::SocketImpl {` 的成员归到宿主类 `UdpTransport`，虚增 2 个 struct、`UdpTransport` 多算 1 字段 | 上表"仅代码存在的 struct"里出现 `UdpTransport`/`UdpTestSlave` 这两个明显是 class 的名字 | 取限定名最后一段作类型名；复核后 `SocketImpl` 独立成条（4 字段裸名），`UdpTransport` 回落到 10 |
| 我在 §2.2.1.1 写下的 `Fixture`/`Harness`/`Rig` 行号锚点（330/275/463）是**凭审核记录的旧摘记忆拼的，实测全错** | 落笔后按"改后验"回查 `grep 'struct (Fixture\|Harness\|Rig)' tests/` | 改为实测值：`recovery_test.cpp:210`、`memory_access_test.cpp:260`、`xcp_master_integration_test.cpp:316`；同批核对 `protocol_types.hpp:292`（`Advance()`）**确认正确** |

---

## 4. 未做与限制（如实登记）

1. **文档与代码目前仍处于命名冲突状态**——这是 D3 选择的**预期中间态**，不是遗漏。量化待办已写入 §2.2.1.3：
   - A 类冲突：6 个 struct / 18 字段 / **189 处引用**
   - B 类待办：9 个 struct / 39 字段 / **377 处引用**
   - class 待办：tests 里 6 个 helper class / **44 处 `m_ag` 型缺尾下划线**
2. **`CommandResult`（§12）改名无代码对应物**——该结构体本身尚未实现，文档改名零成本，但也不构成"已落地"。
3. **文档内的行号引用会漂移**：审核记录与本记录中的 `L145`、`L883` 等行号指**改动前**的版本；本批次后 §2.2.1 区新增约 47 行，后续应优先用章节号而非行号定位。
4. **审计手段是启发式的**，不是编译器级解析。它靠"作用域栈 + 语句终止符"判定成员，已针对 5 类真实陷阱修正（`-notmatch` 清空 `$Matches`、namespace 括号污染深度、花括号初始化粘连、尖括号内 `(` 误判函数、前向声明误判为成员），但仍可能漏判复杂模板或宏展开的声明。**结论有效性依赖 §3.1 那 5 条自检命中**；自检 FAIL 时表内数字一律不作数。
5. **未做互操作验证**：与批次 3 相同，仍无第三方 ECU / CANape 抓包对照（验收标准 15）。

---

## 5. 下一步（需再次批准，按 AGENTS.md 不擅自动代码）

| 批 | 内容 | 规模 | 建议 |
|---|---|---|---|
| 批次 5 | 文档 §2.2.1.3 的 **A 类**：6 个 struct 转裸名 | 18 字段 / 189 处 | 优先。消除当前文档↔代码冲突 |
| 批次 6 | **B 类**：9 个 struct 转裸名 | 39 字段 / 377 处 | 独立提交（牵动 `protocol_types.hpp`，全仓 include） |
| 批次 7 | class 侧 **44 处**缺尾下划线 | 44 字段 | 再独立一批（纯测试代码） |
| 之后 | 回到功能里程碑：A2L 最小集 / XCP on CAN / 跨平台 CI / 真实 ECU 抓包 | — | 均需先澄清需求并出设计 |

**每批共同约束**：禁止全局文本替换（`m_error_code_` 跨 struct/class、`m_data_` 跨 A/B 类），改完复跑 `scripts/tools_audit_member_naming.py` 核对分类计数归零，再跑 Release 构建 + 253 项测试 + clang-format。
