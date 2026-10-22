# cpp_demo_V201 A2L 支持补全方案

> 状态：P0已完成；SDK/bridge、索引/分页详情UI和安全写路径已实现，Release增量构建通过；聚焦CTest 18/18通过。尚未做全量测试、真实FTXUI事件循环、真实Slave联测及Windows/Linux验证。
> 实施边界：用户明确不修改 `thirdparty/a2llib`。原始附件只读，改用独立的经消歧测试语料。
> 范围调整：不承诺从原始 A2L 的 `a2llib` 名称映射中识别 `C_delay_us` 的重复定义；完整 VALUE 写入验收只针对 typedef 名称唯一、INSTANCE 明确声明 `READ_WRITE` 的修正版 A2L。
> 目标：补齐该语料的结构化标定参数、固定曲线与测量数组支持，同时保留保守的寻址、写入和确认边界。

## 1. 分析结论与现有边界

本方案基于用户提供的 cpp_demo_V201 A2L 文本及当前源码核证；经消歧测试语料已纳入 `tests/data/a2l/cpp_demo_V201_corrected.a2l`，并通过当前 A2lGolden 测试。该结果不代表原始附件的重复 typedef 可消歧，也不代表真实 ECU 联测。

- 该文件包含 10 个 MEASUREMENT：9 个标量、1 个 128 元素数组。
- 标定数据通过 INSTANCE → TYPEDEF_STRUCTURE → TYPEDEF_CHARACTERISTIC 定义，不是直接展开的顶层 CHARACTERISTIC：14 个 VALUE 成员、2 条各含 11 个点的 CURVE。
- [加载入口](../thirdparty/a2l-sdk/a2lbridge/src/a2l_bridge.cpp)现经独立的 TYPEDEF_CHARACTERISTIC DTO/API 展开 VALUE/CURVE；[叶子展开](../thirdparty/a2l-sdk/a2lbridge/src/structure_layout.cpp)保留类型、固定轴及唯一 MEMORY_SEGMENT/PAGE 关联，拒绝无法证明的重叠/歧义布局。
- A2L黄金测试覆盖修正版语料中标量、128元素MEASUREMENT数组、固定轴曲线点与页访问事实；原始附件的重复 typedef 仍不具备可消歧保证。
- [SDK 公共 API](../thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp)已导出 `ListTypedefCharacteristics`、`ListMemorySegments`，ABI版本为8；MEASUREMENT API职责保持不变。
- [工具策略](../tools/xcp_master/symbol_policy.hpp)允许安全的一维MEASUREMENT索引/分页读取与固定轴CURVE单点读写；MEASUREMENT不能写入/加入DAQ数组，任意结构、MAP和不匹配布局仍拒绝。
- A2L 的 C_delay_us 在同一 MODULE 重复定义，上限分别是 999999 和 100000。不能用“先到/后到覆盖”猜测两处引用各自的含义。
- 各标定内存段 PAGE 1 明确禁止 XCP 写入；PAGE 0 的 DONT_CARE 不等同于从站保证写入成功。

## 2. 支持范围与验收口径（已确认）

### 2.1 测量项

| 符号 | 目标能力 |
|---|---|
| SigGen1.value_、SigGen1.time | 标量读取；事件 1 的 DAQ 能力按现有 profile/从站兼容性验证 |
| SigGen2.value_、SigGen2.time | 标量读取；事件 2 的 DAQ 能力按现有 profile/从站兼容性验证 |
| temperature、speed、counter、sum、loop_cycletime | 标量读取；事件 0 的 DAQ 能力按现有 profile/从站兼容性验证 |
| loop_histogram | 128 个 ULONG 的分页读取及索引查看，不开放写入 |

READ_WRITE 测量量写入不在本方案范围内。数组观测先通过显式读取完成，不把整数组自动加入 DAQ。文件声明相对 PID 标识及不同地址扩展，不能仅凭符号通过标量门便声称 DAQ 可执行；前置核证需检查现有 profile，并对不兼容配置明确拒绝。

