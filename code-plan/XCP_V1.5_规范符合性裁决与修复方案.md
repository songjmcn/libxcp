# XCP V1.5 规范符合性裁决与修复方案

> **文档性质**：裁决与设计文档。**本文件不含任何代码改动**，仅陈述规范依据、实现偏差与修复方向，供评审后进入实施。
>
> **裁决基准规范（按层分别唯一）**：
> - Protocol Layer：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`，**Version 1.5.0，2017-11-30，312 页**；
> - Ethernet Transport Layer：`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`，**Version 1.5.0，2017-11-30**。
> 本文档的 PL/TL 结论必须分别回溯到对应官方 PDF，不得跨层推导。
>
> **⚠ 来源限定（用户裁决 m01451，最高优先级）**：
> **禁止再参考 `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`（Markdown 转换版）。**
> **凡有疑问，一律直接查阅原始 PDF。**
> 已知该 Markdown 版存在**图片与正文对应错位**（§1.9 中 `:387` 引用的"兼容矩阵图"实为 ASAM 徽标 logo），且表格版面在转换中丢失——**其"某内容不存在"类结论有因转换丢失而误判的风险，不足为凭**。
> 本文档中残留的 `:NNNN` 形式行号引用**均为对 Markdown 版的旧引用，仅作历史痕迹保留，不再作为证据**；其结论的正确性以原始 PDF 复核结果（附录 C、§2A-§2D）为准。
> **执行本文档任何条目时，若需引用规范原文，必须回到 PDF 对应页核实，不得引用 Markdown 版行号。**
>
> **证据基线（重要）**：F-01～F-12 的 Protocol Layer 裁决以官方 PL PDF 为唯一权威来源；T-01～T-08 的 Ethernet 裁决以官方 Ethernet TL PDF 为唯一权威来源。PL 全部 12 项的复核方法与新增发现见附录 C、§2A～§2D；TL 专项复核见 §1.9A 与附录 E。**所有 Markdown 转换版均不得作为规范结论的唯一证据。**
>
> **上游审计**：`code-plan/XCP_1.3.0_规范符合性审计报告.md`（305 行，含 F-01～F-12 共 12 项）
>
> **编制日期**：2026-10-21

---

## 0. 结论摘要

### 0.1 审计报告可信度裁定（全 12 项已完成）

对审计报告全部 12 项进行了**独立规范复核**（以**官方原始 PDF** 为准，而非审计所引的中文 `XCP_1.3.0_document.md`）：

| 审计项 | 严重度（审计） | 审计判断 | 独立复核裁定 | 严重度重估 |
|---|---|---|---|---|
| F-01 AG 地址推进单位 | 高 | 实现错误 | **成立**（§2.1） | 维持高 |
| F-02 AG=DWORD DOWNLOAD 缺对齐 | 高 | 实现错误 | **成立**（§2.2） | 维持高 |
| **F-03 GET_ID 固化 XCPlite 方言** | 高 | 实现错误 | **指控不成立**；但**新增 3 处真实缺陷**（§2A） | **高 → 中** |
| F-04 STORE_DAQ_REQ 位错 | 中 | 实现错误 | **成立**（§2.3） | 维持中 |
| F-05 动态 DAQ 忽略 MIN_DAQ | 高 | 实现错误 | **成立**（§2B.6） | 维持高 |
| F-06 DTO 相对识别字段布局 | 高 | 实现错误 | **成立**（§2B.2） | 维持高 |
| F-07 DTO 多字节固定小端 | 高 | 实现错误 | **成立**（§2B.3） | 维持高 |
| F-08 DAQ_KEY_BYTE 未贯穿 | 高 | 实现错误 | **成立**，措辞须修正（§2B.4） | 维持高 |
| F-09 SHORT_UPLOAD MTA 未同步 | 中 | 实现错误 | **成立**（§2.4） | 维持中 |
| F-10 分段 Seed&Key 超时失步 | 中 | 实现错误 | **成立**，依据须精确措辞（§2C.2） | 维持中 |
| F-11 错误恢复矩阵部分实现 | 中 | 部分符合 | **成立**，属"实现范围"非"违规"（§2C.3） | 维持中 |
| F-12 能力与模式约束未贯穿 | 中 | 部分符合 | **成立**，属"前置校验缺失"（§2D.5） | 维持中 |

**裁定汇总：11 项成立，1 项（F-03）指控不成立并降级。**

**F-03 的复核是本次最重要的反面发现**：审计据中文规范「缺逐字节表」推断实现偏离，但原始 PDF 的 **Table 60** 明确实现即为标准布局。**该误判的根源是本仓库自身的头文件注释错误**（`include/libxcp/response_parser.hpp:185-186` 声称"规范 LENGTH 为 WORD 且偏移不同"），已写入 §2A.3。

**审计报告整体可信度：高。** 12 项中 11 项经独立复核成立，仅 1 项误判（且可归因于本仓库文档缺陷）。**审计的方法论（对照规范逐条比对实现）是可靠的；其唯一的系统性弱点是以中文规范为唯一依据，缺少逐字节表格时会产生推断错误。**

### 0.1.1 大量审计未提及的新增约束（本轮复核发现）

复核过程中发现**十余处审计未提及的规范约束**，必须逐项裁决并进入本轮修复或明确的后续清单，不能无记录遗漏：

| # | 新增约束 | 出处 | 影响项 |
|---|---|---|---|
| 1 | `CAL_PAG_CFG_LOST` = bit 1（`0x02`）**完全缺失** | Table 49 | F-04 |
| 2 | `STORE_DAQ_REQ` **命名陷阱**：GET_STATUS 的"位" vs SET_REQUEST 的"模式参数" | `:847` vs `:837-896` | F-04 |
| 3 | 规范自身矛盾：DOWNLOAD 元素数表格式 vs 正文式 | `:4097` vs `:4108` | F-02 |
| 4 | **DOWNLOAD 块模式上限公式不同**：`min(MAX_BS*(MAX_CTO-2)/AG,255)` | Table 82 | F-02 |
| 5 | UPLOAD 侧同样有对齐要求（审计只提 DOWNLOAD） | Table 71 | F-01/F-02 |
| 6 | **`Address_Extension` 的 `10` 为非法组合** | Table 142 | F-12 |
| 7 | **`OVERLOAD_MSB` 启用时 ODT 编号上限 `< 0x7C`** | PDF 第 180 页 | F-05 |
| 8 | `GRANULARITY_ODT_ENTRY_SIZE` 取值限 `{1,2,4,8}`，且**地址与长度须按 AG 对齐** | PDF 第 184 页 | F-12 |
| 9 | **`Size=011` 为 "Not allowed"**；`Unit=1101..1111` 为保留值 | Table 147/148 | F-12 |
| 10 | 时间戳**截断取最低有效字节**；时钟**环绕是预期行为** | PDF 第 185-186 页 | F-12 |
| 11 | 物理时间换算等式 `t_phys(k+1) = t_phys(k) + Δt×UNIT×TICKS` | PDF 第 185 页 | F-12 |
| 12 | `TIMESTAMP_MODE`/`TICKS` 以 `TIMESTAMP_SUPPORTED` 为有效性前提 | PDF 第 185 页 | F-12 |
| 13 | `ERR_ACCESS_LOCKED` 语义 + `ERR_SEQUENCE` 亦适用于 **DAQ 配置序列** | `:4686`、`:774-784` | F-10/F-11 |

### 0.2 三处审计报告遗漏（影响修复正确性）

复核过程中发现三处审计未提及、但**必须在修复时一并处理**的问题：

1. **`CAL_PAG_CFG_LOST` = bit 1（掩码 `0x02`）当前完全缺失**，而实现正用 `0x02` 表示 `STORE_DAQ_REQ`。若仅把 `0x02` 改为 `0x04`，则 `CAL_PAG_CFG_LOST` 位仍然缺失，且 `0x02` 这一正确掩码会被弃置。**必须同时补齐。**
2. **规范自身存在矛盾**：DOWNLOAD 最大元素数，表格（`:4097`）为 `(MAX_CTO-2)/AG`，正文（`:4108`）为 `MAX_CTO/AG-2`。AG>1 时两者不等。**实现采用表格式并以最终帧长复核**；正文式会保守少发一个元素，而不是超发。
3. **UPLOAD 侧同样有对齐要求**（Table 71，`:3850`），审计只指出了 DOWNLOAD 侧；且 **DOWNLOAD_MAX 的对齐字节数是「1 or 3」**（`:4199`），与 DOWNLOAD 的「2」不同。

### 0.3 一处跳出版本假设的事实

审计报告以 `XCP_1.3.0_document.md` 中文文档为基准。复核确认：**`docs/` 下存在 ASAM 官方 V1.5.0 英文规范**（Markdown 版 + 原始 PDF 版）。这一事实使"以 V1.5 为裁决基准"成为可能，也修正了此前"规范序列止于 V1.4"的假设。

### 0.4 关键工程决策（须显式声明为决策，非规范结论）

| 决策 | 内容 | 依据性质 |
|---|---|---|
| D1 | 支持范围明确为 **V1.3、V1.4、V1.5**；`EffectiveVersion` 上限为 V1.5。GET_VERSION 成功时按返回 Minor 选择；不可用时不得仅凭 CONNECT Major 猜测 Minor，而应进入“版本未确认”状态并采用 §4.2 的保守能力策略 | **工程决策**（规范无兼容矩阵，见 §1.9） |
| D2 | **F-06/F-07/F-08 采用「补全规范模式语义 + 去掉错误约束」**（原表述为「新增标准模式 + 保留 XCPlite 分支」，已按 §2B.2A 更正：XCPlite 本就是标准 `RelativeWordAligned`） | **工程决策**（保互操作） |
| D3 | 实现 `ERR_CMD_BUSY` 的 `wait t7 + retry`，但“repeat ∞”不得造成不可取消的永久阻塞：必须支持取消、断连和可配置总体 deadline，并同步更新固化旧行为的测试 | **规范恢复动作 + 工程安全边界** |
| D4 | O-1 `ERR_SEQUENCE` 配置序列失步保留为后续项；O-2 中与本轮 F-05/F-12 直接相关且可本地判定的 `OVERLOAD_MSB`、`Address_Extension` 约束**纳入 E2 批次**，不再仅作文档记录 | **范围一致性修正** |
| D5 | 已取得并复核官方 `ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`。工程目标更新为 **Protocol Layer V1.3～V1.5 + Ethernet TL V1.5 / IPv4 UDP 单播基础 Profile**；协议层与传输层版本必须独立裁决。当前阶段端到端测试只使用 XCPlite，发布声明限定为 **XCPlite Profile 已验证**；独立 ECU/标准对端留待后续联调，在此之前不得宣称通用 TL V1.5 互操作已验证 | **规范证据已取得；当前验收边界已裁决** |
| D6 | 版本相关 API 采用一次性破坏性升级：以统一 `XcpMasterOptions` 替换旧构造接口，允许公共配置/快照结构增删字段，不保留旧接口兼容层；项目内调用方、示例和测试必须同批迁移，发布记录必须明确 API/ABI break | **用户裁决：一次修完，避免双轨遗留** |
| D7 | 恢复策略整体采用 §4.6 的参数与语义：普通 timeout 最多恢复重试 2 次；BUSY 默认间隔 100 ms、总体 deadline 10 s；GET_SEED 或 UNLOCK 非末段失步时按分段表最多重启完整事务 1 次；UNLOCK 末段 timeout 作为结果未知写操作，禁止自动重放；取消、断连和析构必须终止等待 | **用户裁决 + 工程安全边界** |
| D8 | 本阶段不引入独立标准对端或第三方运行时 oracle；**仅端到端真实对端限定为 XCPlite**。单元测试、组件测试和规范 golden 必须独立覆盖完整标准矩阵（包括 XCPlite 不支持的 Byte Order、AG、Identification 与 Alignment 组合）。后续由用户结合 ECU 联调补做独立互操作验收；缺少独立对端的组合保持 `Unverified` | **用户裁决：阶段性端到端范围；不得削弱标准组件覆盖** |

**D2 的代价与收益**：收益是**在不牺牲 XCPlite 互操作的前提下把解码器修正到规范**；代价是须补齐 `RelativeWordAligned` 的 FILL/CTR 语义并校正模式选择逻辑。**回归判据**：XCPlite（`DAQ_KEY_BYTE=0xC0`）的 DAQ 互操作行为不变，`tests/dto_envelope_decoder_test.cpp` 与 `XcpliteIntegration` 全部通过。

> **§2B.2A 更正说明**：D2 原先建立在"XCPlite 使用私有协议"的前提上。经直接核实 XCPlite 源码（`thirdparty/XCPlite/src/xcp.h:380-388`、`thirdparty/XCPlite/src/xcplite.c:1616-1621, 2486-2498`），该前提**不成立**——XCPlite 用标准 `RelativeWordAligned`，`0xAA` 是合法 FILL 填充值。**决策目标不变（保持互操作），但实现路径从"增加方言分支"变为"补全规范语义"。**

### 0.5 进一步架构建议

1. **集中版本裁决**：新增单一 Version Resolver/Policy，由它生成不可变的 `ReportedVersion + EffectiveVersion + VersionSource + Confidence`；Codec、Parser、DAQ、MemoryAccess 不得各自猜版本。
2. **能力优先于裸版本比较**：业务模块读取冻结的 Capability Snapshot；版本只负责选择规范语义，Optional 命令仍由实际查询或错误响应确认。
3. **连接完成语义收紧**：CONNECT 成功不等于会话已可用。版本裁决、必要能力查询及配置冲突检查完成前保持 Connecting；并为版本阶段设置独立超时与错误类型。
4. **未知 Minor 是一等状态**：不得用 `0`、V1.3 或当前最高版本填充未知 Minor。公共诊断必须能区分 Confirmed、Configured、MajorOnly、Capped、Conflict、ProbeFailed。
5. **兼容不靠静默试解析**：同一响应在解析前必须已有唯一布局；禁止“先按 V1.5 解，失败后再按 XCPlite/V1.3 解”的内容探测式 fallback。
6. **共享 DTO 布局定义**：核心与 a2lbridge 至少共享同一组规范枚举、头长度/偏移计算或同一 golden oracle，避免两套同名结构再次漂移。
7. **恢复策略数据化**：将 Command×Error/Timeout→Pre-Action/Action/RetryLimit/CancelPolicy 建成显式表，而不是继续扩展散落条件分支。
8. **能力声明分级**：对外文档分别声明 Implemented、Verified、Unsupported、Unverified；“能读取 V1.5 版本”不等于“全部 V1.5 Optional 功能已实现”。
9. **协议层与传输层双版本快照**：分别保存 `ProtocolLayerVersion` 与 `TransportLayerVersion` 的 Reported/Effective/Source/Confidence；不得用 PL 版本推导 TL 版本，也不得因两者数字相同而合并状态。
10. **传输布局配置独立于版本号**：`PACKET_ALIGNMENT` 由可信 A2L `XCP_ON_UDP_IP` 或调用方显式配置提供，不能从 TL V1.5 自动推导；只有“可信 A2L 块存在但省略该关键字”时才能采用其 8-bit 默认值。完全没有 A2L/配置证据时 Alignment 必须保持 Unknown，并阻断依赖多 Frame/Tail 的能力，不能静默猜 8-bit。
11. **Ethernet Profile 显式化**：首阶段仅承诺 TL V1.5 / IPv4 UDP 单播；IPv6、Slave Discovery、远程改 IP、DAQ Clock Multicast 均作为明确的 Optional/Unsupported 能力，不得混入基础符合性声明。
12. **一次性收口公开配置**：用 `XcpMasterOptions` 聚合版本、恢复和既有超时配置，直接替换旧三参数构造接口；不保留 Deprecated 重载或新旧配置双轨。PL/TL 快照通过独立只读 API 暴露，不把版本字段继续塞入旧 `SessionParameters`。
13. **当前验收与未来验收分层**：本阶段以规范 golden、单元/组件测试和 XCPlite 跨进程集成为完成门禁；独立 ECU/标准对端是后续验证里程碑。在后者完成前，能力状态只能是 `Implemented` 或 `XCPliteVerified`，不能标记为通用 `Verified`。

---

## 1. 规范证据基础

### 1.1 版本号语义（PDF 第 12 页 §3.1.1 THE XCP PROTOCOL LAYER VERSION NUMBER）

| 事实 | 原文（PDF 第 12 页 / 旧 Markdown `:NNNN` 仅作痕迹） |
|---|---|
| 版本号为 16 位，高字节 = 主版本 X，低字节 = 次版本 Y | "The XCP Protocol Layer Version Number is a 16 -bit value, where the high byte contains the major version (X) and low byte contains the minor version (Y) number." |
| 高字节递增条件 = 需要改动 Slave 驱动软件（如修改既有命令参数、新增**强制性**命令） | "If the Protocol Layer is modified in such a way that a functional modification in the slave's driver software is needed, the higher byte of the XCP Protocol Layer Version Number will be incremented. This could be the case e.g. when modifying the parameters of an existing command or adding a new mandatory command to the specification." |
| 低字节递增条件 = 对 Slave 驱动软件无直接影响（如改写说明文字、修改 AML 描述） | "If the Protocol Layer is modified in such a way that it has no direct influence on the slave's driver software, the lower byte of the XCP Protocol Layer Version Number will be incremented. This could be the case e.g. when rephrasing the explaining text or modifying the AML description." |
| **CONNECT 只返回主版本字节** | "The slave only returns the most significant byte of the XCP Protocol Layer Version Number in the response upon CONNECT." |

> **核验说明**：以上四条已于 **PDF 第 12 页 §3.1.1** 逐字复核（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`）。PDF 提取文本存在断字痕迹（如 `16 -bit`、`slave’s` 的弯引号），引文已按原文语序还原。
> **另**：`CONNECT` 响应中版本字段标注 "(most significant byte only)" 亦见于 **PDF 第 12 页附近/Table 38**，与上表第四条相互印证。

**工程含义**：高字节递增条件（`:369` 原引用）是判定"某差异是版本演进还是实现自由度"的权威标准。CONNECT 只返回主版本字节这一条，决定了仅凭 CONNECT 无法获得 Minor，这是引入 GET_VERSION 的根本动机。

### 1.2 CONNECT 响应中的版本字段（Table 38，`:3217-3224`）

```
| Position | Type | Description                                              |
| 0        | BYTE | Packet ID: 0xFF                                          |
| 1        | BYTE | RESOURCE                                                 |
| 2        | BYTE | COMM_MODE_BASIC                                          |
| 3        | BYTE | MAX_CTO                                                  |
| 4        | WORD | MAX_DTO                        (占 Position 4-5)          |
| 6        | BYTE | XCP Protocol Layer Version Number (most significant byte only)  |
| 7        | BYTE | XCP Transport Layer Version Number (most significant byte only) |
```

**要点**：因 `MAX_DTO` 是 WORD 占 4-5，两个版本字段落在 **Position 6 / 7**，各 **1 字节**，**均为仅主版本**。

### 1.3 GET_VERSION（`0xC0, 0x00`）

**请求 — Table 44（`:3312-3319`）**，共 **2 字节**：

```
| Position | Type | Description                 |
| 0        | BYTE | Command Code = 0xC0         |
| 1        | BYTE | Level 1 Command Code = 0x00 |
```

无保留字节、**无长度字段**。类别：`Category Standard, optional`（`:3308`）。

**响应 — Table 45（`:3323-3330`）**，共 **6 字节**：

```
| Position | Type | Description                                       |
| 0        | BYTE | Packet ID = 0xFF                                  |
| 1        | BYTE | Reserved                                          |
| 2        | BYTE | Major version of protocol layer                   |
| 3        | BYTE | Minor version of protocol layer                   |
| 4        | BYTE | Major version of currently active transport layer |
| 5        | BYTE | Minor version of currently active transport layer |
```

**要点**：
- Position 1 是 **Reserved**，**不是长度字段**，也不是子命令回显。
- 全部为单字节字段，**字节序无影响**。
- 字段顺序：**Protocol 在前（2/3），Transport 在后（4/5）**。
- **这是协议运行时唯一能直接取得 Slave 当前 Protocol/Transport Minor 的命令**；A2L 或调用方配置只能作为辅助证据，不能冒充运行时返回值。

**真实报文示例 — Table 261（`:8366-8374`）**：

```
GET_VERSION  C0 00
FF 00 01 05 01 03   Major version of protocol layer = 1
                    Minor version of protocol layer = 5
                    Major version of current transport layer = 1
                    Minor version of current transport layer = 3
```

即 **协议层 V1.5 + 传输层 V1.3**，印证协议层与传输层版本可各自独立。

**Level-1 命令族（`:3013`）**：命令分 Level 0（单字节码 `0xC1..0xFF`）与 Level 1（双字节码，以 `0xC0` 起始）。V1.5 中 `0xC0` 族已占用：

| 命令 | 码 | 出处 |
|---|---|---|
| GET_VERSION | `0xC0, 0x00` | `:3042` |
| SET_DAQ_PACKED_MODE | `0xC0, 0x01` | `:3086` |
| GET_DAQ_PACKED_MODE | `0xC0, 0x02` | `:3087` |

### 1.4 optional 命令与错误语义

| 事实 | 原文（行号） |
|---|---|
| 未实现的 optional 命令返回 `ERR_CMD_UNKNOWN` | `:3015`，"An attempt to execute a not implemented optional command will return ERR_CMD_UNKNOWN and does not have any effect." |
| 该机制的目的是让 Master 容易探测未实现的 optional 命令 | `:3017`，"This lets the master device detect not implemented optional commands easily." |
| `ERR_CMD_UNKNOWN` = **`0x20`**，"Unknown command or not implemented optional command"，Severity S2 | `:6990` |
| `ERR_SUBCMD_UNKNOWN` = **`0x34`**，"Unknown sub command or not implemented optional sub command"，Severity S2 | `:7009` |
| Slave 对未实现 optional 命令**应当**返回 `ERR_CMD_UNKNOWN` | `:7013` |
| 错误包格式：`[PID=0xFE][Error code][可选错误信息]` | Table 17，`:2828-2834` |

