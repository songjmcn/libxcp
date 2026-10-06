# XCP 1.3.0 实施记录 · 批次 6：接口级一致性收口 + 公开头文件中文注释补全

日期：批次 5 之后（批次 5 已由用户提交，本批次改动尚未提交）
分支：`feat_add_xcp`
触发：用户指令「我已完成提交，分析下一步修改计划，并执行下一步修改」。

---

## 0. 本批次为什么是这两件事（分析结论，不是拍脑袋）

AGENTS.md 有一条硬性规则：**设计方案经用户批准之前，禁止写任何代码**。所以"下一步"
只能在**已批准的设计范围内**找欠账。为此先做一次接口级差集分析（工具
`scripts/tools_audit_iface_gap.py`），把设计文档 `cpp` 原型块 与 `include/**/*.hpp`
实际声明做双向比较，结果：

| 比较方向 | 批次 5 结束时的状态 | 本批次处理结果 |
| --- | --- | --- |
| 文档声明、代码未实现 | **只有 1 个**：`BytesToAg` | 判定为"不该实现"，见 §2.1 |
| 代码已实现、文档未登记 | **7 个** | 全部登记进文档，见 §2.2 |
| 文档原型与代码不一致 | **2 处名字 + 1 处参数名** | 以代码为准修正文档，见 §2.3 |
| 类型级差集 | 仅文档存在 2 个（测试替身，实为扫描范围问题） | 0 / 0 |

即：**代码侧对已批准设计没有欠账，偏差全部在文档侧**；另有一类纯代码侧的规范欠账被
顺带查出——AGENTS.md 要求"所有接口、函数、成员函数、成员变量以及全局变量需要添加
注释，注释使用 doxygen 格式，需要写中文"，实测公开头文件有 **74 处声明没有注释**。

因此本批次 = 文档侧对齐（§2）+ 代码侧注释补全（§3）。**没有新增任何接口、没有改动
任何一行可执行代码**（§4.2 有机器证明）。

---

## 1. 本批次改了什么

### 1.1 代码（仅注释，8 个头文件）

| 文件 | 补注释的声明数 |
| --- | --- |
| `include/libxcp/session.hpp` | 21 |
| `include/libxcp/udp_transport.hpp` | 17 |
| `include/libxcp/command_executor.hpp` | 13 |
| `include/libxcp/command_codec.hpp` | 10 |
| `include/libxcp/memory_access.hpp` | 5 |
| `include/libxcp/xcp_error.hpp` | 4 |
| `include/libxcp/response_parser.hpp` | 2（另 1 处系扫描器误报，见 §4.4） |
| `include/libxcp/udp_header_codec.hpp` | 2 |
| **合计** | **74 → 0** |

新增注释行 264 行，删除注释行 3 行（被更详细的版本替换）。

### 1.2 设计文档（`code-plan/XCP_1.3.0_详细设计_类接口与头文件.md`，9 处）

全部改动都带 `/// @note 批次 6 登记/修正` 说明，便于日后区分"原设计"与"事后对齐"。

---

## 2. 文档侧：接口登记与代码对齐

### 2.1 `BytesToAg`：唯一一个"文档有、代码无"，判定为不实现

文档 §3.6 原有原型 `std::optional<AddressGranularity> BytesToAg(std::uint8_t bytes)`，
代码从未实现。查证依据（不是凭印象）：

1. `include/` + `src/` + `tests/` 全仓检索 **0 个调用者**；
2. XCP 的 AG 来自 `COMM_MODE_BASIC` 的 **bit1-2 编码域**（00/01/10 有效、11 保留），
   不是"字节数"；真正的转换函数是 `CommModeBasicToAg()` / `AgToCommModeBasicField()`，
   两者代码都已实现且有往返用例锁定；
3. `AddressGranularity` 的枚举值刻意取 1/2/4（即字节数），要"字节数→AG"直接
   `static_cast` 即可，包一个函数只会引入"非法字节数"这个协议上不存在的失败分支。

处置：在文档里把该原型**注释掉并写明理由**（保留说明，避免下一轮又被当成缺口），
而不是实现它——AGENTS.md 禁止擅自添加接口。**若后续 A2L 里程碑确有需要，须先出设计再实现。**