### 2.2 标定项

| 路径 | 类型 | 声明范围/语义 |
|---|---|---|
| kParameters.counter_max | VALUE / UWORD | 0–2000 |
| kParameters.delay_us | VALUE / ULONG | C_delay_us 重名，范围待消歧 |
| SigGen1.signal_type、SigGen2.signal_type | VALUE / UBYTE | 0–4：SINE、SQUARE、TRIANGLE、SAWTOOTH、ARBITRARY |
| SigGen1.ampl、SigGen2.ampl | VALUE / FLOAT64 | 0–100 Volt |
| SigGen1.phase、SigGen2.phase | VALUE / FLOAT64 | 0–6.28319 |
| SigGen1.offset、SigGen2.offset | VALUE / FLOAT64 | −100–100 Volt |
| SigGen1.period、SigGen2.period | VALUE / FLOAT64 | 0.01–10 s |
| SigGen1.delay_us、SigGen2.delay_us | VALUE / ULONG | C_delay_us 重名，范围待消歧 |
| SigGen1.lookup.values、SigGen2.lookup.values | CURVE / FLOAT32 | 各 11 点，值域 −1–1，固定轴 0–10 |

验收分为两种：

1. **原始附件边界**：保持只读，不作为完整标定写入验收。当前 `a2llib` 按类型名存储且静默忽略重复插入，不能从已解析模型证明 `C_delay_us` 唯一；不声称可准确显示其限值或提供可写资格。
2. **经用户确认的修正版语料完整支持**：14 个 VALUE 和两条曲线通过元数据、读取、写入及回读验证。生成端将 C_delay_us 拆为主循环与信号发生器不同类型名，并在对应 INSTANCE 明确声明 READ_WRITE。修正版仅作为独立测试/交付语料，不覆盖原始附件；不按实例名猜测范围。

“可写”还须满足布局、换算、页访问及从站权限检查；不以 A2L 分类替代运行时成功证据。

## 3. 架构与组件方案（已确认）

### 3.1 SDK：导出标定类型事实

已新增 `TypedefCharacteristicDto` 与 `ListTypedefCharacteristics`，独立保留 VALUE/CURVE 类型、宽度、换算、范围、RECORD_LAYOUT、维度和固定轴事实；另新增 `MemorySegmentDto` 与 `ListMemorySegments` 导出静态段/PAGE。SDK ABI 版本为8；`ListTypedefMeasurements` 保持原职责，不把标定事实伪装成 MEASUREMENT。

`a2llib` 当前按名称 map 存放 typedef characteristic，并静默忽略重复插入；用户明确不授权修改该子模块。因此不新增底层重复定义解析，测试与完整写入支持只针对经消歧的 A2L。原始附件中的 `C_delay_us` 限值与可写资格不作保证；原始附件不参与可写验收。

SDK DTO/公共虚函数变更须递增 ABI 版本，验证导出符号并重建所有消费者，禁止新旧 SDK 二进制混用。

### 3.2 Bridge：保留类型语义的结构叶子展开

在现有递归展开中增加 TYPEDEF_CHARACTERISTIC 分支：

- VALUE 叶子生成 Characteristic + Value，不能降格为 Measurement。
- CURVE 保留 Characteristic + Curve、11 点数据区与固定轴元数据，不伪装为普通 VALUE 数组。
- 地址由 INSTANCE 基址、嵌套成员偏移和可证布局共同计算；地址扩展独立传递。
- 继承递归深度、叶子预算、循环引用、整数溢出、成员越界、宽度未知等安全检查。
- 将标定叶子的 RECORD_LAYOUT 信息登记到执行校验使用的索引中；不能只插入 SymbolInfo 而遗漏布局约束。
- 类型歧义阻断依赖该类型的叶子，产生可定位到实例成员和定义的诊断；不拖累其他可证叶子。
- 整体 INSTANCE/STRUCTURE 仍仅作为元数据，禁止整块任意内存读写。