**关键区分**：GET_VERSION 是 `0xC0` 族命令，其子命令码为 `0x00`。若 Slave **不认识 `0xC0` 族**，返回 `ERR_CMD_UNKNOWN(0x20)`；若 Slave **认识 `0xC0` 族但不认识 `0x00` 子命令**，返回 **`ERR_SUBCMD_UNKNOWN(0x34)`**。**Master 的探测逻辑必须把两者都视为"不可用"。**

> **⚠ 证据强度标注（核验后追加）**：上述"族不认识 → `0x20`／族认识但子命令不认识 → `0x34`"的**区分机制属推论**——PDF 中**无逐字对应句**。该推论由两个错误码的定义文本（`0x20` "Unknown command or not implemented optional command"、`0x34` "Unknown sub command or not implemented optional sub command"）合理导出，**但不得声称规范明文如此规定**。实施探测逻辑时，把两者都按"GET_VERSION 不可用"处理是**保守且安全**的做法。

### 1.5 AG（地址粒度）语义

| 事实 | 原文（行号） |
|---|---|
| **AG 表示单个地址所容纳元素的尺寸**，用于 Master 做地址计算 | `:3273`，"The address granularity indicates the size of an element contained at a single address. It is needed if the master has to do address calculation." |
| **最关键的一条**：涉及 Slave 地址范围的长度信息按 **AG（元素）** 计；涉及数据流的长度信息按 **字节** 计 | `:3296`，"All length information which refers to the address range of the slave itself is based on the AG (**ELEMENTS**). If the length information refers to the data stream ( XCP Protocol ), it is based on bytes." |
| 地址编码 BYTE=1 / WORD=2 / DWORD=4 | Table 42，`:3263-3271`；PDF 第 112 页。原始 PDF 逐行：ADDRESS_GRANULARITY `0 0 → BYTE 1`、`0 1 → WORD 2`、`1 0 → DWORD 4`、`1 1 → reserved` |
| 以 WORD 为例，`Address n = Byte 00`、`Address n+1 = Byte 02` | Table 43，`:3277-3282`；PDF 第 113 页。**原始 PDF 排版**（表头 `Granularity | BYTE | WORD`）：`Address n` → `Byte 00` / `Byte 00 Byte 01`；`Address n+1` → `Byte 01` / `Byte 02 Byte 03` —— **WORD 下地址 +1 跨 2 字节，是"地址按元素计数"的铁证** |
| XCP 地址为 40 位（32 位地址 + 8 位地址扩展） | `:1428`，亦见 `:1550` — "Elements are referenced with a 40 bit address (32 bit XCP address + 8 bit XCP address extension)." |
| MAX_CTO / MAX_DTO 须整除 AG | `:3292-3294`，"MAX_CTO mod AG = 0" / "MAX_DTO mod AG = 0" |

**Table 43 的地址示例是关键旁证**：地址每 +1 对应 2 字节，直接证明地址字段以元素计数。

### 1.6 MTA 推进语义

| 命令 | 规范表述（行号） | 语义 |
|---|---|---|
| UPLOAD `0xF5` | `:3836` "The MTA will be **post-incremented by the given number of data elements**." | 旧 MTA + n（元素单位） |
| DOWNLOAD `0xF0` | `:4106` "The MTA will be **post-incremented by the number of data elements**." | 旧 MTA + n（元素单位） |
| DOWNLOAD_MAX `0xEE` | `:4203` "The MTA will be **post-incremented by MAX_CTO/AG-1**." | 推进量本身即 AG 单位 |
| SHORT_UPLOAD `0xF4` | `:3892` "The MTA pointer **is set to** the first data byte behind the uploaded data block." | **隐式覆盖**为新位置，与旧 MTA 无关 |
| SHORT_DOWNLOAD `0xED` | `:4232` "The MTA pointer **is set to** the first data element behind the downloaded data block." | 同上 |

**措辞差异是语义信号**：`post-incremented by` 表示相对推进；`is set to` 表示绝对覆盖。

> **⚠ 证据强度标注（核验后追加）**：两种措辞**在 PDF 中确实逐字存在**（已核验），但"`is set to` = 绝对覆盖 / `post-incremented by` = 相对推进"的**语义解读属推论**，PDF 未明文解释该措辞差异。**用于 F-09 修复时，应以行为效果（本地 MTA 是否与 Slave 同步）为判据，而非仅凭措辞。**

### 1.7 DOWNLOAD 系列的对齐要求

| 命令 | 对齐要求（行号） |
|---|---|
| DOWNLOAD `0xF0` | `:4104` "**If AG = DWORD, 2 alignment bytes must be used** in order to meet alignment requirements." |
| DOWNLOAD_NEXT `0xEF` | `:4149` "If AG = 4, 2 alignment bytes must be used." |
| DOWNLOAD_MAX `0xEE` | `:4199` "Depending upon AG, **1 or 3** alignment bytes must be used."（无 `n` 字节，首元素偏移须为 AG） |
| UPLOAD `0xF5` | `:3850` "Depending on AG **1, 2 or 3** alignment bytes must be used in order to meet alignment requirements." |
| SHORT_DOWNLOAD `0xED` | **无对齐要求**（固定头 8 字节，8 是 1/2/4 公倍数；`:4219-4232` 表格中无 "Used for alignment" 行） |

**DOWNLOAD 最大元素数 —— 规范自相矛盾（原始 PDF 确证）**：

Table 82（`:4092-4104`；**PDF 第 142 页**）原文逐行：

```
1  BYTE  n = Number of data elements [AG]
         [1..(MAX_CTO-2)/AG] Standard mode                ← 表格
         [1..min(MAX_BS*(MAX_CTO-2)/AG,255)] Block mode
.. BYTEs Used for alignment, only if AG >2
AG=1: 2   ELEMENT 1  1st data element
AG>1: AG
```

| 出处 | 表达式 | 适用 |
|---|---|---|
| Table 82（`:4097`） | Standard: `(MAX_CTO-2)/AG`；Block: `min(MAX_BS*(MAX_CTO-2)/AG, 255)` | **表格** |
| 正文（`:4108`） | `MAX_CTO/AG-2` | 正文 |

**矛盾成因已查明**：表格的 `-2` 是**先扣除命令码与控制字节（2 字节）再除以 AG**；正文的 `-2` 是**在元素数上直接减 2**。二者数学上不等价，仅在 `(MAX_CTO-2) mod AG == 0` 时相同。

**裁决：采用表格式 `floor((MAX_CTO-2)/AG)`**，并以实际编码后的 CTO 长度再次校验 `<= MAX_CTO`。在规范同时要求 `MAX_CTO mod AG = 0` 时，AG=2/4 下表格式等于 `MAX_CTO/AG-1`，而正文式 `MAX_CTO/AG-2` 会**保守少发一个元素**，不会超发；采用表格式的依据是命令头与对齐后的实际帧布局及 Table 82，而不是“正文式会超发”。规范中**未找到**对该矛盾的澄清。

**注意块模式公式不同**：`min(MAX_BS*(MAX_CTO-2)/AG, 255)` —— 实施 F-02 校验时须区分标准模式与块模式，不可用单一公式。

**填充值语义**：规范**未规定**对齐字节必须为 0（全文检索 "FILL / fill byte / padding" 仅命中 DTO Identification Field 的 FILL byte，与 CMD 对齐无关）。因此：Master 侧应填 0（惯例）；**Slave 侧解析时必须忽略对齐字节的值，不得校验**。

### 1.8 GET_STATUS 的 Current Session Status 位定义

**Table 49（PDF 第 116 页）逐位表**：

```
| Bit 7  | Bit 6       | Bit 5 | Bit 4        | Bit 3         | Bit 2         | Bit 1             | Bit 0         |
| RESUME | DAQ_RUNNING | x     | DAQ_CFG_LOST | CLEAR_DAQ_REQ | STORE_DAQ_REQ | CAL_PAG_CFG_LOST  | STORE_CAL_REQ |
```

> **证据强度（已完成目视核验）**：已直接渲染并目视检查官方 PDF 第 116 页 Table 49。表头从左至右明确标为 `Bit 7` 至 `Bit 0`，各列依次为 `RESUME / DAQ_RUNNING / x / DAQ_CFG_LOST / CLEAR_DAQ_REQ / STORE_DAQ_REQ / CAL_PAG_CFG_LOST / STORE_CAL_REQ`。因此 `STORE_DAQ_REQ = bit 2 (0x04)`、`CAL_PAG_CFG_LOST = bit 1 (0x02)` 已由原始版面直接确认，不再是文本提取推定，也不再保留 F-04 待裁决门禁。

**Table 50 编码表（PDF 第 117 页）**：

| 位 | 掩码（**推定值，非 PDF 引文**） | 标志 | 含义 |
|---|---|---|---|
| bit 7 | `0x80` | RESUME | 0=不在 RESUME 模式 / 1=在 RESUME 模式 |
| bit 6 | `0x40` | DAQ_RUNNING | 0=未运行 / 1=数据传输运行中 |
| bit 5 | `0x20` | x | 保留 |
| bit 4 | `0x10` | DAQ_CFG_LOST | 0=Slave 未改变 DAQ 配置 / 1=已改变 |
| bit 3 | `0x08` | CLEAR_DAQ_REQ | 请求清除 DAQ 配置 |
| **bit 2** | **`0x04`** | **STORE_DAQ_REQ** | 请求存储 DAQ 列表 |
| bit 1 | `0x02` | CAL_PAG_CFG_LOST | 0=CAL/PAG 配置未改变 / 1=已改变 |
| bit 0 | `0x01` | STORE_CAL_REQ | 请求存储标定数据 |

**掩码列说明**：位号已由 PDF 第 116 页 Table 49 原始版面目视确认；`0x04`、`0x02` 等十六进制数值由对应位号直接换算。Table 50 提供各标志的文本定义，不单独重复十六进制掩码。

**旁证（§12 示例，PDF 第 280-281 页）**：`FF 00 15 01 00 00` → "Current Session Status = 0x00 / => no request active, Resume not active, no DAQ running"。

### 1.9 关于兼容矩阵：**原文 PDF 已复核，矩阵数据不在本规范内**

> **复核基线升级**：本节及全文的关键结论已用**原始官方 PDF**（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`，312 页，ASAM e.V.，Microsoft Word 2016 生成）交叉复核，不再仅依赖 Markdown 转换版。参见附录 C。

`3.1.2 THE COMPATIBILITY MATRIX`（`:377-387`；PDF 第 12 页）：

- `:385` "The Compatibility Matrix is documented by the **linked table** and gives an overview of the allowed combinations of XCP Protocol Layer Version Number and XCP Transport Layer Version Number."
- `:387` 紧随其后的引用是 `image_000008_f0c5688a9bbaa79f654277cdbf8ac02d1d94589f9dbbf5f6ed035c203ca637ec.png`。

**图片核实（历史痕迹，基准已切换）**：初稿阶段曾核验 Markdown 版的引用图片（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0_artifacts/`，9363 字节），确认其为 **222×56 像素的 ASAM 徽标（logo）**，并非矩阵表格。**该核验仅为说明 Markdown 转换版不可靠，不构成规范依据**——判定矩阵不存在的权威依据是下方的**原始 PDF 复核**。

**原始 PDF 复核（决定性）**：PDF 第 12 页正文**逐字一致**，且紧随其后即 `PDF PAGE 13` 直接进入 `3.2 REFERENCES TO OTHER STANDARDS`。**PDF 第 12 页上不存在任何矩阵表格。**

**关键判据**：原文用词为 "documented by the **linked table**"（**链接的**表格）——即矩阵是**外部链接**，本就不内嵌于规范正文。因此：

> **原始 PDF 本身同样不含矩阵数据。这不是 Markdown 转换丢失，而是规范的设计如此。**

`:381-383` 给出了矩阵的替代判据：

> "If a certain version of the Protocol Layer Specification needs a certain version of a Transport Layer Specification, this will be mentioned as **prerequisite** in the Protocol Layer Specification."

即兼容关系以 **prerequisite** 形式写在各规范正文中，而 V1.5 协议层规范正文**未列出**与具体传输层版本的 prerequisite。

**原始 PDF 提供的替代线索（新增）** — PDF 第 270-271 页 §8.2.1 `PROTOCOL LAYER AND TRANSPORT LAYER PARTS (XCP_DEFINITIONS.AML)` 说明兼容组合的**实际组织方式**：

```
/include XCP_common_vX_Y.aml    /* protocol layer part            */
/include XCP_on_##_vU_V.aml     /* transport layer part(s)        */
```

并给出实例：`XCP_common_v1_0.aml` + `XCP_on_UDP_IP_v1_0.aml` + `XCP_on_CAN_v1_1.aml`。

**工程含义**：兼容组合是**按传输层各自声明其支持的协议层版本**，分散定义，**而非一张全局矩阵表**。这印证了 `:381-383` 的 prerequisite 机制，也意味着**判定必须针对具体传输层**，不能泛化为"协议层 V1.5 兼容一切"。

**关于 revision history**：Markdown 版推断"无变更章节"；**原始 PDF 复核确证** —— 在 460,698 字符的全文提取结果中，`revision` / `Revision` **命中 0 次**。**V1.4 与 V1.5 的显式差异清单在规范中确实不存在。**

**结论（重要的证据边界声明）**：V1.5 与 V1.3/V1.4 的**兼容组合数据无法从本 Protocol 规范确定**——这一点在 Markdown 版与原始 PDF 上**结论一致，且原因已查明（矩阵为外部链接，非转换丢失）**。任何“V1.5 兼容 V1.4”的结论都属于**工程决策**，不得声称有规范矩阵依据。现已进一步复核官方 Ethernet TL V1.5，结果见 §1.9A：该 TL 规范也未规定 PL/TL Minor 必须相等，因此本项目将支持组合作为工程范围逐项验证。

### 1.9A Ethernet Transport Layer V1.5 专项裁决（D5 已更新）

**权威来源**：`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`，ASAM Associated Standard，Version 1.5.0，2017-11-30。此前因规范缺失而暂定的“TL V1.1 基线”自本节起作废。

#### 1.9A.1 版本组合结论

1. Protocol Layer 与 Transport Layer 是两个独立版本维度；CONNECT 只给二者 Major，GET_VERSION 可给二者完整 Major/Minor，但不能用 PL 版本推导 TL 版本。
2. V1.5 Protocol Layer 正文未声明必须搭配某个具体 TL Minor；新取得的 Ethernet TL V1.5 也没有给出“PL V1.5 只能搭配 TL V1.5”的前置条件。因此不存在“版本号必须相等”的规范结论。
3. 本项目工程目标冻结为：**PL V1.3/V1.4/V1.5 + Ethernet TL V1.5 / IPv4 UDP 单播基础 Profile**。这是工程支持范围，不冒充外部兼容矩阵结论。当前阶段通过规范可追溯 golden 与 XCPlite 验证实现；未经过独立 ECU/标准对端的组合统一标记 `Unverified`，留待后续 ECU 联调升级证据等级。
4. XCPlite 示例 A2L 的 `XCP_ON_UDP_IP` 版本为 `0x0104`，且未声明非默认 Packet Alignment。因此当前可与 XCPlite 通信，只证明已有 TL V1.4/8-bit 对齐组合的互操作，不能外推为完整 TL V1.5 符合。

#### 1.9A.2 当前实现已满足的基础线格式

| TL V1.5 基础要求 | 当前状态 | 证据 |
|---|---|---|
| Header 为 `LEN WORD + CTR WORD`，固定 Intel 字节序 | 已实现 | `include/libxcp/udp_header_codec.hpp:27-49`、`src/udp_header_codec.cpp:29-35` |
| 一个 UDP Datagram 可包含多个完整 XCP Frame | 已实现无 Tail 的紧邻打包 | `src/udp_header_codec.cpp:41-89` |
| 每个 Frame 独立 CTR，支持回绕/缺口诊断 | 已实现 | `src/udp_transport.cpp:364-429` 及对应测试 |
| Frame 不得跨 Datagram | 已校验 | `src/udp_header_codec.cpp:71-74` |
| 单 Frame 的 XCP Packet 最大 65503 字节 | 已正确限制 | `include/libxcp/udp_header_codec.hpp:30-38` |

因此当前代码只能定性为 **TL V1.5 `PACKET_ALIGNMENT_8` 基础线格式子集**。

#### 1.9A.3 确定缺口

1. **Tail/FILL 与 Packet Alignment（P0）**：TL V1.5 §3.3.2 定义 Tail，用于把下一个 XCP Frame 对齐到 8/16/32 bit。当前头文件仍写“XCP on Ethernet 无 Tail”（`include/libxcp/udp_header_codec.hpp:6-10`），编码不补齐 Tail，解码按 `Header + LEN` 直接推进（`src/udp_header_codec.cpp:81`）。遇到 16/32-bit 对齐填充时会把 FILL 当作下一个 Header 或残留字节，从而拒绝整个 Datagram。
2. **A2L Alignment 未贯通（P0）**：`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp:28-42` 已保存 `packet_alignment`，`a2l_dto_map.cpp:483-484` 已映射 8/16/32，但核心 UDP Transport 未消费。必须支持“可信 A2L → Transport 配置”，并允许无 A2L 时由调用方显式配置。
3. **传输层版本状态缺失（P0）**：现有版本计划只完整描述 Protocol `Reported/Effective/Source/Confidence`；必须增加独立 TL 快照，并处理 GET_VERSION、A2L、CONNECT Major、显式配置之间的一致性。
4. **IPv6 未实现（能力边界）**：TL V1.5 地址模型包含 IPv4/IPv6；当前 `src/udp_transport.cpp:118-127` 固定 `AF_INET`。本轮明确只承诺 IPv4 UDP 单播，IPv6 标为 Unsupported，不作为基础 Profile 的发布阻断项。
5. **TL 专用命令未实现（Optional 边界）**：`GET_SLAVE_ID`、`GET_SLAVE_ID_EXTENDED`、`SET_SLAVE_IP_ADDRESS`、`GET_DAQ_CLOCK_MULTICAST` 均为 Optional。当前只有 `TransportLayerCmd = 0xF2` 枚举，无完整 Codec/Parser/Execute。基础 Profile 可暂不实现，但不得声明支持；若 A2L `OPTIONAL_TL_SUBCMD` 声明且调用方请求，则必须显式返回 Unsupported，禁止静默发送。
6. **带宽治理缺失（P1）**：TL V1.5 §7 引入 `MAX_BUS_LOAD`、`MAX_BIT_RATE` 与 DAQ/STIM 净数据带宽检查。当前核心未消费这些字段。本阶段冻结为**显式配置优先、确定性预算、A2L 自动提取 Unsupported**：不修改上游 `thirdparty/a2l-sdk/src`，先由调用方提供 `LinkBudgetConfig`；只有未来确认上游公共 API 能可靠给出这两个字段后，才另行启用 A2L 自动提取。

**`LinkBudgetConfig` 冻结字段与单位**：

```text
nominal_bit_rate_bps            // 对应 MAX_BIT_RATE，bit/s，必须 > 0
max_bus_load_percent            // (0, 100]，仅约束 DAQ/STIM
actual_available_bit_rate_bps   // optional；已知时必须 > 0
unknown_link_policy             // AllowUnverified（默认）或 Reject
allow_over_budget               // 默认 false；true 时仍产生显式告警和 Unverified
```

**预算公式（依据 TL V1.5 §7 及其 worked example）**：

```text
alignment_bytes = PACKET_ALIGNMENT / 8
xcp_frame_bytes = 4-byte Ethernet XCP Header + dto_packet_bytes + tail_bytes
tail_bytes = (alignment_bytes - (xcp_frame_bytes_without_tail mod alignment_bytes)) mod alignment_bytes
event_bytes_per_second = (1 / event_cycle_seconds) * Σ(xcp_frame_bytes of all ODTs in the event)
total_busload_bytes_per_second = Σ(event_bytes_per_second of all configured events)
effective_bit_rate_bps = min(nominal_bit_rate_bps, actual_available_bit_rate_bps if known)
allowed_bytes_per_second = effective_bit_rate_bps / 8 * max_bus_load_percent / 100
```

- 规范明确预算只覆盖 **XCP Header + Packet + Tail**，不计算 Ethernet/IP/UDP 外层开销；Datagram 聚合不改变上述净 XCP 字节总数；
- Event 周期使用 A2L `MIN_CYCLE_TIME` 或调用方显式的保守最小周期；周期未知时按 `unknown_link_policy` 处理，不得假造周期；
- `total_busload_bytes_per_second > allowed_bytes_per_second` 时默认在 DAQ 启动前拒绝；显式 `allow_over_budget=true` 只能降为 `Unverified` 并告警，不能标记 Verified；
- 未提供 `LinkBudgetConfig` 时固定采用 `AllowUnverified` 语义：保持现有 DAQ 可用性、发出诊断并将带宽能力标记 `Unverified`；`Reject` 仅在调用方显式提供配置且将 `unknown_link_policy=Reject` 时生效；
- 所有乘加使用带溢出检查的整数/有理数或足够精度类型，最终比较前不得通过向下取整掩盖超预算。

#### 1.9A.4 传输版本与 Alignment 的裁决规则

| 场景 | TL Reported | TL Effective | Alignment 来源 | 处理 |
|---|---|---|---|---|
| GET_VERSION 返回 TL 1.0～1.5 | 保留原值 | 对已实现语义封顶到 1.5 | A2L 与显式配置只有单一来源时采用；两者并存必须一致 | 按冻结能力运行；Alignment 冲突则失败 |
| TL Minor >5 | 保留原值 | 1.5 | A2L/显式配置 | 告警并禁止未知高版本能力 |
| GET_VERSION 不可用、可信 A2L 有 TL 版本 | Major only + Minor unknown | A2L 声明版本 | A2L | 标记 `StaticEvidence` |
| 仅 CONNECT TL Major=1，且无 A2L/显式 Alignment | 1.x unknown | `1.x-conservative` | Unknown | 只保留可证明安全的单 Frame Header/LEN/CTR 路径；阻断依赖多 Frame/Tail 的能力并标记 `MajorOnly`/`Unverified` |
| GET_VERSION 与 A2L TL 版本冲突 | 两者均保留 | 运行时版本优先 | Alignment 仍按 A2L/显式配置单独裁决 | 记录冲突；影响布局时阻断 UDP 会话 |
| A2L 与显式 Alignment 冲突 | 不受影响 | 不受影响 | 无唯一值 | 连接配置失败，禁止静默覆盖 |

