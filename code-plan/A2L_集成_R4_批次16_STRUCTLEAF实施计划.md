# A2L 集成 R4 批次16修改计划 —— STRUCTURE/INSTANCE Leaf 展开与成员读写

> 状态：**已实施完成（由批次18 接管闭合，见变更记录 B16-P3 与 `XCPlite_Slave_协议调试_实施记录_批次18_结构叶子展开.md`）**
>
> 来源：批次15 D5 决策、设计文档 §4.1/§4.4/§6.3，以及批次13留下的 B-12 首版边界。
>
> 本批只解决 `TYPEDEF_STRUCTURE` / `INSTANCE` 的成员级识别、寻址、换算和读写；不改变 XCP DAQ、动态 DAQ、传输层或 `thirdparty/a2llib`。

## 1. 已确认的范围与边界

### 1.1 本批目标

支持以下形式的叶子路径：

```text
MODULE::INSTANCE.member
MODULE::INSTANCE.array[index]
MODULE::INSTANCE.nested.member
MODULE::INSTANCE.nested_array[index].member
```

实现闭环：

```text
A2L TYPEDEF_MEASUREMENT / TYPEDEF_STRUCTURE / INSTANCE
  → SDK DTO 快照
  → 桥接层类型解析与递归展开
  → 叶子 SymbolInfo / 路径索引
  → 地址、字节数、数组边界
  → Read/Write、ToPhysical/FromPhysical
```

### 1.2 明确不做

| 项 | 处理 |
|---|---|
| 修改 `thirdparty/a2llib` | 不做；只使用现有上游 API |
| 把整个 STRUCTURE 当字节数组读写 | 永久禁止，继续返回结构化拒绝 |
| 递归环、缺失 TYPEDEF、宽度无法证明 | 明确拒绝，不猜测布局 |
| 不可执行 RECORD_LAYOUT / 不支持 COMPU_METHOD | 明确拒绝，不静默返回 raw/0 |
| 结构体本体符号的普通数值换算 | 不做；只有叶子路径具备执行能力 |
| DAQ 自动把结构体成员加入 ODT | 不自动改变；DAQ 仍需调用方选择已经解析出的叶子符号 |
| `thirdparty/a2llib` 上游修复或 PR | 不做，除非另行批准 |

## 2. 已确认的设计决策

| 决策 | 结论 |
|---|---|
| 业务范围 | 完整闭环：标量、固定数组、嵌套结构、读写和换算 |
| SDK/桥接职责 | SDK 只导出上游事实；桥接层新增内部 `StructureLayoutResolver` 负责递归解析 |
| ABI | 新增 TYPEDEF_MEASUREMENT DTO 后 ABI v5→v6；Release/Debug 双侧重建 |
| 路径格式 | `MODULE::INSTANCE.member[索引].nested`；索引使用零基 |
| 地址公式 | INSTANCE 基址 + 成员 `ADDRESS_OFFSET` + 数组索引×元素步长；不把 AG 乘入 ECU 基址 |
| 可执行条件 | 标量宽度、成员类型可解析、数组维度可证明、RECORD_LAYOUT/COMPU 可执行 |
| 不可证路径 | `UnsupportedOperation` 或 `InvalidLayout`，由错误阶段区分；禁止猜测 |

## 3. 修改项分解

### 16-00 前置核证与真实语料门

| 子任务 | 内容 | 完成证据 |
|---|---|---|
| 16-0001 真实 A2L 语料接收 | 提供至少包含标量成员、固定数组、嵌套结构、非零 ECU_ADDRESS_EXTENSION、至少一个不可执行/异常结构的脱敏 A2L | fixture 路径、来源说明、脱敏确认 |
| 16-0002 上游 API 核证 | 核对 `TypedefMeasurements()`、TYPEDEF_STRUCTURE 成员字段、INSTANCE 地址/扩展和 MATRIX_DIM 的实际运行语义 | 核证表：API、字段、原始值、可否导出 |
| 16-0003 当前基线锁定 | 记录批次15 ON/OFF、ABI v5、prepared root、A2lIsolation、格式基线 | 实测命令与结果 |
| 16-0004 失败样本先行 | 对真实语料和合成 fixture 先建立至少一条“当前首版必须失败”的叶子用例，防止实现后补永真断言 | 红灯日志保留在实施记录 |