### 2.2 代码已实现但文档未登记的 7 个接口（补原型）

| 接口 | 代码位置 | 文档补登记处 |
| --- | --- | --- |
| `ToErrorCode(std::uint8_t)` | `protocol_types.hpp:134` | §3.4 |
| `ToEventCode(std::uint8_t)` | `protocol_types.hpp:160` | §3.5 |
| `CommModeBasicToAg(std::uint8_t)` | `protocol_types.hpp:189` | §3.6 |
| `AgToCommModeBasicField(AddressGranularity)` | `protocol_types.hpp:193` | §3.6 |
| `XcpException::GetCommandCode()` / `GetErrorCode()` | `xcp_error.hpp:67,70` | §4.2（见 §2.3） |
| `detail::MakeRecoveryFailed(msg, cmd, retry)` | `xcp_error.hpp:110` | §4.3 |
| `CommandCodec::GetByteOrder()` / `ResponseParser::GetByteOrder()` | `command_codec.hpp:31` / `response_parser.hpp:80` | §8 / §9 |
| `CommandExecutor::AsListener()` | `command_executor.hpp:100` | §11 |

补登记时**逐条读过头文件的真实签名**（写前读），并核实调用者后再写 `@details`：
例如 `GetByteOrder()` 的实测调用者是 `CommandExecutor::EnsureCodec()`
（`m_codec_->GetByteOrder() != byte_order` 决定是否重建编解码器），
`AsListener()` 的唯一调用者是 `xcp_master.cpp:50` 的
`m_transport_->Open(m_executor_->AsListener())`。
（`GetByteOrder` 的说明最初写成了"MemoryAccess/Session 据此判断"，那是推测，
实测调用链后已当场改正——见 §4.5。）

### 2.3 文档原型自身与代码不一致的 3 处（以代码为准）

1. **`XcpException` 两个 getter 的名字写错了，而且文档版本根本不是合法 C++**：
   文档 §4.2 原写
   ```cpp
   [[nodiscard]] std::optional<CommandCode> CommandCode() const noexcept;
   [[nodiscard]] std::optional<ErrorCode>   ErrorCode()   const noexcept;
   ```
   成员名 `CommandCode` 与它在同一行引用的类型名 `CommandCode` 同类作用域冲突
   （成员名遮蔽类型名，`std::optional<CommandCode>` 在类作用域内无法解析）。
   代码自始实现为 `GetCommandCode()` / `GetErrorCode()`，故改文档。
   顺带说明：这也是 §2.2.1.3 里 `m_error_code_` 判为 CONFLICT 的 class 侧成员。
2. **`ResponseParser` 构造函数的 `@param` 写成了类型名**：`/// @param ByteOrder Session 字节序`
   → `/// @param byte_order …`，与 §2.2.1 参数命名规则及代码形参一致。
3. **§4.3 一处注释与声明被挤到同一行**（本批次编辑过程中由我自己的误操作造成，
   当场读回发现并修复，见 §4.3）。

### 2.4 差集工具的最终输出（作为本批次验收证据）

```
=== 提取器自检 ===                     9 项全 OK（含注入探针、测试替身探针）
=== 类型级差异 ===                     仅文档存在: []   仅代码存在: []
=== 成员函数差异（测试替身只比类型不比成员）===   合计待实现成员函数: 0
=== 自由函数差异 ===                   仅文档: ['Connect','Disconnect','ReadMemory','SetResponse','master','move']   仅代码: []
```

"仅文档"这 6 个自由函数经逐条核查**全部是使用示例造成的伪差**（如 §18 的
`master.Connect();`、`std::move(...)`），非真实接口缺口；核查依据是它们在文档中
只以"裸调用"形态出现在示例块内。为消除这类噪声本应让提取器区分"规范块/示例块"，
本批次未做，登记为限制（§5）。

---

## 3. 代码侧：74 处中文 Doxygen 注释补全

### 3.1 判定口径

`scripts/tools_check_doxygen_coverage.py`：
* 先把头文件合并成**逻辑语句**（跨行声明在本项目很常见）；
* 紧邻声明上方的 `///`、`//!` 或 `/** … */` 视为已注释；行尾 `///<` 亦视为已注释；
* 中间隔了空行判为未注释（本项目写法是注释紧贴声明）；
* `= delete` / `= default` 样板行跳过（由其所在分组的 `// 禁止拷贝` 说明覆盖）。