**关键约束**：TL 版本号本身不携带 `PACKET_ALIGNMENT`；即使确认 TL V1.5，也不能自动假定 16/32-bit。Alignment 是独立的传输布局参数。

### 1.10 V1.5 的新增特性（凭存在性判定）

因无变更清单，只能凭"特性是否存在"间接判断：

| 特性 | 判定 | 证据 |
|---|---|---|
| **DAQ PACKED MODE** | **确认为 V1.5 新特性** | 独立章节 4.1.10（`:837-889`）；命令 `SET_DAQ_PACKED_MODE 0xC0,0x01` / `GET_DAQ_PACKED_MODE 0xC0,0x02`（`:3086-3087`）；新增属性位 `DAQ_LIST_PROPERTIES` Bit4 PACKED（Table 172，`:5771`）与 `DAQ_EVENT_PROPERTIES` Bit4 PACKED（Table 155，`:5534`） |
| DAQ LIST PRIORITIZATION | 概念章节，**无新命令** | `:775-779` 仅 3 行描述；优先级承载于既有 `SET_DAQ_LIST_MODE` 的 Priority 字段与 `GET_DAQ_EVENT_INFO` 的 `EVENT_CHANNEL_PRIORITY`（`:5603`） |
| `ERR_SUBCMD_UNKNOWN 0x34` | 编码表存在（`:7009`） | 该错误码本身存在于 V1.5 规范 |
| Level-1 双字节命令码机制 | 机制存在 | `:3013` |

### 1.11 错误恢复：**不存在整体版本降级机制**

规范 Table 229 及各命令错误处理表给出的降级是**逐命令**的 "use alternative" 动作，且**必须遵守该行的 Pre-Action**。例：

| 命令 | 错误 | Pre-Action | 动作（行号） |
|---|---|---|---|
| SYNCH | ERR_CMD_UNKNOWN | - | restart session（`:7036`） |
| DOWNLOAD_NEXT | ERR_CMD_UNKNOWN | **SET_MTA** | use alternative（`:7134`） |
| DOWNLOAD_MAX | ERR_CMD_UNKNOWN | **SET_MTA** | use alternative（`:7146`） |
| GET_PAG_PROCESSOR_INFO | ERR_CMD_UNKNOWN | - | use ASAM MCD-2 MC（`:7212`） |
| GET_DAQ_LIST_INFO | ERR_CMD_UNKNOWN | - | use ASAM MCD-2 MC（`:7343`） |

**关键结论**：规范中**不存在**"Slave 返回 `0x20` 即整体降级到某版本"的机制。版本降级必须按**特性粒度**进行，并遵守前置动作。这对本项目的降级设计构成硬约束。

---

## 2. 实现偏差裁决（F-01／F-02／F-04／F-09）

以下四项已逐处读取实现代码核实，行号为准。

### 2.1 F-01【高】AG>1 时地址推进单位错误

**裁决：成立。实现按字节推进，规范要求按元素推进。**

**实现证据** — `src/protocol_types.cpp:200-209`：

```cpp
std::optional<XcpAddress40> XcpAddress40::Advance(
    ElementCount elements, AddressGranularity ag) const noexcept {
    // 先做乘法溢出检查（ElementCount * AG 字节数），再检查地址回绕
    const auto bytes = static_cast<std::uint64_t>(elements) *
                       static_cast<std::uint64_t>(AgToBytes(ag));
    const auto sum = static_cast<std::uint64_t>(address) + bytes;
    if (sum > 0xFFFFFFFFULL) {
        return std::nullopt;
    }
    return XcpAddress40{static_cast<Address>(sum), extension};
}
```

`elements * AgToBytes(ag)` 把元素数换算为字节数后加到地址上。

**规范冲突点**：`:3296` 明确规定涉及 Slave 地址范围的长度按 **AG（元素）** 计。地址字段本身以元素为单位（Table 43 佐证：WORD 下 `Address n+1 = Byte 02`）。因此**正确的推进是 `address + elements`**，不应乘 AG 字节数。

**受影响的其他位置**（C 批次必须逐项关闭）：对 `src/memory_access.cpp:42-54, 120-126, 224-237, 288-297` 的容量、分块、边界与地址推进建立静态调用点清单；凡把 XCP 地址按 `elements * AG` 推进者同步校正，凡仅用于内存缓冲区字节偏移者保留并在清单中注明单位。清单与对应测试映射是 C 批次前置产物。

**判定注意**：规范中**未找到**以显式公式写出 `new_MTA = old_MTA + n` 的原文。上述结论由 `:3296` 的计数基准声明、Table 43 的地址示例、以及 MTA 推进一律表述为 "by the number of **data elements**"（`:3836`, `:4106`）共同推出，构成无歧义推论。**规范中未找到任何支持"AG>1 时地址推进需乘 AG"的表述。**

### 2.2 F-02【高】AG=DWORD 的 DOWNLOAD 缺少对齐字节

**裁决：成立。`EncodeDownload` 恒为 `[F0][n][data...]`，无对齐填充。**

**实现证据** — `src/command_codec.cpp:381-386`：

```cpp
Bytes cto;
cto.reserve(2 + data.size());
cto.push_back(static_cast<std::uint8_t>(CommandCode::Download));
cto.push_back(static_cast<std::uint8_t>(number_of_elements));
cto.insert(cto.end(), data.begin(), data.end());
return cto;
```

**规范冲突点**：`:4104` 要求 AG=DWORD 时插入 2 个对齐字节，即正确布局应为 `[F0][n][pad][pad][data...]`（首元素偏移 = AG = 4）。表格 `:4092-4104` 的 `AG>1: AG` 行证实首元素偏移为 AG。

**并发的容量校验缺失** — `src/command_codec.cpp:371`：

```cpp
if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
```

只按单字节字段上限 255 校验，**未按 `(MAX_CTO-2)/AG` 校验**。AG>1 且元素数偏大时会**超发**，Slave 将返回 `ERR_OUT_OF_RANGE`。

**对比**：`EncodeShortDownload`（`src/command_codec.cpp:404-411`）固定头 8 字节，**天然对齐，无需填充** —— 与规范一致，此处无缺陷。

**须同步裁决的关联项**：UPLOAD 侧对齐（`:3850`）属于现有路径，必须随 F-02 修复；DOWNLOAD_NEXT（`:4149`）与 DOWNLOAD_MAX 的「1 or 3」（`:4199`）属于当前未实现的 Optional Block Mode。本轮不因 F-02 强行新增 Block Mode API，但必须在能力表中显式 Unsupported，并把其对齐、容量和 UNKNOWN→SET_MTA 替代要求记录为未来启用门禁，避免仅有枚举被误报为支持。

### 2.3 F-04【中】GET_STATUS 的 STORE_DAQ_REQ 位解析错误

**裁决：成立。实现用 `0x02`（规范上的 bit 1）表示 bit 2 的 `STORE_DAQ_REQ`。**

**实现证据** — `src/response_parser.cpp:53-58`：

```cpp
/// @brief Current Session Status 位掩码（GET_STATUS Byte 1）
constexpr std::uint8_t kStoreCalReqMask = 0x01U;   // bit 0 — 正确
constexpr std::uint8_t kStoreDaqReqMask = 0x02U;   // 应为 0x04（bit 2）— 错误
constexpr std::uint8_t kClearDaqReqMask = 0x08U;   // bit 3 — 正确
constexpr std::uint8_t kDaqRunningMask  = 0x40U;   // bit 6 — 正确
constexpr std::uint8_t kResumeMask      = 0x80U;   // bit 7 — 正确
```

**错误模式下的一致性**：bit 0、3、6、7 全部正确，**仅 bit 2 错位**。这使错误不易被察觉——掩码整体看起来是合理的。

**唯一的消费点** — `src/response_parser.cpp:224`：

```cpp
resp.store_daq_req = (session_status & kStoreDaqReqMask) != 0U;
```

**语义后果**：当前该语句实际读取的是 `CAL_PAG_CFG_LOST`（bit 1）。即：
- Slave 报告"标定/分页配置已丢失"时，本库会误判为"请求存储 DAQ 列表"；
- Slave 真正报告 `STORE_DAQ_REQ`（bit 2）时，本库**完全察觉不到**。

**必须同时补齐的遗漏**：`CAL_PAG_CFG_LOST`（bit 1，`0x02`）在**整个仓库中不存在**（全仓检索 `kStoreDaqReqMask` / `CalPagCfgLost` 仅命中上述两处）。若只把 `0x02` 改为 `0x04`，则 bit 1 仍然缺失。**正确做法是 `STORE_DAQ_REQ` 改为 `0x04`，并新增 `CAL_PAG_CFG_LOST = 0x02`。**

**⚠ 命名陷阱（实施时必须注意）** — 原始 PDF 复核中发现 **`STORE_DAQ_REQ` 一词在规范中指两个不同事物**：

| 概念 | 位置 | 语义 |
|---|---|---|
| **GET_STATUS 的 `STORE_DAQ_REQ` 位** | Table 49 **bit 2**；`:847` — "The **STORE_DAQ_REQ bit** obtained by GET_STATUS will be reset by the slave, when the request is fulfilled." | 状态标志位，Slave 在请求完成后复位 |
| **SET_REQUEST 的 `STORE_DAQ_REQ_RESUME` / `STORE_DAQ_REQ_NO_RESUME`** | `:837-840, 870, 896`（PDF 第 23-24 页）；流程图写作 `SET_REQUEST(STORE_DAQ_REQ_NO_RESUME,ID)` | **SET_REQUEST 的模式参数**，决定是否带 RESUME/ID |

规范自己用 "the STORE_DAQ_REQ **bit** obtained by GET_STATUS"（`:847`）明确区分——**前者是位，后者是命令参数**。本项修复只涉及前者。

`tests/response_parser_test.cpp:211-226` 固化了当前错误行为，须同步更新。

### 2.4 F-09【中】SHORT_UPLOAD 后本地隐式 MTA 未同步

**裁决：成立。规范要求 SHORT_UPLOAD 将 MTA 设为块末，实现未更新本地 MTA。**

**规范依据** — `:3892`："A data block of the specified length, starting at address will be returned. **The MTA pointer is set to the first data byte behind the uploaded data block.**"

对照 UPLOAD（`:3836`）的 "The MTA will be **post-incremented** by the given number of data elements." —— 措辞差异明确了两种语义。

**实现证据（已回读当前源码确认）**：
- `src/command_executor.cpp:888-898`：SHORT_UPLOAD 成功后**未更新** `m_last_mta_`。
- `include/libxcp/command_codec.hpp:112-121`：注释声称 SHORT_UPLOAD **不改变 MTA** —— 该断言与规范直接冲突，须改写或删除。

**失败路径（审计所述，逻辑成立）**：`src/command_executor.cpp:540-560, 731-735` 的 UPLOAD 超时恢复会回滚到 `m_last_mta_`；若 SHORT_UPLOAD 未同步该值，恢复后将得到**错误的 MTA**。高层分块 API 因每次显式 SET_MTA 掩盖了该问题，但组合 API 仍存在风险。

---

## 2A. F-03 裁决（本轮新增）：**审计指控不成立，但确有三处真实缺陷**

### 2A.1 审计原指控

审计报告 F-03【高】标题为「GET_ID 固化 XCPlite 方言」，指控实现按 XCPlite 私有布局解析，与标准不符，并自注「该项应在修复前**再以 ASAM 官方英文表格复核**」。

### 2A.2 原始 PDF 裁决：实现的布局**就是**规范标准布局

**Table 60 GET ID positive response structure**（原始 PDF 第 123 页）逐字：

```
Position     Type    Description
0            BYTE    Packet ID: 0xFF
1            BYTE    Mode
2            WORD    Reserved
4            DWORD   Length [BYTE]
8            BYTE 1  first byte of Identification (if mode = 1)
8+Length-1   BYTE n  nth byte of identification
```

剥去首字节 `0xFF` 后（`res_data` 为已剥离视图）：

| 字段 | 规范偏移（剥 0xFF 后） | 实现（`src/response_parser.cpp:395-412`） |
|---|---|---|
| Mode | `0`（1 字节） | `resp.transfer_mode = res_data[0]` |
| Reserved | `1..2`（**WORD**） | （跳过） |
| Length | `3..6`（**DWORD**） | `ReadU32(res_data, 3)` |
| DATA | `7..` | `res_data.begin() + 7` |

**逐字段完全一致。** 审计所称「固化的 XCPlite 方言」实为**规范标准布局**。

> **与本轮 §2B.2A 的关系**：F-03 的复核同样推翻了"XCPlite 方言"的定性。**两项独立复核（F-03 的 GET_ID 布局、F-06 的 DTO 识别字段）得到同一结论**——XCPlite 遵循规范，偏离的一直是本项目实现。审计报告把"与 XCPlite 互通"等同于"方言固化"，属定性错误。

> **审计误判的根源已查明**：`include/libxcp/response_parser.hpp:185-186` 的头文件注释声称「规范 §7.5.1.6 的 LENGTH 为 **WORD** 且偏移不同——实然优先」。该注释**与官方规范冲突**，是误判的直接来源。

**补充反证**：GET_VERSION 的 Table 45（原始 PDF 第 114 页）为 `[Packet ID][Reserved][Major PL][Minor PL][Major TL][Minor TL]`，**固定 6 字节、根本不含 LENGTH 字段**。故「LENGTH 为 WORD」的说法**既不适用于 GET_ID，也不适用于 GET_VERSION**，属臆造。

### 2A.3 但 F-03 确实暴露三处真实缺陷

**缺陷一：头文件注释错误（`include/libxcp/response_parser.hpp:183-187`）**

注释含三处不实陈述：⑴ "规范 §7.5.1.6 的 LENGTH 为 WORD"（实为 DWORD）；⑵ "且偏移不同"（实为相同）；⑶ 结论 "实然优先"（实为**规范优先，且本来就一致**）。同时行号引用有误：GET_ID 位于原始 PDF 第 123-124 页（Markdown 版对应 §7.5.1.6）。

**危害**：该注释已实际导致审计报告做出错误的高风险判断。**须改写为符合规范的表述。**

**缺陷二：Mode 字段未按位掩码处理（`src/response_parser.cpp:400`）**

Table 61 GET_ID mode parameter bit mask structure（原始 PDF 第 124 页）：

```
Bit 7..2        Bit 1                  Bit 0
x x x x x x     COMPRESSED_ENCRYPTED   TRANSFER_MODE
```

- `TRANSFER_MODE` = **bit 0**：1 = 标识符内联于响应剩余字节；0 = Slave 设置 MTA，Master 用 **UPLOAD** 命令读取
- `COMPRESSED_ENCRYPTED` = **bit 1**：仅对 Type 4 合法

**实现将整个 Mode 字节存入 `transfer_mode`**（`GetIdResponse::transfer_mode`，`include/libxcp/response_parser.hpp:188`），头文件旁注按「0x00=走 UPLOAD、0x01=响应内含数据」的**整字节相等**语义使用。

**后果**：当 Slave 置 `COMPRESSED_ENCRYPTED`(bit 1) 时，Mode 值为 `0x02` 或 `0x03`，**按整字节比较会把 bit 1 混入 TransferMode 判断**，且 `COMPRESSED_ENCRYPTED` 信息完全丢失。

**安全修复要求（不得只做掩码）**：

1. `GetIdResponse` 必须分别暴露 `transfer_mode = mode & 0x01` 与 `compressed_encrypted = (mode & 0x02) != 0`，并保留 raw mode 供诊断；
2. bit 7..2 中除规范允许位外出现非零值时，按规范裁决为畸形响应或保留位告警，不得静默改写；
3. 当前库未实现压缩/加密标识数据的解码，因此 `compressed_encrypted=true` 时，上层必须明确返回 `UnsupportedFeature`；
4. **禁止**仅把 `transfer_mode` 改为 bit 0 后继续走现有 `FetchA2lViaUpload()`，否则 Mode=`0x02` 会被误当作普通 UPLOAD 通路并把压缩/加密数据当原始 A2L；
5. 测试至少覆盖 raw Mode `0x00/0x01/0x02/0x03`，验证 TransferMode 与压缩标志正交、未支持格式不会进入普通 A2L 解析。

**缺陷三：Length 未校验 AG 对齐（Table 60 下方原文）**：

> "The parameter **Length** specifies the number of bytes in the identification. If length is 0, the requested identification type is not available. The following rule applies: **Length mod AG = 0**"

即 **Length 必须满足 `Length mod AG = 0`**。实现未做该校验（属健壮性不足，Slave 违规时不应静默接受）。

### 2A.4 裁决结论

| 项 | 原审计定性 | 本轮裁决 |
|---|---|---|
| 响应布局「固化为 XCPlite 方言」 | 高，实现错误 | **不成立** —— 实现即为 Table 60 标准布局 |
| 头文件注释 | 未指出 | **成立，新增** —— 含三处不实陈述并已误导审计 |
| Mode 未按位掩码处理 | 未指出 | **成立，新增** —— bit1 场景下判定失败 |
| `Length mod AG = 0` 未校验 | 未指出 | **成立（低）** —— 健壮性不足 |

**严重度重估：由【高】下调为【中】**（无 wire-level 布局偏差；但 Mode bit 1 场景会影响功能判定，必须按上文显式拒绝未支持的压缩/加密数据，不能宣称对所有标准 Slave 均无互操作影响）。

**同时否决「按标准布局重写解析器」的修复方向** —— 现实现本就符合标准，重写反而会引入偏差。

---

## 2B. F-05／F-06／F-07／F-08 裁决（本轮新增）：**四项全部成立**

> 本节依据**原始 PDF** 复核完成（Markdown 版对应区域排版混乱，不足为凭）。

### 2B.1 共同规范基础：`DAQ_KEY_BYTE`（Table 140／141／142／143）

`DAQ_KEY_BYTE` 由 `GET_DAQ_PROCESSOR_INFO` **响应偏移 7、BYTE 类型**返回（Table 136，PDF 第 178 页）。其位定义（PDF 第 181-183 页）：

| 位 | 字段 |
|---|---|
| **bit 7-6** | `Identification_Field_Type_1` / `_0` |
| **bit 5-4** | `Address_Extension_DAQ` / `_ODT` |
| **bit 3-0** | `Optimisation_STRICT` + `Optimisation_Type_2..0` |

**Table 143 识别字段类型编码（F-06 的裁决依据）**：

| bit 7 | bit 6 | Identification Field Type |
|---|---|---|
| 0 | 0 | Absolute ODT number |
| 0 | 1 | **Relative ODT number, absolute DAQ list number (BYTE)** |
| 1 | 0 | **Relative ODT number, absolute DAQ list number (WORD)** |
| 1 | 1 | **Relative ODT number, absolute DAQ list number (WORD, aligned)** |

**规范原文（PDF 第 182-183 页）**："The Identification_Field_Type flags indicate the type of identification field the slave will use when transferring DAQ packets to the master. **The master has to use the same type** of identification field when transferring STIM packets to the slave."

**Table 142 地址扩展编码（审计未提及，属新增约束）**：

| bit 5 | bit 4 | Address_Extension Type |
|---|---|---|
| 0 | 0 | address extension can be different within one and the same ODT |
| 0 | 1 | address extension to be the same for all entries within one ODT |
| **1** | **0** | **Not allowed（非法组合）** |
| 1 | 1 | address extension to be the same for all entries within one DAQ |

**Table 141 优化方法编码（F-08 相关）**：`0000`=OM_DEFAULT、`0001`=OM_ODT_TYPE_16、`0010`=OM_ODT_TYPE_32、`0011`=OM_ODT_TYPE_64、`1001`/`1010`/`1011`=对应 strict 变体、`0100`=OM_ODT_TYPE_ALIGNMENT、`0101`=OM_MAX_ENTRY_SIZE。

### 2B.2 F-06【高】DTO 相对识别字段布局 —— **裁决：成立（但"私有协议"定性已推翻，见 §2B.2A）**

**规范布局（PDF 第 95-96 页，Figure 51／52／53）**：

原文："the identification field consists of the **PID, containing the relative ODT number**, **DAQ bits, containing the absolute DAQ list number**, and an **optional FILL byte**."

| 模式 | 规范布局（Figure） | 识别字段总长 |
|---|---|---|
| RelativeByte | `PID(相对 ODT) \| DAQ(绝对 DAQ List, BYTE)` | **2 字节** |
| RelativeWord | `PID(相对 ODT) \| DAQ(绝对 DAQ List, WORD)` | **3 字节** |
| RelativeWordAligned | `PID(相对 ODT) \| FILL \| DAQ(WORD)` | **4 字节** |

**规范中不存在固定的 `0xAA` 标志字节**——规范只规定 FILL 是"optional"占位字节，**未约束其取值**；`0xAA` 是 XCPlite 选择的填充值。**故 `0xAA` 并非识别字段的"标志"，而是可自由取值的 FILL**（详见 §2B.2A）。

**实现证据（`src/daq/dto_envelope_decoder.cpp:92-114`）**：

| 模式 | 实现布局 | 与规范对比 |
|---|---|---|
| `RelativeByte`（`:92-104`） | `[relODT][0xAA][DAQ16 LE]`，`header_bytes=4` | **实为 `RelativeWordAligned` 语义（见 §2B.2A）**：`0xAA` 是合法的 FILL 填充值，但 `:97-99` **把它当强制标志校验**——用其它填充值（如 `0x00`）的**合规** Slave 会被 `ThrowMalformed("RelativeByte 识别字段缺少 0xAA 标志")` 拒绝 |
| `RelativeWord`（`:105-113`） | `dto[0]`=DAQ、`dto[1]`=相对 ODT | **字段顺序相反**：规范为 PID 在前；且 DAQ 被**截为 8 bit** |
| `RelativeWordAligned` | 与 `RelativeWord` **走同一分支** | **FILL 字节完全未处理** |