> 若 16-0001 的真实 A2L 仍未提供，本批停止在核证阶段，不使用自造语义替代真实语料；可先完成不依赖语料的 DTO/API 设计，但不得标记 leaf 执行能力完成。
>
> **✅ 2026-07-29 门已解除**：批次17 测试 Slave 运行时生成的 `xcp_test_slave.a2l`（纯测试符号，天然脱敏）入库为真实语料 `tests/data/a2l/xcplite_structleaf_corpus.a2l`；本批全部范围由批次18 实施完成（见 B16-P3）。

### 16-A SDK 类型事实导出（ABI v6）

| 子任务 | 落点 | 修改内容 |
|---|---|---|
| 16-A01 TYPEDEF_MEASUREMENT DTO | `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp` | 新增 `TypedefMeasurementDto`：名称、描述、模块、数据类型原码、COMPU 引用、分辨率/精度/上下限、BYTE_ORDER、RECORD_LAYOUT 相关可证字段；字段只存上游事实，不在 SDK 层推导叶子地址 |
| 16-A02 IDoc 查询接口 | 同上 | 新增 `ListTypedefMeasurements(std::vector<TypedefMeasurementDto>*) const noexcept`；明确空快照、`nullptr`、异常转码语义 |
| 16-A03 ABI 版本升级 | `kLibA2lAbiVersion` | v5→v6；旧/新版本工厂不匹配时明确失败 |
| 16-A04 SDK 快照采集 | `thirdparty/a2l-sdk/src/liba2l_export.cpp` | 在 `BuildSnapshots()` 采集每个 MODULE 的 `TypedefMeasurements()`；保留原始类型、COMPU、边界、版式关联；按 module/name 稳定排序 |
| 16-A05 结构成员事实补全 | `BuildStructureInfo` / `StructMemberDto` | 确认成员引用 TYPEDEF_MEASUREMENT、TYPEDEF_STRUCTURE、矩阵维度、偏移、布局等字段完整；不递归、不丢弃未知原码 |
| 16-A06 SDK 导出单测 | `tests/` | 正常、空容器、未知/缺失引用、非零扩展和 ABI 不匹配测试 |

### 16-B 桥接层类型解析与叶子索引

| 子任务 | 落点 | 修改内容 |
|---|---|---|
| 16-B01 类型快照容器 | `a2lbridge` 内部 DTO map/database | 保存 TYPEDEF_MEASUREMENT、TYPEDEF_STRUCTURE、INSTANCE 三类事实；保留原始名称和模块作用域 |
| 16-B02 `StructureLayoutResolver` | 新增桥接层内部头/源文件 | 输入结构定义和实例，输出叶子路径、地址偏移、扩展地址、元素宽度、数组维度、类型/换算引用及拒绝原因 |
| 16-B03 标量成员展开 | resolver | TYPEDEF_MEASUREMENT 成员解析为可执行叶子；继承 BYTE_ORDER、COMPU、边界和 RECORD_LAYOUT 事实 |
| 16-B04 固定数组展开 | resolver | 按 `MATRIX_DIM` 计算维度乘积、元素步长和总字节数；索引零基，维度/索引越界结构化失败 |
| 16-B05 嵌套结构展开 | resolver | TYPEDEF_STRUCTURE 引用递归展开；累加成员偏移；保留路径前缀和每一级 address_extension 语义 |
| 16-B06 递归环检测 | resolver | 使用 DFS 访问栈检测自环和间接环；错误包含完整类型链，不进入叶子索引 |
| 16-B07 冲突与重复路径 | database/index | 同模块同路径、大小写冲突、实例名冲突和重复叶子拒绝或告警，不能选择第一条 |
| 16-B08 叶子索引发布 | `A2lDatabaseImpl` / snapshot publish | 快照一次性发布；普通 STRUCTURE/INSTANCE 本体仍保留元数据，叶子作为可查询路径索引 |