### 3.2 注释内容全部来自实现，不是凭记忆

每个函数补注释前都读了 `src/*.cpp` 对应实现，把**可验证的行为**写进注释，例如：

* `Session::EstablishConnection()` —— 先校验状态必须是 `Connecting`，校验
  `MAX_CTO ≥ 0x08` / `MAX_DTO ≥ 0x0008`（`kMaxCtoMinimum` / `kMaxDtoMinimum`），
  重置参数后才写入 CONNECT 结果并把 SHORT_UPLOAD 能力置为可用；
* `Session::MarkCommandSent()` —— 槽位被占或处于 `Failed`/`Disconnected` 时抛
  `InvalidState`（单 Outstanding Command 约束）；
* `Session::MaxUploadElements()` = `MaxShortUploadElements() - 1`，而后者 =
  `MAX_CTO / AG` —— 注释里明确写出这个"留一格"的差别，避免两者被当成同一个上限；
* `UdpTransport::Close()` 的 `@note` 写的是**为什么不能先关 Socket 再 join**：
  句柄号会被 OS 立即回收并可能分配给进程内新建的 Socket，而接收线程此刻仍可能
  正在处理入站包（这条来自 `.cpp` 里的实现注释，属"实现细节以代码为准"）；
* `UdpTransport::SendCtr()` 的语义精确到"**下一个待分配**的 CTR，Open() 后为 0，
  每发出一个 Frame 递增一次（不是每条命令一次）"（实测 `Load` 后 `Store(ctr+1)`）；
* `UdpTransport::HandleFrame()` 的 `@return false` 明确列出两种实测丢弃条件：
  CTR 与期望差 `0x8000`（方向歧义）与重复/后向乱序；
* `CommandExecutor::WaitForResponse()` 写明"收到 `EV_CMD_PENDING` 时**重启计时但不重发**
  原命令（计划 §6.3）"；
* `detail::Make*()` 8 个辅助函数逐个核对各自产出的 `ErrorCategory`（实测 8 个一一对应），
  注释里直接写分类名。

### 3.3 `= delete` 两行曾被误删（本批次最严重的一次自伤，已当场抓回）

给 `udp_transport.hpp` 补注释时，`edit` 的 `old_string` 覆盖了
`UdpTransport(const UdpTransport&) = delete;` 两行而 `new_string` 漏掉了它们——
**拷贝禁止被静默移除**，这是语义改动，且编译器不会报错（只是变得允许拷贝）。
靠"改后读回"发现，立即补回，并为此专门写了 §4.2 的机器核对。

---

## 4. 验证

### 4.1 注释覆盖率

```
=== 自检 A：合成样本 ===
  应报出 Bare / bare_field      -> OK      （证明检得出来）
  不应报 Documented / Wrapped / tail_field -> OK （证明不误报，含跨行与行尾注释两种写法）
=== 公开头文件 Doxygen 注释覆盖（12 个头文件）===
  已注释: 172    缺注释: 0
  正例锚点 GetSessionParameters         未被误报 -> OK
  正例锚点 ParseGetCommModeInfoResponse 未被误报 -> OK
  正例锚点 m_transport_                 未被误报 -> OK
  全部公开头文件声明均已带 Doxygen 注释
```

### 4.2 纯注释改动的机器证明（`scripts/tools_verify_comment_only.py`）

从 `git diff -U0` 取删除侧/新增侧，各自**剥掉注释与行尾注释**后比较"纯代码行"多重集，
必须完全相等。自检含 4 项（纯注释=等价、夹带代码=不等、行尾注释变化=不报、
误删代码行=必抓，最后一项正是 §3.3 那次事故的形状）。

```
include/libxcp/command_codec.hpp        代码行完全不变（新增 39 行注释，删除 0 行注释）
include/libxcp/command_executor.hpp     代码行完全不变（新增 82 行注释，删除 2 行注释）
include/libxcp/memory_access.hpp        代码行完全不变（新增 27 行注释，删除 0 行注释）
include/libxcp/response_parser.hpp      代码行完全不变（新增 9 行注释，删除 0 行注释）
include/libxcp/session.hpp              代码行完全不变（新增 38 行注释，删除 0 行注释）
include/libxcp/udp_header_codec.hpp     代码行完全不变（新增 0 行注释，删除 0 行注释）
include/libxcp/udp_transport.hpp        代码行完全不变（新增 59 行注释，删除 0 行注释）
include/libxcp/xcp_error.hpp            代码行完全不变（新增 16 行注释，删除 1 行注释）
结论：全部改动均为注释，代码逐字节不变
```