**Fields 顺序是硬性事实**：规范 Figure 51/52/53 均以 `PID` 起始，实现的头文件注释（`include/libxcp/daq/dto_envelope_decoder.hpp:37-40`）却写「低字节 DAQ 号、高字节相对 ODT」——**该注释把实现描述成标准，与真实规范不符**。

---

### 2B.2A 【重要更正】XCPlite 用的**不是私有协议**，而是规范的 Figure 53

> 本条为本轮**追加核实**的结论，**推翻了本节表格中"XCPlite 私有扩展"的定性**，也推翻了审计报告"方言固化"的叙事。**核实方式：直接读 `thirdparty/XCPlite/` 源码中的常量定义与注释。**

**决定性证据一 —— XCPlite 的 `DAQ_KEY_BYTE` 常量与规范 Table 143 逐位对应**（`thirdparty/XCPlite/src/xcp.h:380-388`）：

| XCPlite 常量 | 值 | bit 7-6 | 规范 Table 143 对应类型 |
|---|---|---|---|
| `DAQ_HDR_ODT_DAQB` | `1 << 6` = `0x40` | `01` | Relative ODT number, absolute DAQ list number **(BYTE)** |
| `DAQ_HDR_ODT_DAQW` | `2 << 6` = `0x80` | `10` | Relative ODT number, absolute DAQ list number **(WORD)** |
| `DAQ_HDR_ODT_FIL_DAQW` | `3 << 6` = `0xC0` | `11` | Relative ODT number, absolute DAQ list number **(WORD, aligned)** |

**XCPlite 的常量命名就是规范术语的直译**，且 `thirdparty/XCPlite/src/xcplite.c:2489` 的注释逐字抄录规范描述：

```c
// DAQ_HDR_ODT_FIL_DAQW: Relative ODT number (BYTE), fill byte, absolute DAQ list number (WORD, aligned)
```

**决定性证据二 —— 编码实现与规范 Figure 53 完全一致**（`thirdparty/XCPlite/src/xcplite.c:1616-1621`）：

```c
// ODT header (ODT8,FIL8,DAQ16 or ODT8,DAQ8)
d0[0] = (uint8_t)(odt - DaqListFirstOdt(daq)); /* Relative odt number as byte*/
#if ODT_HEADER_SIZE == 4
    d0[1] = 0xAA; // Align byte
    *((uint16_t *)&d0[2]) = daq;
#else
    d0[1] = (uint8_t)daq;
#endif
```

- `ODT_HEADER_SIZE` 定义为 **4**（`thirdparty/XCPlite/src/xcplite.c:1081`：`#define ODT_HEADER_SIZE 4 // ODT,align,DAQ_WORD header`）
- 故实际布局为 **`PID(相对 ODT) | FILL(0xAA) | DAQ(WORD)`** —— **与规范 Figure 53 `PID | FILL | DAQ` 完全对应**
- **`d0[0]` 是相对 ODT 号（PID 在前）**，与规范一致
- 源码注释 `ODT8,FIL8,DAQ16` 亦与 Figure 53 一致

**决定性证据三 —— `0xAA` 是 FILL 的填充值，规范未规定其取值**

规范中 `FILL` 的全部出现处（`:3722, 3749-3750, 3816, 3822, 3997, 11985`）统称 "an **optional** FILL byte" / "an **extra** FILL byte"，**无任何取值约束**。故 `0xAA` 是合法的实现自由度选择。

**决定性证据四 —— XCPlite 正确设置了地址扩展位**（`thirdparty/XCPlite/src/xcplite.c:2496-2498`）：`DAQ_EXT_FREE (0 << 4)` 与 `DAQ_EXT_DAQ (3 << 4)`。`3 << 4` 正是规范 Table 142 的 `11`（同一 DAQ 内扩展相同），**不是非法的 `10` 组合**。

**结论更正**：

| 原表述 | 更正后 |
|---|---|
| "XCPlite 私有 `[relODT][0xAA][DAQ16 LE]` 布局" | **XCPlite 用标准 `RelativeWordAligned`（Figure 53），`0xAA` 是合法 FILL 值** |
| "拒绝标准 Slave 的合法 DTO（因校验 0xAA）" | **仍然成立，但原因不同**：项目代码把 FILL 当**固定标志值**，而规范允许任意填充值 |

**因此本项目解码器的真实缺陷是**（`src/daq/dto_envelope_decoder.cpp:96-99`）：把规范中**取值自由的 FILL 字节当成强制标志** —— 任何用其他填充值（如 `0x00`）的**合规** Slave 都会被拒绝。

**⚠ 这对修复方向有实质影响**：§2B.5 中的**旧框架"新增标准模式 + 保留 XCPlite 分支"已不成立**（该节正文已同步更正，见 §2B.5 开头的更正块）。**决策目标不变**——仍是"在不牺牲 XCPlite 互操作的前提下把解码器修正到规范"，但**实现路径已改变**：不是"标准 vs 私有"的取舍，而是：

> **本项目需支持规范 Figure 51/52/53 三种模式的完整语义**（含 `FILL` 取值自由、`CTR` 与 `FILL` 合并），而当前实现只覆盖其中一种的子集并附加了错误约束。

**即：不是"加一个私有分支"，而是"把现有模式补全为规范模式 + 去掉错误约束"。**

**新发现的延伸偏差（本轮首次识别）—— `CTR` 与 `FILL` 的合并语义**

规范 PDF 第 97-98 页原文：

> For the identification field type "relative ODT number and absolute DAQ list number (WORD, aligned)", the DTO CTR is **inserted in a different way to keep the alignment without introducing additional fill bytes**. This is achieved by **replacing the fill byte** of the identification field with the counter field **in the first DTO of the DAQ list**, **effectively merging counter field and identification field**.

**Figure 56（PDF 第 98 页）**：

```
First DTO of DAQ list:   PID | CTR(取代 FILL) | DAQ | Data
Other DTOs:              PID | FILL           | DAQ | Data
```

**即：`WORD, aligned` 模式下 CTR 与 FILL 占用同一个字节，并非"识别字段之后另加一段 CTR"。**

**而本项目实现**（`src/daq/dto_envelope_decoder.cpp:135-140`）在识别字段**之后**独立读取 counter：

```cpp
std::size_t off = field;  // 当前指到 counter/timestamp 段起点
if (frame_layout.counter_enabled) {
    counter = dto[off];
    ++off;
}
```

**对 `RelativeWordAligned` + counter 的对端，会在错误位置读取 counter。** 由于 **XCPlite 正是用该模式（`0xC0`）**，此项须与其他 F-06 修复一并处理。

**`CTR` 的其他语义**（Figure 55，PDF 第 97 页）："CTR only in first DTO packet of sample" —— 计数器**仅出现在采样的首个 DTO**，且 STIM 方向语义为"Slave 期望首 ODT 带计数器"。

### 2B.3 F-07【高】DTO 多字节字段固定小端 —— **裁决：成立**

**规范依据**：CONNECT 协商的 Byte Order 驱动多字节参数解释；`GET_DAQ_PROCESSOR_INFO` 的 `DAQ_KEY_BYTE` 与 DTO 识别字段中的多字节 DAQ List 号须按协商字节序解释。

**实现证据**：

- `src/daq/dto_envelope_decoder.cpp:52-59` 仅提供 `ReadLe`（小端）读取原语。
- `src/daq/dto_envelope_decoder.cpp:63-64` 构造时保存 `m_byte_order_`，但 `:92-114` 与 `:143-152`（时间戳读取）**均未使用**。
- 故 `ByteOrder::Motorola` 会话下：⑴ 标准 RelativeWord 的 DAQ List 号；⑵ `RelativeWordAligned` 的 `DAQ16`；⑶ **DTO 时间戳** —— 均被**按小端错误解码**。

**佐证**：`include/libxcp/daq/dto_envelope_decoder.hpp:88-90` 的构造函数注释称 `byte_order` 仅用于「absolute 之外的 WORD 识别读对齐用」，**实际代码连该用途也未实现**（无任何 `m_byte_order_` 消费点）——**成员变量成为死字段**。

**影响**：Motorola/大端 Slave 的 DTO 会被系统性误解码，且**无任何报错**（静默错误，比抛异常更危险）。

### 2B.4 F-08【高】`DAQ_KEY_BYTE` 未贯穿 —— **裁决：成立（措辞需修正）**

**规范要求**（Table 143 下方原文）：Slave 通过 `DAQ_KEY_BYTE` 告知其使用的识别字段类型；Master 必须用同一类型生成 STIM 包。**即 `DAQ_KEY_BYTE` 是 Master 配置 DTO/STIM 布局的权威依据。**

**实现现状**：

- `include/libxcp/protocol_types.hpp:855-863`：`DAQ_KEY_BYTE` **已能解析**（`GetDaqProcessorInfoResponse` 含该字段）。
- `src/measurement/measurement_session.cpp:91-117` `SetEnvelopeMode(IdentificationFieldType, std::size_t)`：识别字段类型与头长**由调用方手工注入**，其注释明确「只能来自会话取证，绝不猜测（B-16）」。
- `src/xcp_master.cpp:559-575`：动态 DAQ 路径**未把 Processor Info 的 Key Byte 转成 `IdentificationFieldType` 后传给 `MeasurementSession`**。

**裁决措辞修正**：审计标题「未贯穿到 DTO 配置」**准确**，但其描述中「`measurement_session.cpp:91-117` 仍要求调用方手工设置 envelope 类型和头宽」应理解为**架构设计如此（取证式注入）**，而非缺陷本身。**真正的缺口是：`DAQ_KEY_BYTE → IdentificationFieldType` 的自动映射不存在，导致"取证"这一步必须由上层人工完成，而库内已有全部信息却不用。**

**这一裁决与 §2B.2 强耦合**：即便补上映射，**现有枚举 `IdentificationFieldType` 各取值的实现布局与规范 Figure 51/52/53 并不一致**（含 `RelativeWordAligned` 的 FILL/CTR 语义缺失，见 §2B.2A 与 §2B.5）。

### 2B.5 修复方向（已获用户确认）

**用户决策（本轮）**：**补全规范模式语义 + 去掉错误约束**（不做彻底替换，不做纯文档声明）。

**⚠ 修复方向已按 §2B.2A 更正**：原表述为"新增标准模式 + 保留 XCPlite 私有分支"，**该框架现已不成立**——XCPlite 用的就是标准的 `RelativeWordAligned`（Figure 53），其 `0xAA` 是合法 FILL 值。**真实任务是"把现有实现补全为规范语义 + 去掉错误约束"，而非"增加一个方言分支"。**

**三条待补的规范语义**：
1. **FILL 取值自由**：删除对 `0xAA` 的强制校验，FILL 仅作占位跳过。
2. **`RelativeWordAligned` 与 `RelativeWord` 分离处理**：前者含 FILL 字节（总长 4），后者无（总长 3），当前二者走同一分支。
3. **`CTR` 与 `FILL` 合并**（新识别，见 §2B.2A）：`WORD, aligned` 模式下首个 DTO 的 CTR **取代** FILL 字节，而当前实现在识别字段之后独立读 CTR —— 位置错误。

**可行性已核实**：架构本就支持——`DtoFrameLayout::identification_field_type` 与 `MeasurementSession::SetEnvelopeMode` 均为**外部注入式**，故补全模式语义**不动注入机制**：

1. **校正 `IdentificationFieldType` 语义**：三种取值（`RelativeByte` / `RelativeWord` / `RelativeWordAligned`）**本就对应规范 Figure 51/52/53**，无需新增"私有"维度；需修正的是各模式的**实现布局与 FILL/CTR 处理**，使其与规范 Figure 逐字段一致。
2. **补 `FILL` 字节处理**：`RelativeWordAligned` 必须从 `RelativeWord` 分支中分离。
3. **修正字段顺序**：标准分支按 `PID(相对 ODT) → DAQ` 解析。
4. **落实字节序**：`m_byte_order_` 必须真正作用于 DAQ List 号与 DTO 时间戳的读取（消除 F-07 的死字段）。
5. **修 `SetEnvelopeMode` 下界校验**（`measurement_session.cpp:105-117`）：`RelativeByte` 的 `minimum_bytes` 现**硬编码为 4U**（实为 `RelativeWordAligned` 的头长）；按规范，`RelativeByte` 应为 **2U**、`RelativeWord` 为 **3U**、`RelativeWordAligned` 为 **4U** —— 须按模式分别校验。
6. **建立 `DAQ_KEY_BYTE` → 识别字段类型映射**：按 Table 143，bit 7-6 直接给出识别字段类型，`DAQ_KEY_BYTE` 由 `GET_DAQ_PROCESSOR_INFO` 偏移 7 取得。**XCPlite 申报 `0xC0`（`DAQ_HDR_ODT_FIL_DAQW`），恰好映射到 `RelativeWordAligned`** —— 映射建立后旧行为可被标准路径自然覆盖。
7. **头注释修正**：`include/libxcp/daq/dto_envelope_decoder.hpp:37-40` 现把 `[relODT][0xAA][DAQ16]` 描述为标准布局，须按规范 Figure 51/52/53 的真实字段顺序重写。

**实施顺序建议**：F-08（映射）→ F-06（布局补全）→ F-07（字节序）。理由：**先有映射才能自动选择识别字段模式**，否则模式无法被正确触发。

**回归风险**：XCPlite 分支必须保持**逐字节行为不变**，现有 `tests/dto_envelope_decoder_test.cpp` 用例应全部继续通过，作为不回归的判据。

### 2B.6 F-05【高】动态 DAQ 忽略 `MIN_DAQ` —— **裁决：成立**

**规范依据（PDF 第 180 页原文，逐字）**：

> **MIN_DAQ is the number of predefined DAQ lists.** For predefined DAQ lists, DAQ_LIST_NUMBER is in the range **[0,1,..MIN_DAQ-1]**.
> **MAX_DAQ-MIN_DAQ** is the number of configurable DAQ lists. For configurable DAQ lists, DAQ_LIST_NUMBER is in the range **[MIN_DAQ,MIN_DAQ+1,..MAX_DAQ-1]**.
> If DAQ_CONFIG_TYPE = dynamic, **MAX_DAQ equals MIN_DAQ+DAQ_COUNT**.

Table 136 字段布局：`2 WORD MAX_DAQ`、`6 BYTE MIN_DAQ`（PDF 第 178 页）。

**实现证据**：

- `src/xcp_master.cpp:547-555` 强制 `specs[i].daq_list == i`，**从 0 开始编号**。
- `src/xcp_master.cpp:559-575` 动态编排仅缓存 Properties，**未消费 `MIN_DAQ`**。

**后果**：对 `MIN_DAQ != 0` 的标准 Slave，合法可配置区间为 `[MIN_DAQ, MAX_DAQ-1]`，而实现从 0 开始 —— **本地拒绝合法编号或错误寻址**。

**新增约束（审计未提）**：`OVERLOAD_MSB` 启用时（`DAQ_PROPERTIES` bit 6-7 编码为 `01`），PDF 第 180 页要求 **`0x00 <= ODT_NUMBER(DAQ with overrun_msb) < 0x7C`** —— ODT 编号上限被 PID 的 MSB 挤占所限制。配置 DAQ 时须一并校验。

---

## 2C. F-10／F-11 裁决（本轮新增）：**成立，但依据须精确措辞**

### 2C.1 共同规范基础：`§7.6.1.2 PRE-ACTION` / `§7.6.1.3 ACTION`（PDF 第 239 页）

**Pre-Action（把 Slave 带入良定义状态）**：
`Wait t7`、`SYNCH`、**`GET_SEED/UNLOCK`**、**`SET_MTA`**、**`SET_DAQ_PTR`**、`START_STOP_x`、`Reinitialise DAQ`

**Action（尝试从错误状态恢复）**：
`Display error`、`Retry other syntax`、`Retry other parameter`、`Use ASAM MCD-2 MC Description File`、`Use alternative`、`Repeat 2 times`、`Repeat ∞ times`、`Restart session`、`Terminate session`

**超时处理的原文规定（PDF 第 241 页）**：

> If the master detects a timeout..., the master has to perform the Pre-Action and Action. This sequence (pre-action, action) has to be tried **2 times**.
> In the usual case, the (pre-action, action) consists of a **SYNCH** command to re-synchronize command execution between master and slave **followed by a repetition of the command**.
> **For some special commands, the pre-action brings the slave in a well-defined state e.g. by sending again SET_MTA or SET_DAQ_PTR before repeating the command.**

**严重度分级（Table 227，PDF 第 240 页）**：S0 Information、S1 Warning/Request、**S2 Resolvable error**、**S3 Fatal error**。

**逐命令恢复矩阵**（PDF 第 246 页起）中与本项相关的两行原文：

```
GET_SEED  timeout t1   SYNCH   repeat 2 times
UNLOCK    timeout t1   SYNCH   repeat 2 times
```

### 2C.2 F-10【中】分段 Seed & Key 超时恢复可能失步 —— **裁决：成立**

**⚠ 依据措辞必须精确**：规范恢复矩阵对 `GET_SEED`/`UNLOCK` 超时**确实规定** Pre-Action=`SYNCH`、Action=`repeat 2 times`。**因此不能声称"实现违反了恢复矩阵"。** F-10 的真实性质是：

> **规范矩阵给出的是逐命令的统一规则，未对「多段序列中途超时」这一场景给出专门处理；而实现采用通用路径（SYNCH + 重放当前段）在该规范空白下会产生失步风险。**

**失步机制的规范依据（两条约束叠加）**：

**约束一 —— 序列合法性（PDF 第 128 页，§7.5.1.9 原文）**：

> The master has to use **GET_SEED(Mode=1) in a defined sequence together with GET_SEED(Mode=0)**. If the master sends a **GET_SEED(Mode=1) directly without a previous GET_SEED(Mode=0)**, the slave returns an **ERR_SEQUENCE** as negative response.

**约束二 —— 延迟校验（PDF 第 130 页，§7.5.1.10 原文）**：

> The master only can send an UNLOCK sequence **if previously there was a GET_SEED sequence**.
> The master has to send the **first UNLOCK** after a GET_SEED sequence with a Length containing the **total length** of the key.
> If the master does not respect this sequence, the slave returns an **ERR_SEQUENCE**.
> **The key is checked after completion of the UNLOCK sequence.**

**`Length` 字段的双重语义（Table 66 / Table 67）**：

| 命令 | 字段 | Mode/位置 | 语义 |
|---|---|---|---|
| GET_SEED | `Length of seed` | **Mode=0** | **总长度** |
| GET_SEED | `Length of seed` | **Mode=1** | **剩余长度** |
| UNLOCK | `Length of key` | **首段** | **总长度** |
| UNLOCK | `Length of key` | **后续段** | **剩余长度** |

**Figure 66 长序列实例（PDF 第 132 页，MAX_CTO=8、seed 19 字节、key 10 字节）**：

```
GET_SEED(00, Resource)  →  0x13, 99 88 77 66 55 44     ← 总长 19，给 6
GET_SEED(01, xx)        →  0x0D, 33 22 11 00 11 22     ← 剩余 13
GET_SEED(01, xx)        →  0x07, 33 44 55 66 77 88     ← 剩余 7
GET_SEED(01, xx)        →  0x01, 99                    ← 剩余 1
UNLOCK(0x0A, 98 76 54 32 10 01)                        ← 首段 Length=总长 0x0A
UNLOCK(0x04, 23 45 67 89)                              ← 次段 Length=剩余 0x04
```

**失步成因**：某一段超时后，Slave 侧的内部段偏移可能已推进或已复位，而 Master 重放的是**自己认为的当前段**。由于**约束二（key 最后才校验）**，错位**不会立即报错**，会一路推进至序列末尾才失败或产生未定义行为——**静默失步比立即报错更危险**。

**实现证据（已逐行核对）**：

- `src/xcp_master.cpp:805-874` 的分段编排**逻辑本身正确**：`:867-869` 的 `length_field = (offset == 0 ? key.size() : remaining)` 正确实现了「首段报总长、后续段报剩余长」；`:817-819` 正确处理 `Length=0`；`:825-838` 按 `Mode=0` 总长循环收满 `Mode=1`。**编排无误，问题在异常路径。**
- `src/command_executor.cpp:646-745` `RunCommand` 的通用恢复：`GET_SEED`/`UNLOCK` **不在** `:731-742` 的专属 Pre-Action 重构列表中（该列表现有 `Upload → RestoreUploadMta()`、`WriteDaq/ReadDaq → RestoreDaqPtr()`），故走通用「`PerformRecovery(cmd)` + 重放同一 `encoded_packet`」路径。
- `src/command_executor.cpp:675-711` 的 `HasUnknownOutcomeOnTimeout` 机制**已正确实现**「后果未知的命令超时后不得重放」的语义（覆盖 DOWNLOAD 等标定变更命令）。**F-10 缺的是将同理应用于 `UNLOCK`** —— 与既有设计一脉相承，非新概念。

**证据边界与工程决策**：具体恢复矩阵对 GET_SEED/UNLOCK timeout 写的是 `SYNCH + repeat 2 times`，并未逐字规定“多段中途超时必须重启完整事务”。因此不能把完整重启表述为规范明文要求。考虑到中间段是否已被 Slave 接收属于未知结果，本项目采用更安全的工程策略：

- GET_SEED/UNLOCK 分段事务中的任一段 timeout 后，禁止由通用 `RunCommand` 盲目重放该段；
- **GET_SEED 任一段 timeout**：清除本地分段状态，执行 SYNCH；SYNCH 成功后允许从 `GET_SEED(Mode=0)` 完整重启，最多 1 次；SYNCH 失败则断开；
- **UNLOCK 非末段 timeout**：因 Slave 可能已缓存部分 Key，单独 SYNCH 不足以证明分段状态已复位；必须先断开并重新连接，再从 `GET_SEED(Mode=0)` 完整重启，最多 1 次；
- **UNLOCK 末段 timeout**：资源可能已经解锁，属于结果未知写操作；禁止自动重放、自动重连后重启或继续受保护操作，立即返回现有 `OperationOutcomeUnknown`，由调用方明确决定是否断开重连；
- 收到明确的协议 ERR 时按错误码结束或进入 BUSY 策略，不把它伪装成 timeout 重启；
- 事务重启上限不能与单命令 `repeat 2 times` 混为一谈；该分段表优先于通用恢复矩阵；
- 文档和测试必须明确：这是解决规范未覆盖的分段未知结果风险的工程策略。