### 16-C 公开查询与数值操作语义

| 子任务 | 落点 | 修改内容 |
|---|---|---|
| 16-C01 叶子查询接口 | `IA2lDatabase` / `A2lDatabaseImpl` | 扩展 `Find`/`Search` 使完整叶子路径可查；明确裸名不跨结构体猜测，优先要求限定路径 |
| 16-C02 叶子 SymbolInfo | `SymbolInfo` 或等价内部/公开结构 | 填充地址、address_extension、data_type、element_size、dimensions、conversion、record layout 执行状态 |
| 16-C03 `ByteSizeOf` | 数据库实现 | 标量返回元素宽度；固定数组返回总字节数；嵌套结构只对可证明叶子/路径返回，不放行结构体整块 |
| 16-C04 `ComputeElementAddress` | 既有地址工具 | 使用偏移+数组步长；AG 只用于元素计数/对齐，不改变 ECU 基址；检查 32 位地址溢出 |
| 16-C05 `ToPhysical` | 数据库实现 | 仅对可执行标量叶子换算；数组整体换算按明确接口契约执行或拒绝，不隐式取首元素 |
| 16-C06 `FromPhysical` | 数据库实现 | 复用既有边界/类型/COMPU 策略；错误带完整 leaf path |
| 16-C07 读写寻址 | `MemoryAccess` / `A2lBridge` 现有通路 | 叶子读写使用解析出的地址和扩展；禁止调用方自行拼接偏移；失败保留结构化阶段和路径信息 |
| 16-C08 不可执行拒绝 | database / record layout | 缺类型、尺寸不一致、复合 RECORD_LAYOUT、递归环、未知 COMPU 均明确拒绝 |

### 16-D fixture、测试与门禁

| 子任务 | 测试落点 | 覆盖内容 |
|---|---|---|
| 16-D01 生成器扩展 | `tests/a2l_gen` | 标量成员、UWORD×4 数组、两层嵌套、数组嵌套结构、非零 INSTANCE extension |
| 16-D02 SDK DTO 测试 | SDK/桥接测试 | TYPEDEF_MEASUREMENT 字段完整、稳定排序、ABI v6 版本校验 |
| 16-D03 路径解析测试 | `a2l_golden_test.cpp` | 路径、裸名歧义、索引、嵌套路径、重复/冲突路径 |
| 16-D04 地址与尺寸测试 | golden | 偏移、数组步长、总字节数、AG 不乘地址、溢出和 extension 透传 |
| 16-D05 换算测试 | golden | IDENTICAL/LINEAR 等可执行 COMPU；边界、类型不匹配、不可执行版式 |
| 16-D06 读写 E2E | `a2l_e2e_test.cpp` | 叶子路径→地址→读；FromPhysical→写→读回→ToPhysical；非零扩展路径 |
| 16-D07 失败路径测试 | golden/E2E | 缺失 typedef、递归环、越界索引、未知类型、复合 RECORD_LAYOUT、重复路径 |
| 16-D08 DAQ 兼容测试 | golden/E2E | 叶子可作为显式 DAQ entry；结构体本体仍拒绝加入/换算，不改变 F1～F4 语义 |
| 16-D09 双 ABI 门禁 | Release/Debug prepared root | ABI v6 双侧重建、头/库同侧、旧 ABI 工厂拒绝 |
| 16-D10 ON/OFF 与隔离门禁 | 主树 | ON/OFF ctest、A2lIsolation、P9、clang-format |

## 4. 依赖关系

```text
16-00
  → 16-A01 → 16-A02 → 16-A03 → 16-A04 → 16-A05
  → 16-B01 → 16-B02 → {16-B03, 16-B04, 16-B06} → 16-B05 → 16-B07 → 16-B08
  → {16-C01, 16-C02} → 16-C03 → 16-C04 → {16-C05, 16-C06, 16-C07, 16-C08}
  → 16-D01～16-D08 → 16-D09 → 16-D10
```

可并行项：16-A06、16-B06、16-D01 可在不修改同一接口时并行；ABI 重建、全量构建、全量测试和文档回写必须串行。

## 5. 风险与控制