（`udp_header_codec.hpp` 显示 0/0 是因为它改的是两条**行尾** `///<`，被算进代码行
本体后又剥掉了——该文件代码行确实未变。）

### 4.3 行尾与格式（两项都是本批次新引入又被抓出的问题）

* **`edit` 工具把 5 个头文件整体写成了 LF**（`command_executor` / `memory_access` /
  `session` / `udp_transport` / `xcp_error`），而本仓库工作树是 CRLF
  （`core.autocrlf=true`，无 `.gitattributes`）。git 的
  `LF will be replaced by CRLF` 警告暴露了它。已全部转回 CRLF，复查
  `裸LF=0`。同类污染还发生在批次 5 的 2 个 markdown 记录上，一并修回。
* **第一次 clang-format 检查根本没跑**：用了本版本不存在的 `--nolf` 参数，
  clang-format 报 `Unknown command line argument` 而循环只认退出码，于是把 8 个文件
  全报成"不合规"——假信号。改用 `--output-replacements-xml` 数 `<replacement>` 后，
  发现 16 处真实不合规（全是新增中文注释按列宽被 ReflowComments 折行、以及
  `udp_header_codec.hpp` 两条行尾注释过长导致的成员声明重排）。
  先把那两条行尾注释改短（避免代码被重排），再 `clang-format -i`，复查
  **不合规文件数: 0**。

### 4.4 顺带修掉的 3 个检测器缺陷（本批次的"查漏"）

| 缺陷 | 后果 | 修法 |
| --- | --- | --- |
| 注释覆盖率扫描按**物理行**判定 | 跨行声明既误报（`ParseGetCommModeInfoResponse` 明明有 `/** */`）又漏报（`ReadU16`、`PerformAttempt`、`WaitForResponse` 等 5 处跨行声明根本没被看到） | 改成先合并逻辑语句，再判定 |
| 自检锚点用"真实文件里已知缺失的名字"（`AsListener`） | 修完那个名字后锚点必然失效，且我当时已经改了它 → 自检 FAIL | 负例改用**内置合成样本**，正例才用真实文件；这样锚点不会随进度腐坏 |
| 差集工具把 `tests/` 的测试替身纳入**成员级**比较 | 冒出 34 条假"待实现" | 测试替身只做类型级比较（§15 原型是示意性的，非规范接口），并在输出里写明该口径 |

### 4.5 编译与测试

```
cmake --build cmake-build-release --config Release -j 4     → exit 0
  构建日志中 ': error' / ': warning' / 'warning C' 命中 0 行
  产物：xcp.lib / gtest.lib / gtest_main.lib / libxcp_tests.exe
ctest -C Release                                            → exit 0
  100% tests passed, 0 tests failed out of 253
  Total Test time (real) = 13.96 sec
  未运行：#250 AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8 (Skipped)  ← 按设计跳过
```

（本会话中 `cmake --build` 一开始出现"MSBuild 打印版本横幅后无任何输出即 exit 1"的现象，
是沙箱拦截 MSBuild 节点间命名管道所致；经用户批准以 `danger-full-access` 运行构建与
ctest 后正常。另外一次误因是**漏了 `--config Release`**——本仓库是 VS 多配置生成器，
不指定 config 会走 Debug。）

### 4.6 命名审计仍然全绿（未回归）

```
tools_audit_member_naming.py code include src tests
  → 37 个文件 230 条成员记录（与批次 5 收口值完全一致）
    class m_snake_ 132 / s_snake_ 4 / snake_ 4 / struct bare_snake 90 /
    struct m_snake_ 0 / class m_snake_noTail 0 / class bare_snake 0
tools_audit_member_naming.py doc <设计文档>
  → 130 条记录（本批次新增的都是函数原型，无成员变量，故数量不变）
tools_audit_diff.py <设计文档> include tests
  → 16 / 16 struct 字段名完全一致，0 待改，0 真实差异
```