**现有测试缺口**：`tests/seed_key_test.cpp` **无任何 Seed/Unlock 分段超时用例**（审计所述，已核实）。

### 2C.3 F-11【中】错误恢复矩阵只实现部分策略 —— **裁决：成立，但属"实现范围"而非"违规"**

**实现已正确覆盖的恢复策略**（审计认可，已复核）：

| 策略 | 实现位置 | 规范依据 |
|---|---|---|
| Timeout → SYNCH → 重试 | `src/command_executor.cpp:717-741` | PDF 第 241 页；恢复矩阵 `repeat 2 times` |
| UPLOAD 恢复后重建 MTA | `:736-738` `RestoreUploadMta()` | Pre-Action 列表含 `SET_MTA` |
| WRITE_DAQ/READ_DAQ 恢复后重建 DAQ 指针 | `:739-741` `RestoreDaqPtr()` | Pre-Action 列表含 `SET_DAQ_PTR` |
| 后果未知命令不重放 | `:675-711` `HasUnknownOutcomeOnTimeout` | 与「DNLOAD 结果未知」语义一致 |
| EV_CMD_PENDING 重启计时且不重发 | `:278-287, 381-389` | 规范 §7.6.2 计时模型 |

**尚未实现、但矩阵中明列的策略**：

| 规范策略 | 矩阵原文示例 | 实现现状 |
|---|---|---|
| **`ERR_CMD_BUSY` → `wait t7` → `repeat ∞ times`** | `GET_STATUS`/`DISCONNECT`/`GET_ID`/`SET_REQUEST`/`GET_SEED`/`UNLOCK` 等**几乎每个命令**都有此行 | **未实现**；`tests/recovery_test.cpp:540-550` 还**显式锁定"BUSY 不重试"**的当前行为 |
| `ERR_PGM_ACTIVE` → `wait t7` → `repeat ∞ times` | `DISCONNECT`/`SET_REQUEST`/`GET_SEED`/`UNLOCK` 等 | 未实现 |
| `ERR_CMD_SYNTAX` → `retry other syntax` | `GET_COMM_MODE_INFO`/`GET_ID`/`SET_REQUEST` 等 | 未实现（直接抛错） |
| `ERR_RES_TEMP_NOT_A.` → `display error` + `repeat` | 多命令 | 未实现 |
| `ERR_CMD_UNKNOWN` → **`restart session`** | `SYNCH` 行 | 未实现（§1.11 已裁定不存在整体版本降级） |
| `ERR_OUT_OF_RANGE` → `retry other parameter` | `CONNECT`/`SET_REQUEST` | 未实现 |
| 动态 DAQ **`Reinitialise DAQ`** | Pre-Action 列表 | 未实现 |

**逐命令恢复矩阵原文样例（PDF 第 246 页）**：

```
GET_STATUS  timeout t1          SYNCH        repeat 2 times
            ERR_RES_TEMP_NOT_A. display error repeat
SYNCH        timeout t1          -            repeat 2 times
            ERR_CMD_SYNCH       -            –
            ERR_CMD_UNKNOWN     -            restart session
SET_REQUEST timeout t1          SYNCH        repeat 2 times
            ERR_CMD_BUSY        wait t7      repeat ∞ times
            ERR_PGM_ACTIVE      wait t7      repeat ∞ times
            ERR_CMD_SYNTAX      -            retry other syntax
            ERR_OUT_OF_RANGE    -            retry other parameter
GET_SEED    timeout t1          SYNCH        repeat 2 times
            ERR_CMD_BUSY        wait t7      repeat ∞ times
            ERR_CMD_SYNTAX      -            retry other syntax
            ERR_OUT_OF_RANGE    -            retry other parameter
```

**裁决措辞修正**：规范超时章节使用 “master has to perform the Pre-Action and Action”，恢复矩阵又为具体命令列出动作，因此不能笼统地把全部策略降格为“可选增强”。本轮按以下边界处理：

- 对本库公开支持的命令，矩阵中能够确定、安全自动化且不需要业务参数重选的动作，应视为符合性工作；
- `retry other syntax`、`retry other parameter` 等需要调用方决策的动作，应通过结构化错误暴露给调用方，而不是库内猜测；
- `repeat ∞ times` 在协议语义上表示持续等待资源，但库实现必须允许用户取消、断连和总体 deadline，不能形成不可中断的同步调用；
- 本轮纳入 `ERR_CMD_BUSY → wait t7 → retry`，同时提供取消/总体 deadline；其它矩阵项进入显式能力表，未实现项必须在 API 文档标明。

因此 F-11 定性为“恢复矩阵覆盖不完整”：其中一部分是本轮合规补齐，一部分是需要调用方参与的受限能力，不能统一标为可选或违规。

**新增约束（审计未提）**：`ERR_ACCESS_LOCKED` 的语义（PDF 第 119 页原文）：

> The Resource protection flags indicate that all commands allocated to the respective resource are protected and will return an **ERR_ACCESS_LOCKED** upon an attempt to execute the command without a previous successful GET_SEED/UNLOCK sequence.

以及 `ERR_SEQUENCE` 的**其他触发点**（`:774-784`）：DAQ 配置序列中被 ALLOC_DAQ / ALLOC_ODT / FREE_DAQ 等交错打断时同样返回 —— **F-10 的失步风险同样适用于 DAQ 配置序列**，但审计未涉及。

> **O-1 观察项（处置已定，用户 m01397）**：记为文档记录，**不纳入本轮批次**。理由：与当前 12 项修复目标不同源（审计未涉及），混入会扩大改动面。

---

## 2D. F-12 裁决（本轮新增）：**成立，规范给出三条硬约束**

### 2D.1 规范依据：`GET_DAQ_RESOLUTION_INFO`（Table 145，PDF 第 184-187 页）

**响应字段布局**：

| 偏移 | 类型 | 字段 |
|---|---|---|
| 0 | BYTE | Packet ID: 0xFF |
| 1 | BYTE | `GRANULARITY_ODT_ENTRY_SIZE_DAQ` |
| 2 | BYTE | `MAX_ODT_ENTRY_SIZE_DAQ` |
| 3 | BYTE | `GRANULARITY_ODT_ENTRY_SIZE_STIM` |
| 4 | BYTE | `MAX_ODT_ENTRY_SIZE_STIM` |
| 5 | BYTE | `TIMESTAMP_MODE` |
| 6 | WORD | `TIMESTAMP_TICKS` |

### 2D.2 硬约束一：ODT Entry 的地址与长度对齐（PDF 第 184 页原文）

> The possible values for `GRANULARITY_ODT_ENTRY_SIZE_x` are **{1,2,4,8}**.
> For the **address** of the element described by an ODT entry:
> `Address[AG] mod (GRANULARITY_ODT_ENTRY_SIZE_x[BYTE] / AG[BYTE]) = 0`
> For every **size** of the element described by an ODT entry:
> `SizeOf(element described by ODT entry)[AG] mod (GRANULARITY_ODT_ENTRY_SIZE_x[BYTE] / AG[BYTE]) = 0`
> The `MAX_ODT_ENTRY_SIZE_x` parameters indicate the **upper limits** for the size:
> `SizeOf(element described by ODT entry) <= MAX_ODT_ENTRY_SIZE_x`

**三条可实施判据**：

| # | 约束 | 表达式 |
|---|---|---|
| C1 | 粒度取值合法 | `GRANULARITY_ODT_ENTRY_SIZE_x ∈ {1,2,4,8}` |
| C2 | 粒度可由当前 AG 表示 | `GRANULARITY >= AG && GRANULARITY mod AG = 0`；否则能力组合不可执行，配置前失败 |
| C3 | **Entry 地址按 AG 换算后须对齐粒度** | `ratio = GRANULARITY / AG`，且仅在 C2 成立后计算；`Address[AG] mod ratio = 0` |
| C4 | **Entry 元素长度须对齐粒度** | `Size[AG] mod ratio = 0` |
| C5 | **Entry 长度不得超上限** | `Size ≤ MAX_ODT_ENTRY_SIZE_x` |

**注意 C2～C4 的单位与安全语义**：`Address[AG]` 与 `Size[AG]` 均**以 AG 为单位的元素数**（与 §1.5 一致），而 `GRANULARITY` 是**字节数**。实现必须先验证 `GRANULARITY >= AG && GRANULARITY % AG == 0`，再计算 `ratio`；禁止先做整数除法后执行 `% ratio`，避免 `GRANULARITY < AG` 时出现除零/模零。无法表示的 Slave 能力组合统一按现有 `ErrorCategory::MalformedPacket` 处理，不新增同义错误类别，且不得继续下发 DAQ 配置。**这正是 F-01 同一族错误的延伸。**

### 2D.3 硬约束二：`TIMESTAMP_MODE` 位域与非法组合（Table 146/147/148）

**Table 146 位掩码结构（PDF 第 185 页）**：

| 位 | 字段 |
|---|---|
| bit 7-4 | `Unit_3..Unit_0` |
| **bit 3** | **`TIMESTAMP_FIXED`** |
| bit 2-0 | `Size_2..Size_0` |

**Table 147 Timestamp size 编码**：

| `Size_2 Size_1 Size_0` | Timestamp size [bytes] |
|---|---|
| `0 0 0` | No timestamp |
| `0 0 1` | 1 |
| `0 1 0` | 2 |
| **`0 1 1`** | **Not allowed（非法组合）** |
| `1 0 0` | 4 |

**Table 148 Timestamp unit 编码**：`0000`=1ns、`0001`=10ns、`0010`=100ns、`0011`=1µs、`0100`=10µs、`0101`=100µs、`0110`=1ms、`0111`=10ms、`1000`=100ms、`1001`=1s、`1010`=1ps、`1011`=10ps、`1100`=100ps。**`1101`~`1111` 未列出 → 保留值，须按非法处理。**

**`TIMESTAMP_FIXED` 语义（PDF 第 185 页原文）**："the slave **always** will send DTO packets in time-stamped mode."

**有效性前提（PDF 第 185 页原文）**：

> If the slave does not support a time-stamped mode (**no `TIMESTAMP_SUPPORTED` in GET_DAQ_PROCESSOR_INFO**), the parameters `TIMESTAMP_MODE` and `TIMESTAMP_TICKS` are **invalid**.

**即：应先查 `DAQ_PROPERTIES` 的 `TIMESTAMP_SUPPORTED`（bit 4），未置位则不得使用 `TIMESTAMP_MODE`/`TIMESTAMP_TICKS`。**

### 2D.4 硬约束三：时间戳截断语义与环绕（PDF 第 185-186 页）

**截断规则**：

> When the size of the timestamps ... is **less than** the timestamp size of the data acquisition clock, the DAQ timestamps correlate to the **least significant bytes** of the timestamp size of the clock... the most significant bytes are **truncated**.

**时钟本质**：数据采集时钟是 "a **free running counter**, which is **never reset or modified and wraps around if an overflow occurs**" —— **环绕是规范预期行为，不是异常**。

**物理时间换算等式（可直接实施）**：

```
t_physical(k+1) = t_physical(k) + [(t_protocol(k+1) − t_protocol(k)) × TIMESTAMP_UNIT × TIMESTAMP_TICKS]
```

**⚠ 实施注意**：单独一个 DTO 时间戳只能得到**相对时间**（需两个采样点做差）；转换为绝对物理时间依赖 `t_physical(k)` 的锚点。且**环绕回退时差值须按无符号模运算处理**，否则会出现负增量。

### 2D.5 实现偏差与裁决

**实现证据（已回读当前源码确认）**：

- `src/response_parser.cpp:353-373`：`GET_DAQ_RESOLUTION_INFO` 的 granularity、max entry size、timestamp unit/ticks/fixed **均已解析**。
- `src/xcp_master.cpp:474-490, 577-592`：配置层**主要只使用 timestamp width**。

**未完整实施的校验**：

| 约束 | 规范要求 | 实现现状 |
|---|---|---|
| C1 粒度取值 `{1,2,4,8}` | 硬性 | 未校验 |
| C2 粒度可由当前 AG 表示 | 本地安全前提 | **未校验** |
| C3 Entry 地址对齐 | 硬性等式 | **未校验** |
| C4 Entry 长度对齐 | 硬性等式 | **未校验** |
| C5 `MAX_ODT_ENTRY_SIZE` 上限 | 硬性 | **未校验** |
| `Size=011` 非法组合 | Not allowed | 未校验 |
| `Unit=1101..1111` 保留值 | 非法 | 未校验 |
| `TIMESTAMP_SUPPORTED` 前置 | 有效性前提 | 未校验 |
| `TIMESTAMP_FIXED` 语义 | Slave 恒发时间戳 | **未消费** |
| `ALTERNATING` + `TIMESTAMP` 组合 | 组合约束 | 未校验 |

**裁决：成立。** 定性维持审计的自我克制表述——审计原文称「Slave 返回标准错误时不会被吞，因此**更适合定性为健壮性不足，而非必然的 wire-level 违规**」。**本轮认可该定性**：F-12 属**前置校验缺失**（本地不拦截非法配置，交由 Slave 报错），**不是静默错误**。

**但 C2～C4 的缺失值得单列**：它们是**可本地判定的等式**（不依赖 Slave 反馈），且与 F-01 同类（AG 单位换算）。**若 F-01 修复时统一处理 AG 换算，C2～C4 可顺势一并补上，边际成本很低。**

**新增观察项（审计未提）**：`OVERLOAD_MSB` 启用时 ODT 编号上限 `< 0x7C`（见 §2B.6）；`Address_Extension` 的 `10` 为非法组合（见 §2B.1）。

> **O-2 范围修正**：`OVERLOAD_MSB` 的 ODT 编号限制直接影响 F-05，`Address_Extension` 非法组合直接影响 F-12，且二者均可在本地确定性校验，故**纳入 E2 批次**。不再仅作文档观察项。

---

## 3. 副作用与一致性影响（实施前必读）

### 3.1 F-01 的连锁影响

`XcpAddress40::Advance` 的语义变化会波及所有依赖地址推进的路径。实施前须完成**调用点普查**，确认每一处对"address"的单位预期一致；**不得出现同一函数内混用元素地址与字节地址**。若确需字节视角，必须在边界处显式换算（除以 AG）并集中注释说明。

### 3.2 F-02 的连锁影响

对齐字节的引入会改变 CTO 长度。须核查：
- 长度上限校验是否随之调整（`kMaxElementsPerField` 与 `(MAX_CTO-2)/AG` 的关系）；
- 分块发送（block mode）路径的元素数上限计算；
- 全仓检索并更新所有固化 `CTO 长度 = 2 + data.size()` 假设的测试；检索结果须进入 C 批次实施记录。

### 3.3 F-04 的连锁影响

`store_daq_req` 语义修正后，所有依赖该标志的上层逻辑行为会改变。C 批次修改前必须完成该字段的生产/测试消费点静态清单，并为每个消费点增加 bit1/bit2 独立行为测试；清单关闭前不得实施替换，以消除此前依赖错误语义而“巧合正确”的风险。

### 3.4 与既有的"解析但不使用"模式的区别

F-04 属于**解析错位**（值读错位），与 F-05 的**解析正确但未使用**（`min_daq` 已解析，`src/xcp_master.cpp:547-555` 仍硬性要求从 0 连续编号）是**两类不同缺陷**，修复手法与验证方式均不同，不可混为一谈。

---

---

## 3A. 跨模块同步影响评估（examples / tools / tests / adapter）

> **本节为第 3 节之后追加的同步影响面评估**，回答"修复 F-06／F-07／F-08 时，哪些非 `src/`、`include/` 的模块必须一并改动"。
> **核实方式**：全仓检索 `IdentificationFieldType`、`SetEnvelopeMode`、`DtoFrameLayout`、`header_bytes` 的全部定义点与调用点，逐处读取确认。

### 3A.1 【重大发现】存在**两套并行的 DTO 信封解码实现**，且各有独立的布局假设

| 路径 | 类型定义 | 布局计算实现 | 校验 FILL 值？ | 是否本次修复目标 |
|---|---|---|---|---|
| **核心**（libxcp） | `calmcar::xcp::DtoFrameLayout`（`include/libxcp/daq/dto_envelope_types.hpp:48`）<br>枚举 `calmcar::xcp::IdentificationFieldType`（`:28`） | `src/daq/dto_envelope_decoder.cpp:92-114` | **是**（`:97-99` 强制 `0xAA`） | ✅ 主要目标 |
| **桥接**（a2lbridge） | `calmcar::xcp::a2l::DtoFrameLayout`（`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/daq_layout.hpp:127`）<br>枚举 `DaqInfo::IdFieldType`（`:48`） | `thirdparty/a2l-sdk/a2lbridge/src/daq_layout_impl.cpp:48-60` | **否**（只算长度） | ✅ **须一并修正**（经用户确认：`a2lbridge` 为本项目自研代码，非第三方库） |

**两者的布局假设高度巧合地一致**（均为 `RelativeByte` +1 字节、`RelativeWord*` +2 字节），但**类型完全独立、互不引用**——`calmcar::xcp::DtoFrameLayout` 与 `calmcar::xcp::a2l::DtoFrameLayout` 是**同名的两个不同结构体**。

**这条并行实现必须一并修正**（实施约束已解除）：`tests/a2l_golden_test.cpp:1480-1493` 的 `MakeEnvelope` 与 `thirdparty/a2l-sdk/a2lbridge/src/daq_layout_impl.cpp:48-60` **镜像了同一套（错误的）假设**。

**⚠ 归属澄清（用户 m01346 确认）**：`a2lbridge` **不是第三方库，可自由修改**。AGENTS.md 第 97 行的"针对 thirdparty 不能在没有明确指示的情况下修改任何文件"针对的是**真正的外部依赖**（如 `XCPlite`、`a2llib`）。

**但须注意目录内的边界**：`thirdparty/a2l-sdk/` 同时包含两类内容——

| 子路径 | 性质 | 本轮是否可改 |
|---|---|---|
| `thirdparty/a2l-sdk/a2lbridge/` | **本项目自研桥接层**（22 个文件全入库） | ✅ **可改**（本轮修复目标） |
| `thirdparty/a2l-sdk/src/`、`include/liba2l/` | `liba2l` 本体及其日志/导出封装（`liba2l_export.cpp`、`a2llogstream_boost_locale.cpp`、`dll_main.cpp` 等） | ❌ **不应改**（本轮修复不涉及） |

**故 N-04 的处置为：修正 `a2lbridge/src/daq_layout_impl.cpp` 与 `a2lbridge/include/libxcp/a2l/daq_layout.hpp`，但不触碰同目录下的 `liba2l` 本体。**

**已核实的调用关系**：`tests/a2l_golden_test.cpp:28` include `libxcp/a2l/a2l_bridge.hpp`，全程走 `A2lBridge`（a2lbridge 实现），**不经过核心 `DtoEnvelopeDecoder`** —— 故修剪核心解码器**不会**影响 `A2lGolden` 套件。

### 3A.2 生产代码中**唯一**的模式配置源是 adapter 层

**核实结论**：核心解码器的 `IdentificationFieldType` 在生产路径上**只有两个注入点**：

| 注入点 | 位置 | 注入的模式 |
|---|---|---|
| adapter 层（正常路径） | `adapter/a2l/measurement_runtime_profile.cpp:279-280`（`ApplyMeasurementRuntimeProfile` → `SetEnvelopeMode`） | `profile.identification_field_type`（来自 `XcpliteMeasurementRuntimeProfile`） |
| tools 兜底路径 | `tools/xcp_master/main.cpp:2839`（无 profile 时） | 硬编码 `Absolute, 1` |

**关键实体** `adapter/a2l/measurement_runtime_profile.hpp:66-73`：

```cpp
struct XcpliteMeasurementRuntimeProfile {
    IdentificationFieldType identification_field_type{
        IdentificationFieldType::RelativeByte};   // ← 模式名错误
    std::uint8_t identification_header_bytes{4};  // ← 头长实为 RelativeWordAligned
    ...
};
```

**即：用错误的枚举名（`RelativeByte`）表达了正确的布局（4 字节 `PID|FILL|DAQ16`）。** 按规范，4 字节的 `PID | FILL | DAQ` 应命名为 **`RelativeWordAligned`**。

**这一处是修复的枢纽**：`BuildXcpliteMeasurementRuntimeProfile`（`adapter/a2l/measurement_runtime_profile.cpp:183-185`）已正确校验对端申报 `DAQ_KEY_BYTE.identification_field_type == 3U`（即 `0xC0 >> 6`，规范 Table 143 的 `11` = WORD aligned），**但随后映射到了错误的枚举值**。

### 3A.3 逐模块同步影响清单