建议用语料中的已知布局作为验收锚点：kParameters 基址 0x80010000；SigGen1/2 基址分别为 0x80020000/0x80030000。信号参数的 ampl/phase/offset/period 偏移为 0x00/0x08/0x10/0x18，lookup.values 为 0x20，delay_us 为 0x4C，signal_type 为 0x50。11 个 FLOAT32 占 44 字节，必须验证不越过后续成员。

### 3.3 写权限与页访问

INSTANCE 的 `READ_WRITE` 是上游语法可表达的访问属性；用户附件未声明，故原始附件实例不可作为可写授权。修正版测试语料须显式声明 `READ_WRITE`。属性缺失按只读处理，不从 TYPEDEF_CHARACTERISTIC 自行推导可写。

写入授权取元数据、布局/换算、页访问、会话保护和从站能力的共同约束：

- SDK 导出 MEMORY_SEGMENT 范围与 XCP 页写属性；通过现有 GET_CAL_PAGE/GET_PAGE_INFO 查询运行时页状态及访问属性。
- PAGE 1 明确拒绝，不发送 DOWNLOAD；PAGE 0 的 DONT_CARE 不作成功承诺，仍处理从站保护与访问失败。
- 页面与内存段归属只在唯一可证时使用；地址范围重叠、页映射缺失或无法确认页状态时不猜测。
- 不自动切页、解锁或 COPY_CAL_PAGE；需要切页时明确拒绝并说明理由。
- 跨主站/ECU的活动页切换存在固有限制：本进程互斥不能阻止外部主站/ECU在最终页检查与WRITE之间切换；后续页检查只能检测部分变化，不能撤销可能写入错误页的操作。

### 3.4 XCP Master UI/命令

保留现有命令区/显示区分离及模态确认，不做无关 UI 重构。

- 标量 VALUE：复用详情、编辑、预读、默认取消确认、发送及回读路径。
- signal_type：显示数值与枚举标签，验证正反向转换一致性及非法枚举输入；默认不超出 0–4。
- loop_histogram：增加数组分页/索引读取视图，保留数组身份和索引，不开放标定入口。
- 两条 CURVE：专用固定轴/数值表格，先支持单点编辑及回读；整条曲线批量写入、动态轴、MAP 和通用图形曲线编辑器不在本轮范围。
- 禁止简单放宽 SymbolReadUnsupportedReason/SymbolWriteUnsupportedReason 来让所有数组或曲线通过标量流程。新增容器/元素操作需有独立范围与地址校验。
- UI 显示SDK可证明的能力及拒绝原因：类型缺失、布局不支持、页只读、转换不可逆等，不全部归为“仅支持 VALUE”。若上游已静默丢弃重复定义，不能伪造逐符号诊断；须在A2L级限制中明确说明。

## 4. 数据流与失败处理（已确认）

加载：经消歧 A2L → 类型/布局事实 → 结构成员展开 → 带来源和能力诊断的符号快照 → UI。原始附件仅作为只读输入保存，不对其重复 typedef 的准确限值/可写资格作承诺。

读取：选择叶子/索引 → 校验类型、边界和地址 → 现有传输读取 → 原始值转换 → 显示值及状态。扩展地址不能与主地址合并；不假定相同数值地址代表同一变量。

写入：选择 VALUE 或专用曲线点 → 校验转换/限值/页权限 → 预读 → 展示完整路径、索引、地址扩展及新旧值 → 用户确认 → Dispatch 前重检必要状态 → 写入 → 回读核对。

- 写入许可绑定具体符号、索引、目标值及必要页状态；修改草稿、换对象或取消不得复用旧许可。
- 取消先于 Dispatch 生效时零写入；已经在途的 XCP 请求不得宣称可撤销。
- 写后超时或回读不一致报告“状态未确认/不一致”，不自动重试写入，不宣称回滚成功。
- 数组分页读取并非原子快照，应标明读取批次/时间，避免把跨时刻数据称为一致采样。