> ⚠️ **`tools_audit_diff.py` 的目录口径必须连数字一起引用**：`FaultInjection` 定义在
> `tests/udp_test_slave.hpp`，只有把 `tests` 一起传入才得到"16 / 16"；只传 `include`
> 得到"15 / 15"。本批次收尾时因少传 `tests` 一度误判为"struct 掉出"，实际无回归。
> 传 `include tests` 时"仅代码存在的 struct"为 7 个测试/实现内部结构
> （`AgCase` / `AgIntegrationCase` / `Case` / `Fixture` / `Harness` / `Rig` /
> `SocketImpl`），属预期——设计文档不登记测试夹具。

### 4.7 收尾复查总表（最终复测值）

| 核对项 | 工具 | 结果 |
| --- | --- | --- |
| 接口级差集 | `tools_audit_iface_gap.py` | 类型差集 0/0；待实现成员函数 0；代码有文档无 0；提取器自检 9 项 OK |
| 成员命名（代码/文档） | `tools_audit_member_naming.py` | 230 / 130 条，两级自检全 OK |
| 文档↔代码字段一致性 | `tools_audit_diff.py` | 16/16 一致，0 待改，0 差异 |
| 纯注释证明 | `tools_verify_comment_only.py` | 8 个头文件代码行逐字节不变，exit 0 |
| 注释覆盖率 | `tools_check_doxygen_coverage.py` | 已注释 172 / 缺注释 0，自检 3 项 OK |
| 格式 | `clang-format --style=file --output-replacements-xml` | 8 个改动文件替换数 0 |
| markdown 围栏 | `tools_check_md_fences.py` | 4 个文档围栏成对闭合，行内三连反引号 0 处 |
| 行尾 | 字节统计 | 11 个受影响文件全部 CRLF，裸 LF=0 |
| 编译 / 测试 | cmake + ctest (Release) | exit 0、0 error / 0 warning、253/253 通过（1 项按设计跳过） |

### 4.8 本批次我自己造成并当场抓出的 4 个问题

| # | 问题 | 怎么被抓出来的 | 处置 |
| --- | --- | --- | --- |
| 1 | `edit` 覆盖 `udp_transport.hpp` 时**吞掉两行 `= delete`**（拷贝禁止被静默解除，编译器不会报） | 改后读回改动处 | 立即补回；为这类事故写了 `tools_verify_comment_only.py`（其自检第 4 项正是这个形状） |
| 2 | `edit` 的 `old_string` 只含一段引言的首行，插入新节后**该引言首行丢失、尾句孤立在文档尾部** | 插入后读回上下文 | 恢复完整 3 行引言，删除孤立残句 |
| 3 | 第一次格式检查用了**不存在的 `--nolf` 参数**，clang-format 从未运行，却把 8 个文件全报"不合规" | 读到 `Unknown command line argument` 的 stderr | 改用 `--output-replacements-xml` 数替换项，查出 16 处真不合规并全部修掉 |
| 4 | `edit` 工具与我写的 `fix_inline_fences.py`（`read_text()` 默认做换行转换）先后**把文件写成 LF**，破坏工作树 CRLF 约定 | git 的 `LF will be replaced by CRLF` 警告 + 专门的字节统计复查 | 5 个头文件 + 3 个 markdown 全部转回 CRLF（复查裸 LF=0）；脚本改为 `read_bytes`/`write_bytes` 并保留原行尾 |

外加修掉一处**早期批次遗留**的文档缺陷：正文中以行内形式书写"三个反引号 + cpp"
共 5 处（含 §2.2.1.2 与设计文档、审核记录各处），CommonMark 会把它当成行内代码定界符
而吞掉同行后续文字，已统一改为用单反引号包住的 `cpp` 写法；为此留下只读工具
`tools_check_md_fences.py`。

**本批次累计教训**：`edit` 的 `old_string` 覆盖范围必须与 `new_string` 严格对齐——
两次事故（#1、#2）都是"替换块比插入块宽"造成的静默删除。凡此类别的批量文本改动，
收尾必须跑"代码行逐字节不变"这类**与改动意图无关的正交核对**，而不是再读一遍自己写的
`new_string`。

---

## 5. 本批次未做与限制