| 模块 | 文件:行 | 现状 | 是否需同步 | 理由 |
|---|---|---|---|---|
| **adapter** | `adapter/a2l/measurement_runtime_profile.hpp:68` | `RelativeByte` 默认值 | ✅ **必须** | 改为 `RelativeWordAligned`，与 `0xC0` 语义对齐 |
| **adapter** | `adapter/a2l/measurement_runtime_profile.cpp:183-185` | 校验 code==3 即放行 | ✅ **必须** | 校验通过后显式映射到 `RelativeWordAligned`；错误文案"不是 XCPlite RelativeByte 方言"同步修正 |
| **tools** | `tools/xcp_master/main.cpp:2839-2840` | 兜底 `Absolute, 1` | ❌ **无需修改，已核查** | 兜底路径本身符合通用 Absolute 默认；实施时通过静态调用点清单确认不存在其它硬编码 4 字节假设 |
| **tools** | `tools/xcp_master/main.cpp:2767-2799` | `identification_bytes = profile.Value().identification_header_bytes` | ❌ 无需 | 走 adapter 返回的 profile，随 adapter 自动修正 |
| **examples** | `examples/xcp_integration_demo/xcp_integration_demo.cpp:76` | 默认 `RelativeWord` + `id_header_bytes=0` | ✅ **必须** | CLI 增加 `relative-word-aligned` 选项（`:304-310` 当前只接受 3 种），使示例能表达规范 Figure 53 |
| **examples** | `examples/measurement_demo/measurement_demo.cpp:17` | 注释写 `SetEnvelopeMode(RelativeByte,4)` | ✅ **必须**（仅注释） | 注释随枚举语义更正，否则误导 |
| **tests** | `tests/dto_envelope_decoder_test.cpp:8-9, 69-107` | 3 个用例固化 `RelativeByte`=[relODT][0xAA][DAQ16]、`RelativeWord`=低字节 DAQ | ✅ **必须** | **这是错误行为的回归锁**，须改写为标准 Figure 51/52/53 |
| **tests** | `tests/dto_envelope_decoder_test.cpp:121-142` | `RelativeWordAligned` 用 `dto[1]` 当相对 ODT | ✅ **必须** | 与规范 `PID\|FILL\|DAQ` 矛盾 |
| **tests** | `tests/measurement_session_test.cpp:289` | 断言 `RelativeByte + 3U` 抛异常 | ✅ **必须** | 依赖 `minimum_bytes=4U`；改为 2U 后该断言失效，须改用 `RelativeByte + 1U` |
| **tests** | `tests/xcplite_daq_test.cpp:138-146` | 注释 + 布局标注 `[ODTrel][0xAA][DAQ16 LE]`、`RelativeByte` | ✅ **必须** | 模式名与注释须更正为 `RelativeWordAligned`；**但线格式本身不变**，用例应继续通过 |
| **a2lbridge** | `thirdparty/a2l-sdk/a2lbridge/src/daq_layout_impl.cpp:48-60` | `RelativeByte` +1、`RelativeWord*` +2 | ✅ **必须** | 与核心路径镜像同一错误假设；**用户 m01346 确认该目录可改**（§3A.1） |
| **a2lbridge** | `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/daq_layout.hpp:48-61, 127-132` | `IdFieldType` 枚举与 `DtoFrameLayout` 定义 | ✅ **必须** | `DtoFrameLayout` 必须显式表达 FILL/对齐语义，并与核心布局使用相同枚举值、头长和 golden；禁止继续保留镜像错误定义 |
| **tests** | `tests/a2l_golden_test.cpp:1480-1493` | `MakeEnvelope` 镜像布局假设 | ✅ **必须**（随 a2lbridge 修正） | 走 a2lbridge 路径；bridge 改为规范布局后，本处填充长度须同步调整 |
| **tests** | `tests/xcplite_measurement_test.cpp:188-192` | 本地 `MeasurementRuntimeProfile` 硬编码 `RelativeByte, 4U` | ✅ **必须** | 改为调用 adapter 工厂函数覆盖真实路径，避免本地硬编码继续掩盖映射错误 |
| **tests** | `tests/xcp_slave_demo_integration_test.cpp:721, 1238-1240` | 走 `BuildXcpliteMeasurementRuntimeProfile` | ❌ 无需 | 随 adapter 自动修正 |

### 3A.4 新发现的缺陷（本轮 review 首次识别，审计报告与本方案此前均未记载）

**缺陷 1 — `SetEnvelopeMode` 下界校验与规范不符**（`src/measurement/measurement_session.cpp:98-112`）：

```cpp
case IdentificationFieldType::RelativeByte:      minimum_bytes = 4U;  break;  // 规范应为 2U
case IdentificationFieldType::RelativeWord:
case IdentificationFieldType::RelativeWordAligned: minimum_bytes = 2U; break;  // 规范应为 3U / 4U
```

- `RelativeByte` 的 4U 实为 `RelativeWordAligned` 的头长（历史混淆的直接痕迹）；
- `RelativeWord` 与 `RelativeWordAligned` **共用 2U**，但规范分别为 **3** 与 **4**（`RelativeWordAligned` 多一个 FILL 字节）。

**正确值**（按规范 Figure 51/52/53）：`RelativeByte = 2U`、`RelativeWord = 3U`、`RelativeWordAligned = 4U`。

**缺陷 2 — 头文件注释与实现自相矛盾，且都不符规范**（`include/libxcp/daq/dto_envelope_decoder.hpp:36-40`）：

```
 *  - RelativeByte：byte0 = 相对 ODT 号，byte1 = DAQ 扩展字节，净荷自 byte2；
 *  - RelativeWord / RelativeWordAligned：低字节 DAQ 号、高字节相对 ODT（Intel），
 *    净荷自 byte2；Aligned 模式要求帧长相对扩展表 4 字节对齐。
```

- 注释称 `RelativeByte` 的 byte1 是 **"DAQ 扩展字节"**，但实现 `src/daq/dto_envelope_decoder.cpp:97` 校验它是 **`0xAA` 标志** —— **注释与实现直接冲突**；
- 注释称 `RelativeWord` 是"低字节 DAQ、高字节相对 ODT"，与规范 **`PID`（相对 ODT）在前** 的 Figure 52 相反；
- 注释**完全没有**提到 `RelativeWordAligned` 的 FILL 字节。

**缺陷 3 — `CTR`/`FILL` 合并语义缺失**（见 §2B.2A）：`WORD, aligned` 模式下首个 DTO 的 CTR 取代 FILL 字节，而 `src/daq/dto_envelope_decoder.cpp:135-140` 在识别字段之后独立读取 CTR。

### 3A.5 实施顺序建议（含同步项）

```
① 核心枚举语义澄清（dto_envelope_types.hpp 注释）
   ↓
② 核心解码器布局补正 + 去掉 0xAA 强制校验（dto_envelope_decoder.cpp）
   ↓
③ SetEnvelopeMode 下界校验改正（measurement_session.cpp:98-112）
   ↓
④ adapter 映射改正（measurement_runtime_profile.hpp:68 / .cpp:183-185）
   ↓
⑤ a2lbridge 并行实现同步改正（daq_layout_impl.cpp:48-60；**不触碰同目录 liba2l 本体**）
   ↓
⑥ tests 同步更新（dto_envelope_decoder_test / measurement_session_test /
   xcplite_daq_test / a2l_golden_test）
   ↓
⑦ examples 补 relative-word-aligned 选项 + 注释更正
```

**回归护栏**：`XcpliteIntegration`（跨进程真实对端）必须全程保持通过——它是"线格式未变"的最终证据。

---

## 4. 版本发现、公开 API 与恢复策略的冻结设计约束

以下为复核中确立的设计约束，须在**批次 B1/B2/D/F** 实施；本节冻结实施边界，本次文档更新不修改代码。

### 4.1 版本证据来源与可信度

| 来源 | 可得信息 | 可信度与局限 |
|---|---|---|
| CONNECT 响应 Position 6/7 | 协议层主版本、传输层主版本 | 运行时真值，但**无 Minor**（`:373`） |
| **GET_VERSION 响应** | 协议层 Major/Minor + 传输层 Major/Minor | 完整运行时版本；命令为 optional |
| A2L/IF_DATA Protocol Layer | 文件声明的协议版本 | 只可作辅助证据；当前 a2lbridge 将协议版本硬编码为 `0x0100`，修正前不得作为 Minor 证据 |
| A2L `XCP_ON_UDP_IP` | Ethernet TL 版本、地址、端口、`PACKET_ALIGNMENT`、Optional TL Subcommand（若存在） | TL 与布局的静态证据；必须保留来源，且与 GET_VERSION/显式配置冲突时不得静默覆盖 |
| 调用方显式配置 | 预期或强制 PL/TL 版本、无 A2L 时的 Alignment | 属部署配置，不是 Slave 自报告；与运行时 Major 冲突或与可信 A2L Alignment 冲突时必须失败 |

**结论**：仅凭 CONNECT 不足以区分 V1.3/V1.4/V1.5。GET_VERSION 是首选补充，但其 optional 属性决定系统必须能表达“Minor 未确认”，不得伪造一个精确版本。

若要启用 A2L 作为 fallback，批次 B1 必须先消除 `thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:419-425` 的 `0x0100` 硬编码，并证明版本值确实来自当前 ECU 的 IF_DATA。若现有 liba2l 数据模型无法可靠提供该字段，则本轮将 `A2l` 来源保持为 Disabled/Unavailable，继续使用 `1.x-conservative`，不得把硬编码值包装成“可信证据”。

### 4.2 版本解析状态机与探测失败策略

GET_VERSION 为 `0xC0` 族命令。探测必须同时处理：
- **`ERR_CMD_UNKNOWN(0x20)`** —— Slave 不认识 `0xC0` 族（`:6990`）；
- **`ERR_SUBCMD_UNKNOWN(0x34)`** —— Slave 认识 `0xC0` 族但不认识 `0x00` 子命令（`:7009`）。

两者均表示“GET_VERSION 不可用”，但**不能据此推定具体 Minor**。建立以下不可混淆的冻结状态：

| 场景 | ReportedVersion | EffectiveVersion | VersionSource | 处理 |
|---|---|---|---|---|
| GET_VERSION 返回 1.3/1.4/1.5 | 返回原值 | 同返回值 | `GetVersion` | 按对应版本能力运行 |
| 返回同 Major、Minor > 5 | 保留原值 | 封顶 1.5 | `GetVersion`，并设置 `capped=true` | 告警；禁止未知高版本能力 |
| 返回 Major != 1 | 保留原值 | 无 | `GetVersion` | `UnsupportedVersion`，连接失败 |
| 返回 `0x20`/`0x34`，调用方提供可信 1.3/1.4/1.5 配置 | CONNECT Major=1、Minor=unknown | 配置值 | `Configured` | 记录 GET_VERSION 不可用；配置与 CONNECT Major 冲突则失败 |
| 返回 `0x20`/`0x34`，存在已修正且可追溯的 A2L 版本证据 | CONNECT Major=1、Minor=unknown | A2L 声明值 | `A2l` | 仅作次级证据；与 CONNECT Major 冲突则失败 |
| 返回 `0x20`/`0x34`，无可信 Minor 证据 | Major=1、Minor=unknown | `1.x-conservative` | `ConnectMajorOnly` | 只启用经 V1.3～V1.5 交集验证的能力；版本敏感功能需显式阻断或逐特性探测 |
| timeout/传输错误/畸形响应 | unknown | 无 | `ProbeFailed` | **连接失败或返回可诊断错误，不得伪装成低版本** |
| A2L 与 GET_VERSION 冲突 | 两者均保留 | GET_VERSION 为运行时裁决 | `GetVersion` | 记录冲突；影响布局/地址解释时阻断相应功能 |

`1.x-conservative` 是能力策略，不是伪造的版本号；公共 API 必须能区分“确认 V1.3”和“Minor 未确认”。版本快照还应携带独立的 `Confidence`：GET_VERSION 确认为 `RuntimeConfirmed`，显式配置为 `Configured`，可信 A2L 为 `StaticEvidence`，仅 CONNECT Major 为 `MajorOnly`；`Capped` 作为额外状态标志而非伪造的来源。断连时清空全部版本证据，重连必须重新探测。

#### 4.2.1 `1.x-conservative` 能力基线（冻结）

Minor 未确认时启用命名能力集 `Xcp13CoreBaseline`，但 **Reported/Effective Minor 仍保持 unknown**，不得把该名称写成“Slave=V1.3”。能力表如下：

| 能力组 | Minor unknown 行为 | 前置证据/限制 | 测试 ID |
|---|---|---|---|
| CONNECT、DISCONNECT、GET_STATUS、SYNCH | **Allow** | 仅使用 CONNECT 已返回的 Major、AG、Byte Order、MAX_CTO/MAX_DTO；F-04 位语义按已确认 Table 49 | `VersionUnknown.CoreSession` |
| GET_COMM_MODE_INFO、GET_ID | **Allow/Capability-gated** | GET_COMM_MODE_INFO 仅在 CONNECT optional 位声明时发送；GET_ID 使用规范布局，压缩/加密位按 F-03 拒绝 | `VersionUnknown.StandardQueries` |
| SET_MTA、UPLOAD、SHORT_UPLOAD、DOWNLOAD、SHORT_DOWNLOAD、MODIFY_BITS | **Allow/Resource-gated** | 必须通过本轮 AG、对齐、容量和 MTA 修复；Optional 命令收到 UNKNOWN 时只走已冻结替代路径 | `VersionUnknown.Memory` |
| GET_SEED、UNLOCK | **Allow/Protection-gated** | 仅在资源保护需要时使用；严格执行 §2C.2/§4.6 分段恢复表 | `VersionUnknown.Security` |
| CAL/PAG 命令 | **Allow/Resource-and-A2L-gated** | 仅在 CONNECT 资源位与可信静态描述共同表明可用时开放；不探测写类命令 | `VersionUnknown.CalPag` |
| DAQ 查询、动态配置、DTO 解码 | **Allow/Runtime-capability-gated** | 必须先取得 Processor/Resolution/DAQ_KEY_BYTE/Event/List 查询结果；按返回能力驱动，不依据 Minor 猜测；未取得唯一布局时阻断 DTO | `VersionUnknown.DaqBaseline` |
| `LEVEL_1_COMMAND` 除 GET_VERSION 外的子命令、DAQ Packed Mode 及其它仅在高版本出现/未完成跨版本证据的能力 | **Block** | 返回 `UnsupportedFeature`；禁止内容探测式尝试 | `VersionUnknown.HighVersionBlocked` |
| Ethernet Header/LEN/CTR 单 Frame | **Allow** | TL Major=1；不依赖 Tail/多 Frame | `TransportUnknown.SingleFrame` |
| Tail、多 Frame Datagram、16/32-bit Alignment | **Evidence-gated** | 必须有唯一可信 Alignment；Unknown 时阻断，不从 TL Minor 推导 | `TransportUnknown.AlignmentGated` |
| IPv6、四个 Optional TL Subcommand | **Block** | 本阶段 Profile 明确 Unsupported | `TransportUnknown.OptionalBlocked` |

所有 `Allow/Capability-gated` 分支必须有对应的命令码/资源位/查询结果条件，不能把“未知命令后再换布局解析”作为 Probe。新增命令进入公共 API 时必须先更新本表与对应测试 ID；否则 Minor unknown 下默认 Block。

### 4.2A 传输层状态必须与协议层状态分离

同一次 GET_VERSION 响应同时提供 PL 与 TL 的完整版本，但两者必须写入两个独立快照：

```text
ProtocolVersionSnapshot {
  reported, effective, source, confidence, capped
}
TransportVersionSnapshot {
  kind=UdpIp, reported, effective, source, confidence, capped,
  packet_alignment, alignment_source,
  ipv4=true, ipv6=false,
  optional_tl_subcommands
}
```

约束如下：

1. `ProtocolVersionSnapshot.effective` 只能控制协议命令、响应和 DTO 的版本语义；
2. `TransportVersionSnapshot.effective` 只控制 Ethernet Header/Tail、地址族、TL 命令和传输诊断；
3. `packet_alignment` 不是版本字段，必须记录独立来源；
4. GET_VERSION 不可用时，A2L 可以分别为 PL 与 TL 提供证据，不得将其中一方的版本复制给另一方；
5. 断连、重连和切换 Slave 时必须同时清空两个快照和 Alignment 证据；
6. 连接只有在 PL/TL 版本裁决、Alignment 裁决和冲突检查全部完成后才能进入 Ready。

### 4.3 降级必须按特性粒度

**不得**设计成"探测失败 → 整体降级到某版本"。规范（Table 229 等）要求按命令逐个 "use alternative"，且**遵守 Pre-Action**（如 DOWNLOAD_NEXT/DOWNLOAD_MAX 的 ERR_CMD_UNKNOWN 要求**先执行 SET_MTA** 再走替代命令）。

### 4.4 关于版本上限的工程决策（须显式声明为决策）

将 `EffectiveVersion` 上限设为 V1.5，并覆盖 V1.3/V1.4，**属工程决策**：
- **支持依据**：V1.5 规范确实存在（`docs/` 下已有官方英文版）；GET_VERSION 语义明确。
- **不支持依据**：**兼容矩阵数据不存在**（§1.9），V1.3/V1.4/V1.5 无完整显式差异清单。
- **传输层裁决已完成**：已复核官方 Ethernet TL V1.5，详见 §1.9A。此前“规范缺失”的门禁关闭；TL 独立版本快照、Tail/Alignment、A2L 贯通和带宽治理仍是当前实现门禁。独立 ECU/标准对端已按 D8 移至后续验证里程碑，不阻塞当前 XCPlite 阶段，但继续阻塞通用 `Verified` 声明。
- **要求**：文档中必须如实标注 PL V1.3～V1.5 + TL V1.5 为工程支持范围，**不得声称有外部兼容矩阵依据**；GET_VERSION 不可用时，PL 与 TL 分别进入各自的 `1.x-conservative` 状态，不得仅凭 CONNECT Major 猜测 Minor。
- **Profile 边界**：首阶段只承诺 TL V1.5 / IPv4 UDP 单播基础 Profile；IPv6 与四个 Optional TL Subcommand 必须公开标为 Unsupported，直至独立实现和验证。

### 4.5 版本 API/ABI 的一次性破坏性升级

用户已裁决采用破坏性方案 B，不保留兼容层。实施边界如下：

1. 以统一 `XcpMasterOptions` 替换 `XcpMaster(std::unique_ptr<IXcpTransport>, CommandTimeouts, IEventListener*)`；旧构造函数直接删除，不增加 Deprecated 重载；
2. `XcpMasterOptions` 聚合 `CommandTimeouts`、`VersionPolicy`、`RecoveryPolicy` 和非拥有的 `IEventListener*`；监听器生命周期约束继续由 Doxygen 明确，禁止同时存在两套权威配置；
3. 新增独立 `ProtocolVersionSnapshot`、`TransportVersionSnapshot` 和能力快照查询；允许为这些新公共结构增删必要字段，但 PL/TL 不得合并为同一版本字段；
4. 项目内 tools、examples、tests 和 adapter 必须在同一批次迁移，禁止以临时适配器长期保留旧调用方式；
5. 对公开虚接口不做与本任务无关的扩张；取消操作优先由 `XcpMaster`/`CommandExecutor` 的明确 API 提供，避免迫使全部外部 Transport/Listener 派生类无关改造；
6. CMake 项目版本按 SemVer 从 `1.3.0` 提升为 **`2.0.0`**，并将 DESCRIPTION 改为协议支持范围说明（PL V1.3～V1.5 + Ethernet TL V1.5 IPv4 UDP 基础 Profile）；发布说明和迁移文档必须明确 API/ABI break，禁止继续让库版本号冒充协议版本号；
7. API 迁移必须有编译级测试：旧签名不再作为支持目标，新签名覆盖默认配置、显式版本、恢复参数和查询快照。

冻结的公共接口目标形态（名称和核心语义不再留待实施时二次裁决）：

```cpp
struct XcpMasterOptions {
    CommandTimeouts command_timeouts{};
    VersionPolicy version_policy{};
    RecoveryPolicy recovery_policy{};
    std::optional<LinkBudgetConfig> link_budget{};
    IEventListener* event_listener{nullptr};  // 非拥有
};

explicit XcpMaster(std::unique_ptr<IXcpTransport> transport,
                   XcpMasterOptions options);

void CancelPendingOperation();
[[nodiscard]] ProtocolVersionSnapshot GetProtocolVersionSnapshot() const;
[[nodiscard]] TransportVersionSnapshot GetTransportVersionSnapshot() const;
```

错误契约同步冻结：复用现有 `ErrorCategory::OperationOutcomeUnknown`；新增 `ErrorCategory::Cancelled` 表示显式取消/断连唤醒，普通或总体 deadline 到期继续使用 `ErrorCategory::Timeout`，两者不得只靠错误文本区分。

这是经批准的接口替换，不再以“保持旧 ABI”为设计约束；但仍要求迁移完整、错误诊断明确、无新旧行为暗中分叉。

### 4.6 恢复策略的冻结参数与语义

恢复策略不继续扩展现有 `CommandTimeouts`，而是由新的 `RecoveryPolicy` 集中表达。默认值冻结为：

```text
timeout_retry_limit                = 2
busy_retry_delay                   = 100 ms
busy_total_timeout                 = 10 s
seed_key_transaction_restart_limit = 1
```

约束如下：

1. **普通 timeout**：仅恢复矩阵允许的命令执行 `SYNCH → 必要的隐式状态重建 → 最多重试 2 次`；
2. **`EV_CMD_PENDING`**：重启当前等待计时，不重发原命令，但仍受该顶层操作的总体 deadline 和取消控制；
3. **`ERR_CMD_BUSY`**：收到显式 BUSY 后等待 t7；有可信 A2L/调用方 t7 时覆盖 100 ms 默认值，否则使用默认值。10 s deadline 从顶层调用首次发送前开始计算，覆盖全部等待、重发和响应时间，重试后不得重新计时；
4. **Seed&Key**：执行 §2C.2 的冻结分段表。GET_SEED timeout 可在 SYNCH 成功后完整重启；UNLOCK 非末段 timeout 必须先断开重连再完整重启；两类完整事务最多重启 1 次；UNLOCK 末段 timeout 禁止任何自动重启；
5. **结果未知写操作**：DOWNLOAD、UNLOCK 末段等在 timeout 后不得自动重放，统一返回现有 `ErrorCategory::OperationOutcomeUnknown`；该规则优先于事务重启上限，并且是不可关闭的安全不变量，不提供“允许重放”的公共开关；
6. **取消语义**：提供线程安全的 `XcpMaster::CancelPendingOperation()`；取消、Transport Close、Disconnect 和析构必须唤醒等待者、清理 pending 状态，并返回新增 `ErrorCategory::Cancelled`，不得与 `Timeout`/`ProtocolError` 混用；
7. **时间基准**：所有 deadline 使用单调时钟；负数、零值、溢出及 `retry_limit < 0` 在构造或 Connect 前失败；
8. **配置来源**：上述数值是项目工程默认值，不得表述为 XCP V1.5 固定常量；调用方显式值或可信 A2L t7 必须保留来源并参与诊断。