## 5. 分阶段实施建议

| 阶段 | 工作 | 退出条件 |
|---|---|---|
| P0 核证与基线 | 已完成：核对 SDK TYPEDEF_CHARACTERISTIC 实际导出、a2llib 重名丢弃、INSTANCE 权限缺省及 XCP 页查询接口；用户选择不改 a2llib | 以已确认边界实施：原始附件不作重复诊断/可写验收，独立修正版语料用于完整能力验证 |
| P1 SDK/桥接 | 已实现：ABI 8、TYPEDEF_CHARACTERISTIC/MEMORY_SEGMENT DTO/API、VALUE/CURVE 结构叶子和固定轴事实 | `A2lGolden` 含修正版语料测试通过；未覆盖原始重复 typedef |
| P2 标量闭环 | 已接入保守读写策略、A2L READ_WRITE、静态及运行时 PAGE 检查、确认/回读路径；尚待mock端到端覆盖及真实Slave验证 | `XcpMasterSymbolPolicyTest` 及 `A2lGolden` 当前通过；写入未联机执行 |
| P3 容器能力 | 已实现一维MEASUREMENT 8项分页/索引读取及固定轴CURVE单点读写详情路径 | 分页范围、元素布局和A2L黄金策略测试通过；真实终端事件与ECU读取/写入未验证 |
| P4 联测与文档 | README与实施记录已更新；真实从站验证、全平台构建待做 | 各项有独立结果与未覆盖边界，不用局部结果代替全量通过 |

不因 P2 完成就宣布该文件“完整支持”；P3 完成也不意味着新增通用数组 DAQ、MAP 或任意布局支持。

## 6. 测试与验收建议（已确认）

已确认采用“离线自动化必做、真实 Slave 联测作为运行时完成门”的方案，本节全部测试范围纳入后续实施验收。联测环境的实际接入信息仍需在实施阶段核实；同意测试方案不代表环境已经接入或测试已经通过。

所有新增回归放在 tests 下；具体新增文件名、目标及测试注册方式按现有测试结构确定，不修改 thirdparty 内测试文件作为替代。

- 语料：原始附件保持外部只读；在 tests 下建立明确差异的经消歧 fixture，C_delay_us 使用唯一类型名，标定 INSTANCE 显式 READ_WRITE。
- SDK：VALUE/CURVE/布局/单位/限值/枚举事实保真；ABI 与全部消费者链接验证。重复 typedef 检测不在本次范围，因为用户选择不修改 a2llib。
- 结构叶子：经消歧语料的 14 个预期路径、嵌套曲线、准确地址/扩展；原始附件中的 3 个重复名成员不作为限值或写入验收。
- 数组/曲线：128 个 ULONG 与 11 个 FLOAT32 的宽度/索引；0、末尾、越界、负索引、溢出、分页尾页；固定轴 0–10 不占用虚构的存储区。
- 换算：temperature 线性换算、loop_cycletime 单位/换算、signal_type 五个枚举及非法标签、浮点边界与非有限值。
- 写保护：页只读、未知权限、受保护会话、错误布局、歧义类型、越界输入时 mock 发送次数为零。
- 确认：默认取消、Esc/F5、resize、对象/索引切换、Dispatch 竞态；写后超时和回读差异不得报告成功。
- UI：真实 FTXUI 事件驱动的焦点、分页、编辑、取消与窗口缩放测试，不能仅以纯模型测试替代。
- DAQ：9 个标量对应事件分组、相对 PID 兼容性和不同 address_extension 的隔离；若现有 profile 不支持则展示明确拒绝，不隐式使用错误解码。
- 构建：优先 Release；完整构建前先 clean；实施时记录定向测试与全量测试的独立结果。Windows/Linux/macOS 验证未执行的项目必须明确列出。