1. **未做任何功能开发**。§12 的六个后续里程碑（A2L 最小集、XCP on CAN/CAN FD、
   TCP、Linux/macOS CI、真实 ECU ↔ CANape 互操作抓包、DAQ/ODT/Timestamp、
   Seed & Key）**都需要先出设计方案并经用户批准**才能动代码（AGENTS.md 硬性规则），
   故列为待用户选择项，见 §6。
2. **差集工具只做名字级，不做签名级**。签名级比对本批次只人工覆盖了 §4.3 的
   `detail::Make*`（8 个全部一致）与 §4.2 的 getter；其余章节的签名逐字比对未做。
3. **示例代码块未与规范块区分**，导致 §2.4 的 6 条"仅文档自由函数"伪差需要人工核查。
4. **`tools_audit_iface_gap.py` 依赖文档 `cpp` 块的语法正确性**；文档里手写原型若
   换行方式特殊仍可能归类到相邻类型（批次 6 就遇到 `GetByteOrder` 的归属被误判一次，
   靠人工核对 `class` 边界纠正）。
5. 注释覆盖率扫描的口径是"紧邻上方有 doxygen 注释"；**注释内容是否充分**仍需人工判断
   （本批次 74 条的语义来源已逐条读实现核对，见 §3.2）。
6. **markdown 行内代码无法表达"三个反引号"**，所以文档里凡是引用围栏标记本身的地方
   只能改写成"三个反引号"这样的叙述（见 §4.8 末），略微降低可读性。
7. **`edit` 工具保持既有 CRLF 行尾，`write` 工具与 Python `read_text()` 不保持**——
   本批次实测（§4.8 #4）。新建 markdown 记录后必须显式转 CRLF，或改用
   `read_bytes`/`write_bytes`。
   顺带查明：`XCP_1.3.0_最小协议核心实现计划.md` 与批次 1~4 的 4 份实施记录
   **本来就是全 LF**（早于本发现，非本批次造成）。因 `core.autocrlf=true` 且无
   `.gitattributes`，git 侧看不到差异，故**本批次未擅自改动它们**；若要统一，
   建议加一条 `.gitattributes`（`*.md text eol=crlf` 或统一 `text=auto`）而不是
   手工批量转换。列为待用户决定项。
8. `scripts/` 仍在 `.gitignore` 内（用户此前的改动），本批次新增 5 个工具不入库：
   只读核对类 4 个——`tools_audit_iface_gap.py`（接口差集）、
   `tools_check_doxygen_coverage.py`（注释覆盖率）、
   `tools_verify_comment_only.py`（纯注释证明）、`tools_check_md_fences.py`（围栏检查）；
   一次性改写类 1 个——`fix_inline_fences.py`（行内三连反引号修正，已改为保留原行尾）。
   连同批次 5 保留的 5 个只读工具，仓库现有 9 个本地核对工具。

---

## 6. 下一步建议（需用户批准其一后再动代码）

| 选项 | 内容 | 代价 / 风险 | 备注 |
| --- | --- | --- | --- |
| **A. 外部互操作验证**（推荐） | 与真实 XCP Slave（CANape Slave / 实际 ECU 网关）做一次抓包互操作，或至少用第三方 XCP 实现做对照 | 需要外部环境；纯验证性工作，不改接口 | 直接补上验收标准 15 这个唯一真正的外部缺口 |
| B. Seed & Key（安全解锁） | `Security` 模块 + `GET/SET/UNLOCK` 命令 | 中等；要新增接口与新测试 | 标定/烧录的前置能力 |
| C. A2L 最小集 | 解析 IF_DATA XCP 段 + 记录布局 | 大；引入第三方解析依赖或自写 parser | 决定后续标定/测量体验 |
| D. XCP on CAN / CAN FD | 新增 transport + 分片/传输层 | 大；需要硬件或模拟总线验证 | §12 里程碑之一 |
| E. Linux/macOS CI | 跨平台构建与测试流水线 | 小～中；可能暴露 Winsock 抽象的缺口 | 项目已声明三平台语法支持 |

推荐 **A**：它是目前唯一"验收标准里写着、且不需要新增接口就能推进"的缺口；
B/C/D 都要先出设计文档再实现，我可以先只做设计不写代码。