恢复矩阵应数据化为 `Command × Failure → PreAction / Action / RetryLimit / CancelPolicy`，避免在执行器中继续增加互相覆盖的条件分支。

---

## 5. 实施批次划分（原审计 12 项及 TL 专项均已裁决）

**前置状态**：原 F-01～F-12 已全部复核（§2.1-§2.4、§2A-§2D），Ethernet TL V1.5 专项已形成 T-01～T-08（§1.9A、附录 E），无待裁决项。以下批次按**依赖关系 + 风险 + 改动耦合度**排序。

| 批次 | 内容 | 依赖 | 风险 | 说明 |
|---|---|---|---|---|
| **A** | 冻结 PL V1.3～V1.5 + Ethernet TL V1.5 / IPv4 UDP 单播 Profile 与规范证据 | 无 | 低 | **规范裁决已完成**；实施时固化能力表、Optional 边界和可追溯页码，不再使用旧 TL V1.1 基线 |
| **B1** | 破坏性 API 迁移 + GET_VERSION + PL/TL 双版本状态机 + 双 `1.x-conservative` + 冲突/失败处理 | A + D1/D5/D6 | 高 | 先统一迁移到 `XcpMasterOptions`，再稳定两个独立的 Reported/Effective/Source/Confidence 和连接完成语义；不保留旧接口，不得以 timeout 冒充老版本 |
| **B2** | T-01～T-08：Tail/Alignment、A2L 贯通、带宽治理、IPv4/Optional 边界 | B1 | **高** | **TL V1.5 发布门禁**；先完成独立 Alignment 证据，再改 Header Codec 和 UDP Transport，最后接入 DAQ 带宽预算 |
| **C** | F-01 + F-02a～F-02d + F-04 | 无，可与 A/B1 部分并行 | 中 | 地址、对齐和状态位；实施前先产出 `Advance`、MTA、UPLOAD/DOWNLOAD/Block 相关静态调用点清单与测试映射，确认无遗漏后再修改 |
| **D** | F-09 + F-10 | B1 的 `RecoveryPolicy` + C 的 MTA 单位语义 | 中 | **独占 Seed&Key 事务恢复**；严格执行 GET_SEED/UNLOCK 非末段/末段 timeout 分段表，禁止通用层盲重放 |
| **E1** | **F-08 → F-06/N-01/N-02/N-03/N-04 → F-07 → N-05** | D2 + B1 的能力快照 | **高** | DAQ 识别字段、CTR/FILL、双解码实现、Byte Order 与示例同步；顺序不可颠倒 |
| **E2** | F-05 + F-12 + O-2 | C 的 AG 语义，可与 E1 并行 | 中 | MIN_DAQ、Resolution、`OVERLOAD_MSB`、`Address_Extension` 及 Granularity/AG 可表示性等本地前置约束 |
| **F** | F-03a～F-03c + F-11 + 冻结的恢复参数 | B1 + D + D3/D7 | 中 | F-03 拆分注释、Mode、Length/AG；F-11 只负责通用 BUSY/错误矩阵与取消/deadline，**不重复实现 Seed&Key 事务逻辑** |
| **G** | 文档声明、API/ABI 迁移说明、跨平台、三版本、TL Alignment 和 XCPlite 验收 | A～F（含 B2） | 中 | 只收口能力矩阵、迁移/发布文档和验收记录；API行为、调用方迁移和编译测试不得拖到 G |

### 5.1 建议的优先级调整（相较初审的变化）

**变化一：批次 A 的外部规范调查已经完成，实施阶段拆为 B1/B2。** B1 先建立 PL/TL 双版本状态和未知 Minor 策略；B2 再完成 TL V1.5 Tail/Alignment、A2L 贯通与带宽治理。版本探测、传输布局和能力冻结都不能作为最后附加功能。

**变化二：F-12 与 O-2 纳入批次 E2。** C2～C4 对齐等式与 F-01 同属 AG 单位换算；`OVERLOAD_MSB` 与 `Address_Extension` 也直接属于 F-05/F-12 的本地前置校验，避免同一区域多次返工。

**变化三：F-03 仍为中风险。** wire 布局无需重写，但 Mode 必须拆分两个正交位；在未支持压缩/加密时必须显式拒绝，不能只掩码 bit 0 后沿用普通 A2L 上传路径。

**变化四：F-10 与 F-09 合批。** 两者共享 MTA/隐式状态与超时恢复边界；UNLOCK 非末段 timeout 必须先断开重连，再由 Seed&Key 事务层从 GET_SEED 完整重启（最多 1 次），不得交由调用方二选一，也不得由通用执行器盲重放；UNLOCK 末段仍只返回 `OperationOutcomeUnknown`。

**变化五：N-01～N-05 纳入 E1 正式追踪。** 它们不是旁注：CTR/FILL、头长度、注释、a2lbridge 并行布局及示例表达能力均是 DTO 修复闭环的一部分。

**变化六：B1 允许一次性 API/ABI break。** 旧构造接口和旧配置布局不再是兼容目标；必须在同批次完成 `XcpMasterOptions`、版本快照 API 以及所有项目内调用方迁移，禁止遗留双轨。

**变化七：当前 G 批次只以 XCPlite 完成端到端验收。** 独立 ECU/标准对端不再阻塞当前实现批次，但阻塞“通用 XCP V1.5 Verified”声明；后续 ECU 联调另建验证里程碑并回填证据。

### 5.2 每批次实施时须遵守的既有规范（源自项目 `AGENTS.md`）

1. 修改前 read 相关头文件确认签名与字段名（禁止凭记忆编码）；
2. 改动后读回确认，能编译则编译、能跑测试则跑；
3. 在 `tests/` 下新增或更新测试用例；
4. 在 `code-plan/` 下生成本次修改内容与测试结果的实施记录；
5. 所有新增接口/函数/成员/全局变量须加 Doxygen 中文注释；代码段须加中文注释；
6. 不得重构已完成代码、不得擅自新增接口（除非任务明确要求）；
7. `thirdparty/` 下的**真外部依赖**不得修改（`XCPlite`、`a2llib` 等）。
   **例外——已获用户裁决（m01346）**：`thirdparty/a2l-sdk/a2lbridge/` 属**本项目自研桥接层**（22 个文件全部入库，含本项目构建脚本），**可以修改**。本轮 N-04 修复须触及 `a2lbridge/src/daq_layout_impl.cpp` 与 `a2lbridge/include/libxcp/a2l/daq_layout.hpp`。
   **边界**：`thirdparty/a2l-sdk/` 下的 `src/`、`include/liba2l/` 为上游 `liba2l` 本体及封装（`liba2l_export.cpp`、`a2llogstream_boost_locale.cpp`、`dll_main.cpp`），**不在可改范围**，本轮不得触碰。

### 5.3 回归风险提示

| 批次 | 必须保持不变的既有行为 | 判据 |
|---|---|---|
| E1 | XCPlite（`RelativeWordAligned`, DAQ_KEY_BYTE=`0xC0`）**互操作行为不变** | `tests/dto_envelope_decoder_test.cpp` 通过；XCPlite 集成用例（`XcpliteIntegration`）通过 |
| C | `tests/response_parser_test.cpp:211-226` 固化了 F-04 的**错误行为**，须**更新而非保留** | 该用例断言需改为 bit 2 语义 |
| C | F-01 的 `XcpAddress40::Advance` 生产调用点位于 `memory_access.cpp` 的分块读写路径，不在 `command_codec.cpp` | `memory_access` 的 AG×跨块×边界测试须复核，并确认无其它调用点 |

### 5.4 统一追踪矩阵

| 编号 | 批次 | 核心改动 | 主要测试 | 完成判据 |
|---|---|---|---|---|
| V-01 | A | 固化 PL V1.3～V1.5 + TL V1.5 / IPv4 UDP 单播 Profile 及 Optional 边界 | 双 PDF 文档证据复核 | 版本/传输组合有来源、章节和适用范围 |
| V-02 | B1 | GET_VERSION 编码、解析、`0x20/0x34` 处理 | command codec/parser golden | 6 字节响应及所有错误分支确定 |
| V-03 | B1 | Protocol Reported/Effective/Source/Confidence、`1.x-conservative`、连接状态冻结 | session/master 参数化测试 | 未确认 Minor 不冒充精确版本，重连清空状态 |
| V-04 | B1（行为）/G（文档） | B1 移除 A2L `0x0100` 硬编码或禁用该来源，并完成配置/A2L/运行时冲突策略与公开查询；G 只记录结果 | 来源与冲突矩阵、API 测试 | B1 结束时每种来源组合已有唯一结果和诊断；无伪造 A2L Minor |
| V-05 | B1（接口与迁移）/G（迁移说明） | B1 以 `XcpMasterOptions` 替换旧构造接口、迁移全部项目内调用方并新增 PL/TL 快照查询；G 只编写迁移/发布文档 | 编译级 API 测试、tools/examples/tests/adapter 全量迁移 | B1 结束时无旧接口兼容层、无双配置权威且全仓可编译；G 明确 API/ABI break 与库版本 2.0.0 |
| T-01 | B1 | 新增独立 Transport Reported/Effective/Source/Confidence 快照 | GET_VERSION/A2L/CONNECT/config 组合 | PL/TL 不互相推导，重连同时清空 |
| T-02 | B2 | `PACKET_ALIGNMENT` 8/16/32 强类型配置、来源和冲突门禁 | A2L/显式/默认/冲突矩阵 | 每个会话只有唯一 Alignment，冲突在 Open/Connect 阶段失败 |
| T-03 | B2 | UDP 编码按 Alignment 添加任意值 FILL Tail | 长度 1～边界、单帧/多帧、8/16/32 golden | 每个后继 Header 位于正确边界，LEN 不含 Tail |
| T-04 | B2 | UDP 解码按 Alignment 跳过 Tail，保持 Frame 不跨 Datagram 校验 | 合法任意 FILL、截断、伪 Header、混合长度 | 16/32-bit 对齐 Datagram 正确拆帧，畸形输入整体拒绝 |
| T-05 | B2 | A2L `XCP_ON_UDP_IP` 的 TL version/alignment/sub_commands 自动贯通核心 | a2lbridge→transport 集成 golden | 不需调用方重复填写；来源和冲突可查询 |
| T-06 | B2（行为）/G（文档） | B2 冻结 IPv4 UDP 单播 Profile，并使 IPv6/四个 Optional TL Subcommand 的能力查询与请求路径显式 Unsupported；G 只同步发布声明 | 能力查询、请求 Optional 命令负例 | B2 结束时不发送未支持命令、不误报能力；G 声明无能力膨胀 |
| T-07 | B2 | 按 §1.9A.3 的冻结 `LinkBudgetConfig` 与净 XCP 字节公式在 DAQ 启动前预算；本阶段 A2L 自动提取明确 Unsupported | DTO×Event周期×Header/Packet/Tail、8/16/32 Alignment、实际链路较低、unknown policy、overflow、override | 超预算默认拒绝；未提供配置固定 AllowUnverified，已提供配置但链路未知时按其策略 Unverified/拒绝；不计算 UDP/IP 外层开销，不擅改上游 |
| T-08 | G / 后续 ECU 里程碑 | 当前以规范 golden + XCPlite 完成 Header/Tail/CTR/Alignment 回归；后续接入独立 ECU/标准对端 | 当前 XCPlite 矩阵；后续补丢包/乱序/多 Alignment 实际对端证据 | 当前只标 `XCPliteVerified`/`Unverified`；后续独立对端通过后才能升级通用 `Verified` |
| F-01 | C | 地址按元素推进 | AG=1/2/4 × 跨块 × 溢出 | SET_MTA 地址序列正确 |
| F-02a | C | DOWNLOAD 标准模式 AG 对齐、FILL 与表格式容量 | AG=1/2/4 × ByteOrder golden、边界长度、任意接收侧 FILL | 编码长度不超 MAX_CTO，数据首偏移为 AG，Master 发送填0且解析端不校验FILL值 |
| F-02b | C | UPLOAD 响应侧 AG 对齐与数据起点 | AG=1/2/4 × 返回长度 × 畸形短帧 | 不把对齐字节当数据，元素数与字节数换算一致 |
| F-02c | C | DOWNLOAD_NEXT Optional 边界 | 能力查询/公开API审查、未支持路径负例 | 当前无公开执行路径且显式 Unsupported；未来启用前必须实现 AG 对齐、容量及 UNKNOWN→SET_MTA fallback |
| F-02d | C | DOWNLOAD_MAX Optional 边界 | 能力查询/公开API审查、未支持路径负例 | 当前无公开执行路径且显式 Unsupported；未来启用前必须实现 1/3 字节前缀、块模式公式及 UNKNOWN→SET_MTA fallback |
| F-04 | C | STATUS bit2 + CAL_PAG_CFG_LOST bit1 | 每个位独立翻转；PDF第116页版面证据记录 | 两标志互不串扰，且无待裁决状态 |
| F-09 | D | SHORT_UPLOAD 后同步本地 MTA | SHORT_UPLOAD→UPLOAD→timeout 恢复 | 恢复不使用旧 MTA |
| F-10 | D | Seed&Key 分段恢复状态机 | GET_SEED各段timeout、UNLOCK非末段timeout、UNLOCK末段timeout、SYNCH失败、重连、重启上限 | GET_SEED可SYNCH后重启；UNLOCK非末段须重连后重启；末段只返回 `OperationOutcomeUnknown`，无自动重放 |
| F-08 | E1 | DAQ_KEY_BYTE 自动映射为布局 | 四种 Identification 类型 | 不依赖调用方手工填重复协商数据 |
| F-06/N-01 | E1 | 标准 Identification + CTR/FILL 合并语义 | Figure 51/52/53/56 golden | 位置、长度和首 DTO CTR 正确 |
| N-02/N-03 | E1 | SetEnvelopeMode 下界及注释 | 2/3/4 字节边界负例 | API 约束与布局一致 |
| N-04 | E1 | a2lbridge 并行布局同步 | A2L golden 与核心交叉向量 | 两路径对同一 DTO 得到一致边界 |
| F-07 | E1 | DTO 多字节按协商 Byte Order | Intel/Motorola timestamp/DAQ | raw 值一致 |
| N-05 | E1 | 示例支持 RelativeWordAligned | CLI 解析/示例 smoke | 可从示例显式选择四种类型 |
| F-05 | E2 | MIN_DAQ 编号区间 | 非零/边界 MIN_DAQ | 全部 DAQ 命令使用正确 List Number |
| F-12/O-2 | E2 | Granularity/AG可表示性、地址/长度对齐、Max/Mode/OVERLOAD/Address Extension 前置校验 | Granularity `{1,2,4,8}` × AG `{1,2,4}` 全组合、ratio=0/不可整除、上限/模式正负例 | 先验证 `GRANULARITY>=AG && GRANULARITY%AG==0`，非法组合在发命令前失败且无除零/模零 |
| F-03a | F | 修正 GET_ID 布局与“XCPlite方言”错误 Doxygen | 文档/头文件审查 | 注释与 Table 60 一致，不再传播错误规范结论 |
| F-03b | F | GET_ID Mode 拆位和压缩/加密拒绝 | Mode `00/01/02/03` | 未支持数据不进入普通 A2L 路径 |
| F-03c | F | GET_ID `Length mod AG == 0` 与长度边界校验 | AG=1/2/4 × 合法/非法Length、短帧 | 非法 Length 返回 `MalformedPacket`，不设置错误MTA或进入A2L解析 |
| F-11 | F | 按 §4.6 实现通用可取消 BUSY 重试及结构化恢复策略 | 默认/override t7、10s总deadline、timeout重试2次、deadline不重置、cancel/disconnect/析构、非法参数、单调时钟边界 | 无永久阻塞、取消返回 `Cancelled`、无 deadline 重置；Seed&Key只复用D批次结果，不在F重复实现 |
| DOC-01 | G | README、模块文档、能力矩阵、迁移说明 | 文档审查 | 声明不超过测试证据 |

### 5.5 测试与验证矩阵

#### 单元与组件测试

- 编译级 API 迁移测试：仅允许新的 `XcpMasterOptions` 构造方式，覆盖默认/显式版本策略、恢复策略和 PL/TL 快照查询；tools、examples、tests、adapter 全部编译通过，旧构造签名不再作为支持目标；
- `tests/command_codec_test.cpp`：GET_VERSION 请求；DOWNLOAD 在 AG=1/2/4 下的对齐、FILL与容量边界；DOWNLOAD_NEXT/DOWNLOAD_MAX 当前验证为显式 Unsupported、无公开执行路径（未来实现 Block Mode 时再启用相应 wire golden）；
- `tests/response_parser_test.cpp`：GET_VERSION、GET_STATUS bit1/bit2、GET_ID Mode `0x00..0x03`、`Length mod AG` 合法/非法、畸形/短帧；UPLOAD 在 AG=1/2/4 下的数据起点；
- `tests/session_test.cpp`：版本状态冻结、`Xcp13CoreBaseline` Allow/Block/Capability-gated 表、Minor 未确认、断连清空、重连换 Slave；
- `tests/memory_access_test.cpp`：AG=1/2/4 的读写分块、地址溢出、SHORT_UPLOAD 后 MTA；并验证未实现的 DOWNLOAD_NEXT/MAX 不会被能力层误报或从现有写路径发出；
- `tests/seed_key_test.cpp`：GET_SEED 每段 timeout 后 SYNCH+完整重启、UNLOCK 非末段 timeout 后重连+完整重启、UNLOCK 末段 timeout 返回 `OperationOutcomeUnknown` 且不重放、重启上限；
- `tests/recovery_test.cpp`：BUSY 默认与显式 t7、10s总deadline不因重试/EV_CMD_PENDING重置、取消/断连/析构、`Cancelled`、非法零/负/溢出参数、单调时钟边界，替换“BUSY 永不重试”的旧断言；
- `tests/dto_envelope_decoder_test.cpp`：四种 Identification、FILL 任意值、CTR 替代 FILL、Intel/Motorola；
- `tests/measurement_session_test.cpp`：2/3/4 字节下界、DAQ_KEY_BYTE 自动贯通及冲突配置；
- `tests/a2l_golden_test.cpp`：a2lbridge 与核心共享的标准 DTO golden；
- `tests/xcplite_daq_test.cpp`、`tests/xcplite_measurement_test.cpp`：XCPlite `0xC0`/RelativeWordAligned 回归；
- `tests/udp_header_codec_test.cpp`：TL V1.5 Alignment 8/16/32、单帧/多帧 Tail、任意 FILL、LEN 不含 Tail、截断与伪 Header 负例；
- `tests/udp_transport_test.cpp`、`tests/xcp_udp_loopback_test.cpp`：PL/TL 双版本状态、A2L/显式 Alignment 冲突、CTR 回绕/缺口/乱序和多 Frame Datagram；
- `tests/a2l_golden_test.cpp`：`XCP_ON_UDP_IP` 的 TL version、`PACKET_ALIGNMENT`、`OPTIONAL_TL_SUBCMD` 提取与核心贯通；本阶段对 `MAX_BUS_LOAD`/`MAX_BIT_RATE` 的 A2L 自动提取固定验证为 Unsupported，不保留实施时分支；
- DAQ 带宽预算测试：按 §1.9A.3 的净 XCP 字节公式覆盖 DTO 大小 × Event 周期 × Header/Packet/Tail、Alignment 8/16/32、Datagram 聚合不改变净负载、实际链路低于 nominal、周期未知策略、乘加溢出、超预算拒绝和显式 override。

#### 版本和集成组合

| 维度 | 必测值 |
|---|---|
| Protocol | 确认 V1.3、确认 V1.4、确认 V1.5、Minor >5 封顶、Minor unknown |
| GET_VERSION | 成功、`ERR_CMD_UNKNOWN`、`ERR_SUBCMD_UNKNOWN`、timeout、畸形响应 |
| Version evidence | 仅运行时、运行时+A2L一致、冲突、仅显式配置、无 Minor 证据 |
| Transport version | 确认 TL V1.0/V1.1/V1.4/V1.5、Minor >5 封顶、Minor unknown；PL/TL 版本交叉组合不得互相推导 |
| Ethernet Profile | TL V1.5 / IPv4 UDP 单播；IPv6 与 TL Optional Subcommand 标 Unsupported，直至单独实施 |
| Packet Alignment | 8/16/32 bit；单帧、多帧、任意 FILL、A2L/显式/默认/冲突来源 |
| Bandwidth | 预算内、超 MAX_BUS_LOAD、实际链路未知、显式 override |
| Byte Order | Intel、Motorola |
| AG | 1、2、4 |
| 对端 | 当前阶段：XCPlite 跨进程集成；后续里程碑：独立 ECU/标准对端（当前未覆盖组合保持 Unverified） |
| 平台 | Windows、Linux、macOS Release |

测试不能要求所有正交维度做全笛卡尔积；单元测试覆盖全部正交边界，端到端选择风险驱动的代表组合。任何未覆盖组合必须标为 `Unverified`，不得从相邻组合外推 Supported。

### 5.6 Definition of Done