本轮Release增量构建：`xcp_master`、`XcpMasterSymbolPolicyTest`、`A2lGolden` 均成功。聚焦命令 `ctest --test-dir cmake-build-release-a2l --output-on-failure -R 'XcpMaster|A2lGolden|a2l_gen'`：**18/18通过**，新增分页范围算法边界用例，并覆盖策略、A2L黄金测试/fixture生成及CLI帮助参数测试。环境 Python 3.9 不支持 `Path.write_text(newline=...)`；生成器已改用 `Path.open(..., newline="")` 并随 `a2l_gen` 测试通过。构建仍有 GoogleTest `char8_t` 转换警告和重复链接库提示。尚未运行全量 CTest、真实 FTXUI 事件循环、真实 Slave 或 Windows/Linux。

## 7. 修改边界与评审清单

预计涉及 SDK 公共事实接口、bridge 的加载/结构叶子/布局索引、工具能力策略与容器 UI，以及 tests 和说明文档。实施不得顺带重构已完成代码。

SDK 与 bridge 均位于 thirdparty 下，其修改已获原方案授权。用户另行明确选择不修改 `thirdparty/a2llib`；本次不得更改该子模块及其测试。原始附件的重复 TYPEDEF_CHARACTERISTIC 不作诊断/写入支持，完整验收改用独立修正版语料。

### 7.1 已确认决策

用户已同意全部待确认项，记录如下：

1. 范围：包含 VALUE、两条固定曲线单点标定、histogram 分页读取；不开放 MEASUREMENT 写入或数组 DAQ。
2. 架构：SDK 导出事实、bridge 保留标定类型和布局、UI 按能力分流。
3. 数据与安全：重复类型拒绝、只读页阻断、禁止自动切页和隐式写入重试。
4. 测试：第 6 节自动化测试全部纳入，真实 Slave 联测作为运行时完成门；环境接入与可用性在实施阶段实际核验。
5. 消歧：由生成端修复 C_delay_us 类型名冲突、为标定 INSTANCE 明确声明 READ_WRITE，并提供修正版 A2L；原始附件保持只读，不在工具中猜测范围。
6. 子模块边界：用户明确选择不修改 `thirdparty/a2llib`；因此原始重复 typedef 的检测/拒绝不纳入本次验收，修正版 A2L 是完整写入支持的输入前提。

### 7.2 确认不替代事实核证

- 经消歧语料已作为独立 fixture 纳入测试并通过 `A2lGolden`；这不解决原始附件中三个 `C_delay_us` 引用的限值/权限歧义。
- P0 API/ABI/页语义核证已完成，SDK ABI 为8；DAQ profile 与真实ECU行为仍未联测。
- 已修改 SDK、bridge、xcp_master、tests 和测试生成器；P1落地，P2保守读写路径已接入，P3索引/分页UI已接线，README/实施记录已更新，真实Slave/跨平台仍待验证。

## 8. 文档自审

- 已区分 A2L 定义能力、源码静态结论和运行时验证结果。
- 已避免把“未生成标定叶子”描述成“已生成但全部错标为 Measurement”。
- 已区分 14 个声明的 VALUE 与原始语料中 11 个无歧义、3 个待消歧成员。
- 未假定未声明 READ_WRITE 就意味着标定项只读，也未将 PAGE 0 DONT_CARE 当作成功保证。
- 新增 TYPEDEF_CHARACTERISTIC/MEMORY_SEGMENT API 后 ABI 升至8并重建消费者；真实Slave交互仍未验证。
- 未扩大到任意 CURVE/MAP、自动解锁、自动切页或通用数组 DAQ。
- 测试结论限定为上述聚焦 18/18；未宣称全仓库、跨平台或 ECU 联测通过。原始附件重复 typedef 限制与本次不改 `a2llib` 的边界仍明确。