| 风险 | 控制措施 |
|---|---|
| 上游 TYPEDEF_MEASUREMENT 字段语义与预期不符 | 16-0002 核证不过即停止，不在桥接层自造事实 |
| 结构递归导致栈溢出 | DFS 访问栈、最大深度限制、环路径错误信息 |
| 数组维度乘积溢出 | 使用 checked multiplication，失败为 InvalidLayout |
| 成员宽度与 RECORD_LAYOUT 不一致 | 先判版式可执行性，再允许 ByteSizeOf/读写 |
| extension 在嵌套成员中语义不一致 | 真实语料核对；不确定时拒绝，不把父扩展静默覆盖子扩展 |
| 叶子路径改变既有 Search 结果 | 默认保持结构体本体结果；叶子按限定路径查询，必要时增加显式查询选项 |
| ABI v6 与旧 prepared root 混用 | Release/Debug 双侧同批重建，A2lSmoke ABI 常量断言 |
| 真实样本不足 | 真实语料是 16-00 硬门；合成 fixture 只能补边界，不能替代语义核证 |

## 6. DoD

> **实现状态（批次18 闭合）**：第 1–6 条全部达成（证据见批次18 实施记录 §0/§5 门禁与 §6 映射表）；第 7 条设计文档回写以 **R4.9 行**落地（§4.1/§4.4 处为历史草案块，按 R4.6-⑧ 字段名锚点纪律不回改正文），实施记录由 `XCPlite_Slave_协议调试_实施记录_批次18_结构叶子展开.md` 承担。

1. 真实脱敏 A2L 能成功导出 TYPEDEF_MEASUREMENT、结构定义和实例地址事实。
2. 标量、固定数组、嵌套结构和嵌套数组叶子路径可查询；地址、尺寸、extension 和索引正确。
3. 可证叶子完成读写和物理换算闭环；不可证路径全部结构化拒绝，不静默返回 0 或整块字节。
4. 递归环、缺失类型、越界索引、重复路径、宽度/版式不一致均有反例测试。
5. 原 STRUCTURE/INSTANCE 本体仍保留元数据，整块 ByteSizeOf/ToPhysical/FromPhysical 继续拒绝。
6. ABI v6 Release/Debug prepared root 同步；ON/OFF ctest 全绿；A2lIsolation/P9/clang-format 通过。
7. 设计文档 §4.1、§4.4、§6.3、§9.1、B 类语义文档 §5 和验证集均回写；新增批次16实施记录。

## 7. 交付物

- 本计划：`A2L_集成_R4_批次16_STRUCTLEAF实施计划.md`
- 实施记录：`A2L_集成_R4_实施记录_批次16_STRUCTLEAF.md`（**实际由批次18 记录承担**：`XCPlite_Slave_协议调试_实施记录_批次18_结构叶子展开.md`）
- ABI v6 Release/Debug prepared root
- 真实脱敏 A2L fixture 与生成器/回归测试
- 设计文档与 B 类语义文档更新

## 8. 变更记录

| 轮次 | 内容 |
|---|---|
| B16-P1 | 根据批次15 D5，确定 STRUCTURE/INSTANCE leaf 完整闭环范围：标量、固定数组、嵌套结构、递归环检测、地址/尺寸/换算/读写、不可证路径明确拒绝；确定 SDK 导出事实、桥接层递归解析、ABI v6、真实 A2L 前置核证和 ON/OFF 双侧门禁。 |
| B16-P2 | 用户确认当前没有真实脱敏 A2L；按计划 §3 的 16-0001 硬门暂停代码实现，不用自造语义替代真实上游字段。 |
| B16-P3 | **语料门解除并移交批次18 实施（2026-07-29）**：批次17 对手端运行时 A2L 入库为真实语料；批次18 完成 16-A/B/C/D 全部范围（SDK v6、`structure_layout.cpp` 展开器、真实语料 42 符号 0 告警、负例 6 形态告警化、E2E SKIP→PASS、ON 377/OFF 352）。唯一有意延后项 = 16-D08 DAQ 叶子兼容（随批次20）。本计划就此关闭，不再独立实施。 |

(End of file)