- [ ] A 批次固化 PL/TL 两份官方 PDF 的来源、章节、适用范围和 IPv4 UDP 单播 Profile；旧 TL V1.1 临时基线不再出现在发布声明中；
- [ ] V-01～V-05、T-01～T-08、F-01、F-02a～F-02d、F-03a～F-03c、F-04～F-12、N-01～N-05、O-2、DOC-01 在追踪矩阵中均有实施记录和测试结果；
- [ ] 旧 `XcpMaster` 构造接口已由 `XcpMasterOptions` 完整替换，所有项目内调用方在 B1 同批迁移，无 Deprecated 兼容层或新旧配置双轨；库版本升级到 2.0.0，G 只补迁移/发布文档；
- [ ] GET_VERSION 不可用时不会伪造 V1.3/V1.4/V1.5 的 PL 或 TL Minor；`Xcp13CoreBaseline` 的 Allow/Block/Capability-gated 每项均有测试，timeout 不会触发静默降级；
- [ ] PL 与 TL 使用独立 Reported/Effective/Source/Confidence，重连会同时清空，任一层不得从另一层推导；
- [ ] TL V1.5 Alignment 8/16/32 的 Tail 编解码、A2L 自动贯通和冲突门禁全部通过；
- [ ] IPv6 和四个 Optional TL Subcommand 在未实现时对外明确为 Unsupported，不会被能力查询误报；
- [ ] DAQ 启动前可使用冻结的显式 `LinkBudgetConfig` 和 §1.9A.3 净 XCP 字节公式完成带宽预算；超预算默认失败，未知周期/链路按策略处理，所有溢出与 override 有测试；本阶段 A2L 自动提取明确为 Unsupported，且未擅改上游；
- [ ] V1.3、V1.4、V1.5 的已承诺 Protocol 核心能力，以及 TL V1.5 IPv4 UDP 单播 Profile，分别通过版本化测试；
- [ ] GET_ID 压缩/加密标志在未支持时明确失败，不进入普通 A2L 解析；
- [ ] 恢复策略默认值与 §4.6 一致：timeout 重试2次、BUSY 100 ms间隔/10 s总deadline；GET_SEED与UNLOCK非末段按冻结分段表最多重启1次，UNLOCK末段只返回 `OperationOutcomeUnknown`；取消/断连/析构返回 `Cancelled` 并终止等待；显式override、非法值和deadline不重置均有测试；
- [ ] 核心与 a2lbridge 的 DTO 布局对同一 golden 得到一致结果；
- [ ] XCPlite DAQ/Measurement 作为当前阶段唯一端到端对端验收保持通过，且不再依赖错误枚举名或固定 FILL 值；所有尚无独立 ECU/标准对端证据的组合明确标记 `Unverified`；
- [ ] 按项目规则执行 Release clean build 后运行目标测试；Windows/Linux/macOS 结果均有记录；
- [ ] README、模块文档、API Doxygen 的版本与能力声明不超过实测范围；
- [ ] 每批提交在其下游尚未合入时可原子回滚；回滚上游批次时连同全部依赖批次回滚，不留下半更新的版本快照、公开 API 或 DTO 布局定义。

---

## 6. 证据边界声明

为避免后续误用，以下事项**在原始 PDF 规范文本中确实无法确定**，不得臆造依据。

**⚠ 基准说明（用户裁决 m01451）**：本节所有"不存在 / 未找到 / 未规定"的判定，**其有效性以对原始 PDF 的复核为条件**，不得援引 Markdown 转换版的行号（该版本存在图片错位与表格版面丢失，会把"转换丢失"误判为"规范没有"）。下表"依据"列中的 `:NNNN` 为旧 Markdown 行号，**仅作历史痕迹**；判定本身已由附录 C／§2A-§2D 的 PDF 复核确证。

| 事项 | 状态 | 依据 |
|---|---|---|
| 兼容矩阵的具体组合数据 | **文本中不存在**；原文为 "documented by the **linked table**"（**外部链接**，非转换丢失） | §1.9／附录 C（PDF 第 12 页复核） |
| V1.4 ↔ V1.5 的显式差异清单 | **规范无 revision history 章节**（PDF 全文 `revision` 命中 0 次） | §1.9／附录 C |
| 对齐填充字节必须为 0 | **规范未规定**；属实现自由度 | §1.7 |
| FILL 字节的具体填充值（如 XCPlite 用 `0xAA`） | **规范未规定**，仅称 "optional FILL byte"；**不得把任一填充值当作强制标志校验** | §2B.2A |
| `new_MTA = old_MTA + n` 的显式公式 | **未找到明文**；结论为**推论**（不可声称规范明文要求） | §2.1 |
| 基于 ERR_CMD_UNKNOWN 的整体版本降级机制 | **不存在**；降级为逐特性 | §1.11 |
| V1.5 对应的传输层版本 prerequisite | PL V1.5 未指定具体 TL Minor；官方 Ethernet TL V1.5 已复核，也未规定“PL/TL 版本号必须相等”。因此 PL V1.3～V1.5 + TL V1.5 是本项目工程支持范围，仍须逐组合验证，不得冒充外部兼容矩阵结论 | §1.9／§1.9A／附录 E |
| DOWNLOAD 最大元素数矛盾（表格式 `(MAX_CTO-2)/AG` vs 正文式 `MAX_CTO/AG-2`） | **规范自身矛盾，无澄清**；**表格式才正确** | §1.7／§2.2（PDF 第 142 页） |
| "最低支持版本"声明 | **未找到** | §1.1 |

---

## 附录 A：本文采用的核实方法

1. **规范原文核实（基准已切换为原始 PDF）**：本文档**初稿**的 `:NNNN` 行号引用来自 Markdown 转换版（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`，9614 行）的实际读取与检索，非凭记忆。**但该版本经用户裁决（m01451）已停止使用**——因图片与正文错位、表格版面丢失，"某内容不存在"类结论存在误判风险。
   **现基准为官方原始 PDF**（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`，312 页）：全部 12 项裁决均以 pypdf 逐页提取文本后逐条复核（方法见附录 C、附录 D）。**凡有疑问，一律查原始 PDF。** 文档中残留的 `:NNNN` 行号**仅作历史痕迹，不再作为证据**。
1b. **对端实现核实（§2B.2A）**：涉及"XCPlite 是否使用私有协议"的结论，**直接读取对端源码**（`thirdparty/XCPlite/src/xcp.h:380-388` 的 `DAQ_KEY_BYTE` 常量、`thirdparty/XCPlite/src/xcplite.c:1081, 1616-1621, 2486-2498` 的编码实现与注释）核实，**不依赖对实现的推测或审计报告的二手定性**。
2. **实现代码核实**：F-01／F-02／F-04 的偏差位置已逐处读取源码确认（`src/protocol_types.cpp:200-209`、`src/command_codec.cpp:367-412`、`src/response_parser.cpp:53-58, 224`），并经全仓检索确认消费点数量。
3. **图片核实**：兼容矩阵所引 `image_000008_...png` 已实际读图确认内容。
4. **未实施任何代码改动**：本文档编制过程为纯只读操作。

## 附录 B：审计报告 F-01～F-12 索引与裁决状态

| 项 | 严重度 | 标题 | 裁决状态 | 严重度重估 |
|---|---|---|---|---|
| F-01 | 高 | AG>1 时分块地址推进单位错误 | **成立**（§2.1） | 维持高 |
| F-02 | 高 | AG=DWORD 的 DOWNLOAD 缺少对齐字节 | **成立**（§2.2） | 维持高 |
| F-03 | 高 | GET_ID 固化 XCPlite 方言 | **指控不成立**；新增 3 处真实缺陷（§2A） | **高 → 中** |
| F-04 | 中 | GET_STATUS 的 STORE_DAQ_REQ 位解析错误 | **成立**（§2.3） | 维持中 |
| F-05 | 高 | 动态 DAQ 忽略 MIN_DAQ | **成立**（§2B.6） | 维持高 |
| F-06 | 高 | DTO Relative Identification Field 不符合标准布局 | **成立**（§2B.2） | 维持高 |
| F-07 | 高 | DTO 多字节字段固定按小端解码 | **成立**（§2B.3） | 维持高 |
| F-08 | 高 | DAQ_KEY_BYTE 未贯穿到 DTO 配置 | **成立**，措辞须修正（§2B.4） | 维持高 |
| F-09 | 中 | SHORT_UPLOAD 后本地隐式 MTA 未同步 | **成立**（§2.4） | 维持中 |
| F-10 | 中 | 分段 Seed & Key 超时恢复可能失步 | **成立**，依据须精确措辞（§2C.2） | 维持中 |
| F-11 | 中 | 错误恢复矩阵只实现部分策略 | **成立**，属"实现范围"非"违规"（§2C.3） | 维持中 |
| F-12 | 中 | 能力与模式约束未完整贯穿 | **成立**，属"前置校验缺失"（§2D.5） | 维持中 |

**裁定汇总：11 项成立，1 项（F-03）指控不成立并降级。全部 12 项复核已完成，无待裁决项。**

**严重度分布（重估后）**：高 5 项（F-01、F-02、F-05、F-06、F-07）+ 1 项 F-08 = 6 项高；中 6 项（F-03、F-04、F-09～F-12）。

### 附录 B.1：审计报告未记载、本轮 review 新增识别的缺陷

| 编号 | 严重度 | 标题 | 位置 | 说明 |
|---|---|---|---|---|
| N-01 | **高** | `CTR` 与 `FILL` 合并语义缺失 | `src/daq/dto_envelope_decoder.cpp:135-140` | 规范 PDF 第 97-98 页 Figure 56：`WORD, aligned` 模式首 DTO 的 CTR **取代** FILL 字节；实现在识别字段后独立读 CTR，**位置错误**。XCPlite 恰用该模式（§2B.2A） |
| N-02 | **中** | `SetEnvelopeMode` 下界校验与规范不符 | `src/measurement/measurement_session.cpp:98-112` | `RelativeByte` 硬编码 4U（规范 2U）；`RelativeWord` 与 `RelativeWordAligned` 共用 2U（规范 3U／4U）（§3A.4） |
| N-03 | **中** | 头注释与实现自相矛盾且不符规范 | `include/libxcp/daq/dto_envelope_decoder.hpp:36-40` | 注释称 `RelativeByte` 的 byte1 是"DAQ 扩展字节"，实现却校验 `0xAA`；`RelativeWord` 字段顺序与 Figure 52 相反；未提 `RelativeWordAligned` 的 FILL（§3A.4） |
| N-04 | **中** | a2lbridge 存在**并行的同名解码实现**且含同一布局缺陷 | `thirdparty/a2l-sdk/a2lbridge/src/daq_layout_impl.cpp:48-60` | 与核心路径布局假设镜像。**处置已定（用户 m01346）：`a2lbridge` 属本项目自研代码，可修改**，须一并修正；但**不触碰同目录 `liba2l` 本体**（§3A.1） |
| N-05 | **中** | 示例无法表达规范的 `RelativeWordAligned` | `examples/xcp_integration_demo/xcp_integration_demo.cpp:304-310` | CLI 的 `--id-field` 仅接受 absolute／relative-byte／relative-word 三值（§3A.3） |

**汇总**：新增 5 项（1 高 4 中），均**不在原审计报告 F-01～F-12 范围内**，其中 N-01 与 N-04 直接关联 XCPlite 互操作。

---

## 附录 C：原始 PDF 交叉复核记录

**复核动机**：本文档初稿基于 Markdown 转换版（`Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`，673 行）。该版本存在**图片与正文对应错位**（§1.9 中 `:387` 引用的"矩阵图"实为 ASAM logo），因此"规范没有 X"类结论存在**因转换丢失而误判**的风险。为消除该风险，改用官方原始 PDF 复核全部关键结论。

**复核方法与可复现步骤**：

1. **源文件**：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`
2. **注意**：`file` 命令报告该 PDF 为 "7 pages"，**属误报**（该 PDF 使用增量更新结构的可视化签名页），以实际内容为准。
3. **提取**：使用 Python `pypdf` 逐页提取至文本文件，并插入 `===== [PDF PAGE n] =====` 页标记（共 **312 页**，导出 **460,698 字符**）。
4. **检索**：对上一步文本做关键词定位，逐条比对本文档结论。

**复核结论汇总**：

| 复核项 | Markdown 版结论 | 原始 PDF 结论 | 判定 |
|---|---|---|---|
| 兼容矩阵数据 | 文中不可得 | **PDF 第 12 页同样无表格**；原文为 "documented by the **linked table**"（外部链接） | **一致，且原因查明**（非转换丢失） |
| revision history | 无该章节 | `revision`/`Revision` 全文**命中 0 次** | **确证** |
| Table 43 地址计数 | 地址 +1 跨 2 字节（WORD） | PDF 第 113 页排版：`Address n` → `Byte 00 Byte 01`；`Address n+1` → `Byte 02 Byte 03` | **确证（铁证）** |
| Table 42 粒度编码 | BYTE=1/WORD=2/DWORD=4 | PDF 第 112 页：`00`→BYTE1、`01`→WORD2、`10`→DWORD4、`11`→reserved | **确证** |
| AG/ELEMENTS 计数基准 | 地址侧按 AG 计 | `:4462` 逐字一致，且含 `MAX_CTO mod AG = 0` / `MAX_DTO mod AG = 0` | **确证** |
| Table 49 DAQ 状态位 | `STORE_DAQ_REQ` = bit 2 | PDF `:4534-4559` 逐位表：`STORE_DAQ_REQ` = **bit 2**、`CAL_PAG_CFG_LOST` = **bit 1** | **确证** |
| DOWNLOAD 元素数 | 表格 `(MAX_CTO-2)/AG` vs 正文 `MAX_CTO/AG-2` 矛盾 | PDF 第 142 页原文**逐字再现该矛盾**，且**块模式公式为 `min(MAX_BS*(MAX_CTO-2)/AG,255)`** | **确证，并新增块模式差异** |

**复核新增的三项发现**：

1. **`STORE_DAQ_REQ` 命名陷阱**（§2.3）：规范中该词兼指 GET_STATUS 的**位**（`:847` "the STORE_DAQ_REQ **bit** obtained by GET_STATUS"）与 SET_REQUEST 的**模式参数** `STORE_DAQ_REQ_RESUME` / `STORE_DAQ_REQ_NO_RESUME`（`:837-896`，PDF 第 23-24 页流程图）。**实施时必须区分。**
2. **兼容组合的实际组织方式**（§1.9）：PDF 第 270-271 页 §8.2.1 表明组合按传输层以 `XCP_on_##_vU_V.aml` 分散声明，**不存在全局矩阵表**。
3. **DOWNLOAD 块模式上限公式不同**（§2.2）：标准模式 `(MAX_CTO-2)/AG`，块模式 `min(MAX_BS*(MAX_CTO-2)/AG, 255)`。

**版面核验说明**：PDF 文本提取会丢失表格结构，因此关键表格应回到原始版面核对。Table 49 已在本次计划收口时直接渲染 PDF 第 116 页并目视确认 Bit 7→0 与字段列对应关系；F-04 不再保留待确认状态。其它尚未做版面核验、且会成为代码实现唯一依据的表格，实施时仍须打开对应 PDF 页确认。

**复核完成度**：本附录记录的是**首轮** PDF 交叉复核（当时覆盖 F-01／F-02／F-04／F-09）。**其后的 F-03、F-05～F-08、F-10～F-12 亦已逐项以原始 PDF 复核完毕**，结论见 §2A／§2B／§2C／§2D。**全部 12 项现均具备规范原文依据。**

**回归说明**：F-06 的结论在后续追加核实中发生**实质更正**——"XCPlite 私有协议"的定性被推翻，详见 §2B.2A（该更正依据的是**对端源码**而非规范文本，故独立于本附录的 PDF 复核方法）。

---

## 附录 D：Protocol Layer 复核的方法与可复现路径

**来源限定（用户裁决 m01451，须先读）**：

> **本附录所述 Protocol Layer 裁决的唯一权威来源是原始 PDF**（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`，312 页）；Ethernet TL 裁决见附录 E。
> **禁止参考 Markdown 转换版**（`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`）；**有疑问直接查 PDF**。

**原因**：Markdown 版系第三方转换产物，已知存在**图片与正文错位**（§1.9 引用的"矩阵图"实为 ASAM 徽标）与**表格版面丢失**。后者尤为危险——它会把"转换丢失"伪装成"规范没有该内容"，从而使"未规定 / 不存在"类结论**反向出错**。凡涉及"规范是否规定了 X"的判定，**必须回 PDF 确认**。

**可复现提取步骤**：
1. 源文件：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`
2. **注意**：`file` 命令报该 PDF "7 pages" **属误报**（增量更新结构的可视化签名页），实际 **312 页**。
3. 提取：用 Python `pypdf` 逐页提取文本，插入 `===== [PDF PAGE n] =====` 页标记（导出 460,698 字符；写入 `/tmp/xcp15_pdf.txt`）。
   - 若环境缺 `pypdf`，可 `pip install --target .pdftools pypdf` 装在工作区内，再以 `PYTHONPATH=$PWD/.pdftools` 引用（避免写入 `~/.local`）。
4. 检索：对提取文本做关键词定位，逐条比对本文档结论。
5. **局限**：PDF 文本提取会丢失表格版面结构，对表格的解读依据**行序与表头位置**推定；若某结论成为代码实现的**唯一依据**，实施时应打开 PDF 对应页**目视确认**。

**复核顺序**（每批解决一个具体裁决问题）：

| 批次 | 项目 | 主要规范出处 | 结果 |
|---|---|---|---|
| 1 | F-03 | Table 58-61（PDF 第 123-124 页） | 指控不成立，新增 3 处真实缺陷 |
| 2 | F-06/F-07/F-08 | Figure 51-53（PDF 第 95-96 页）、Table 140-143（第 181-183 页） | 三项成立 |
| 2 | F-05 | Table 136（第 178 页）、正文（第 180 页） | 成立 |
| 3 | F-10/F-11 | §7.6.1.2-§7.6.2（第 239-241 页）、恢复矩阵（第 246 页起） | 成立 |
| 4 | F-12 | Table 145-148（第 184-187 页） | 成立 |

**关键方法论**：对每一项执行**三向核对**——⑴ 规范原文（原始 PDF）；⑵ 实现代码（读源码确认行号与行为）；⑶ 审计描述。**三者不一致时以规范原文为准，并查明不一致的成因。**

**本轮因该方法发现的问题**：
- F-03 审计误判，根源是本仓库头文件注释错误（§2A.3）
- F-06 审计称"文档与实现矛盾"，实测头文件与实现自洽，真实偏离是"实现布局与规范 Figure 51/52/53 不符，且把 FILL 当强制标志"（§2B.2、§2B.2A）
- F-10 审计称"实现错误"，实测分段编排逻辑正确，问题在异常路径与规范空白（§2C.2）

**未实施任何代码改动**：本轮为纯只读复核 + 文档编写。`git diff` 确认未触碰任何源码。

---

## 附录 E：Ethernet Transport Layer V1.5 专项复核记录

### E.1 权威来源

- `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`
- ASAM Associated Standard，XCP Ethernet Transport Layer，Version 1.5.0，2017-11-30。
- 本附录只裁决 Ethernet TL；Protocol 命令和响应仍以 PL PDF 为准。

### E.2 规范核实结果

| 主题 | TL V1.5 结论 | 当前实现裁定 |
|---|---|---|
| Addressing | 支持基于 IP 的 Ethernet Transport；规范讨论 IPv4/IPv6 | 当前固定 `AF_INET`；本轮 Profile 限 IPv4 |
| Header | LEN、CTR 均为 Intel WORD | 符合 |
| Frame packing | 一个 UDP Datagram 可含多个完整 XCP Frame，Frame 不跨 Datagram | 无 Tail 的紧邻打包已支持 |
| Tail | 可使用 FILL 把后续 Frame 对齐到 8/16/32 bit，LEN 不包含 Tail | 未实现；T-02～T-04 |
| TL Commands | GET_SLAVE_ID、GET_SLAVE_ID_EXTENDED、SET_SLAVE_IP_ADDRESS、GET_DAQ_CLOCK_MULTICAST 均为 Optional | 只有命令枚举；基础 Profile 明确 Unsupported |
| Error handling | 定义通信错误、SYNCH/重连等传输恢复语义；个别 Optional 命令含重复策略 | 与 PL 恢复矩阵合并实现，不把 Optional 命令规则泛化到全部命令 |
| Bandwidth | `MAX_BUS_LOAD`、`MAX_BIT_RATE` 与 DAQ 带宽管理 | 未贯通；T-07 |
| A2L interface | `XCP_ON_UDP_IP` 提供 TL 版本、地址、端口、Alignment 等静态证据 | bridge 已有部分字段，核心未消费；T-05 |

### E.3 实现证据

- `include/libxcp/udp_header_codec.hpp:27-38`：4 字节 Header 和 65503 字节单 Frame 上限正确；
- `include/libxcp/udp_header_codec.hpp:6-10`：仍按旧基线声称“无 Tail”，须更新；
- `src/udp_header_codec.cpp:41-89`：解码按 `Header + LEN` 紧邻推进，无法跳过 16/32-bit Tail；
- `src/udp_transport.cpp:118-127`：固定 IPv4；
- `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/if_data_xcp.hpp:28-42`：已有 TL version、地址、端口、packet_alignment、sub_commands 字段；
- `thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:483-484`：已有 Alignment 8/16/32 映射，但核心未贯通；
- `thirdparty/XCPlite/examples/freertos_demo/freertos_emu_demo/CANape/freertos_demo.a2l:153`：XCPlite 示例声明 TL `0x0104`，未声明非默认 Alignment。

### E.4 最终支持声明模板

在 T-01～T-08 完成前：

> Implemented/Verified：Ethernet Header LEN/CTR、IPv4 UDP 单播、8-bit Alignment 基础路径；XCPlite TL V1.4 特定组合。  
> Unverified：TL V1.5 端到端互操作、16/32-bit Alignment、带宽治理。  
> Unsupported：IPv6、GET_SLAVE_ID、GET_SLAVE_ID_EXTENDED、SET_SLAVE_IP_ADDRESS、GET_DAQ_CLOCK_MULTICAST。

在 T-01～T-08 与当前 XCPlite 验收完成、但尚未进行独立 ECU 联调时，只可升级为：

> Implemented：Protocol Layer V1.3～V1.5 + Ethernet TL V1.5 / IPv4 UDP 单播基础 Profile 的计划内能力。  
> XCPliteVerified：XCPlite 实际覆盖的版本、Byte Order、AG、Alignment、DAQ 与恢复组合。  
> Unverified：未由 XCPlite 覆盖或尚无独立 ECU/标准对端证据的组合。  
> Unsupported：IPv6 及尚未实现的 Optional TL Subcommand。

后续独立 ECU/标准对端按能力矩阵完成联调并保存日志/抓包证据后，才可将对应组合逐项升级为通用 `Verified`；不得仅凭一次连接成功宣称“完整支持 XCP V1.5”。

**本次更新边界**：只更新计划文档，未修改生产代码、测试代码或 thirdparty。

