# XCPlite cpp_demo 基础测量与标定闭环——迭代修复计划

> 状态：R0–R4 已完成。Windows Release ON/OFF 全量测试通过；本轮代码与测试证据、一次瞬时失败及复跑记录见 `code-plan/XCPlite_cpp_demo_基础测量标定闭环_修复实施记录.md`。
> 目标：以当前 vendored XCPlite 的原版 cpp_demo 为对手端，完成“测量物理值正确 + 在线标定实际生效 + 恢复原值”的最小闭环。
> 本计划不以其他 ECU、Linux/macOS、Seed&Key 或全 XCP 命令覆盖作为本次最小集验收条件；也不将本次结果宣传为这些能力已通过。

## 1. 现状与问题证据

### 1.1 已具备的基础

- `examples/xcp_master_udp/CMakeLists.txt:20-25` 直接引用上游 cpp_demo 的 main/sig_gen/lookup 源码构建对手件。
- `examples/xcp_master_udp/xcp_master_udp.cpp:281-328` 实现参数原值读取、写入、读回、恢复与再读回；既有示例覆盖 kParameters.counter_max 和 kParameters.delay_us，使用实例基址加固定偏移。
- `code-plan/XCPlite_cpp_demo_实施记录_批次22_示例程序交付.md:28-40` 记录两个参数的写读恢复结果。
- `code-plan/libxcp_测量子系统_L4冒烟_cpp_demo_记录.md:31-67` 记录 cpp_demo 600 秒多事件 DAQ 冒烟，约 170 万帧、零丢弃和零解码错误。这是历史证据，不能替代本轮代码修改后的复测。
- `examples/measurement_demo/CMakeLists.txt:38-59` 表明默认 ExampleMeasurementDemo 使用 xcp_test_slave，不能把该默认测试通过当作原版 cpp_demo 专项回归通过。

### 1.2 本轮必须关闭的问题

| 编号 | 问题 | 证据及影响 |
|---|---|---|
| F-01 | LINEAR 标准顺序已核实为 factor、offset；SDK 导出映射和桥接公式与标准不符 | ASAM LINEAR 为 `PHYS=a*INT+b`，`COEFFS_LINEAR a b`；XCPlite 按 factor、offset 写出，a2llib parser 保序。但 SDK 将首值映射到 O、次值映射到 C，bridge 又将 O 加入斜率，导致 cpp_demo 的 `1 -50` 在 raw=50 时得到 -2450，而应为 0°C。详见下方 R0 取证表。 |
| F-02 | 冒烟通过未覆盖 temperature 物理值正确性 | 历史 L4 记录明确去除了 temperature 值域断言，仅保留覆盖与变化性，不能视为该缺陷已解决。 |
| F-03 | 参数写读验证尚未形成运行效果闭环 | 既有两个参数写读测试只证明存储值可修改；未形成原版 cpp_demo 上“DAQ 持续采集→修改参数→算法输出按预期变化→恢复”的专项自动验收。 |
| F-04 | 功能完成范围易被扩大 | cpp_demo 还包含 SignalGenerator 的幅值/偏移/周期/枚举及 lookup 曲线；两个参数写读与标量 DAQ 不证明全部对象、整结构或曲线编辑已完成。 |

说明：F-01 是端到端产品缺陷，不因位于 thirdparty 而消失；但任何第三方修改必须另行获得明确授权。R0 已完成规范核证，结论及数据流见 §4 R0。历史 golden 用例只证明旧实现自洽，不能推翻标准。

## 2. 本次范围与非目标

### 必须交付

1. 明确并修正 LINEAR 系数从 A2L 文本到物理值的完整语义链，正向/逆向一致。
2. temperature 的原始值与物理值严格对应验证，恢复值域断言。
3. 原版 cpp_demo 的在线标定与 DAQ 运行效果闭环自动测试，含原值恢复。
4. Windows Release 专项及 ON/OFF 回归、修改记录和能力边界说明。

### 不纳入本次最小集

- 修改 cpp_demo 算法以迎合测试，或以 xcp_test_slave 替换原版 cpp_demo 验收。
- 全部信号发生器参数、枚举、lookup 曲线/轴、整结构标定的产品化接口。
- 新增通用标定 UI、批量事务、完整 Page Switching 测试、NVM/Flash 持久化、STIM、Seed&Key。
- 异构 ECU/CANape 认证、所有传输及跨平台互操作。

上述非目标不是“不支持”的最终判断，只表示本轮不承诺实现或验收。

## 3. 实施纪律与批准门禁

- 遵守 `AGENTS.md`：C++20；代码/API 设计确认后才实施；不重构已完成模块；新增或更新 tests 用例；每批输出 code-plan 修改与测试记录。
- 本计划最初仅获文档授权；实施前已完成 R0 核证并提交精确文件、语义、兼容性影响及测试方案，用户随后明确批准了列出的 thirdparty/a2l-sdk 源码修改。批准与实施结果见本轮实施记录。
- 第三方修改仍须逐项明确授权；未授权时必须停在核证与方案，不改源码，也不隐藏换算错误。
- 优先修正真正的错误层，不在 MeasurementSession 或示例中硬编码 `temperature = raw - 50` 作为生产补丁。测试使用独立公式作为 oracle 是允许的。
- 不直接修改 prepared SDK 的二进制冒充源码修复；源码、导出 ABI/语义版本与 Release 依赖产物必须一致并可追溯。

## 4. 迭代任务

### R0：LINEAR 规范与实际数据流核证（完成，代码只读）

#### 规范结论

ASAM LINEAR 方向为 INT→PHYS，公式 `PHYS = a * INT + b`，A2L 文本 `COEFFS_LINEAR a b` 因此依次是 **factor、offset**；逆向为 `INT = (PHYS - b) / a`（仅 `a != 0` 可逆）。依据：ASAM MCD-2 MC 规范镜像对 LINEAR 给出 `f(x)=ax+b` 及 `COEFFS_LINEAR 1.25 -2.0` 示例（[PDFCoffee 规范镜像](https://pdfcoffee.com/asamaemcd-2mcbsv1-6-1pdf-pdf-free.html)）；独立生成器资料明确列为 `[a b]`、`COEFFS_LINEAR a b`、`PHYS = a×INT+b`（[MathWorks GitHub reference](https://github.com/matlab/simulink-agentic-toolkit/blob/main/skills-catalog/code-generation/simulink-customize-a2l/references/compu-method-guidance.md)）。官方 ASAM 下载 URL 的正文抓取受当前网络 DNS 策略限制，故保留规范镜像/独立实现交叉证据，不声称已直接抓取官方 PDF。

#### 逐层取证

| 层 | 实际行为 | 证据/结果 |
|---|---|---|
| cpp_demo 注册/生成 | `A2lCreateLinearConversion_(..., factor, offset)` 原样写 `COEFFS_LINEAR factor offset`；温度是 `(1,-50)`，A2L 行为 `COEFFS_LINEAR 1 -50` | `thirdparty/XCPlite/src/a2l.c:924-932`；`thirdparty/XCPlite/examples/cpp_demo/src/main.cpp:143-145`；`build-v09/l4smoke/cpp_demo_V201.a2l:125` |
| 上游词法/语法解析 | `COEFFS_LINEAR any_float any_float` 按词面顺序压入 deque，无交换或语义命名 | `thirdparty/a2llib/src/a2lparser.y:1538-1542`；parser 动作保序 `thirdparty/a2llib/src/a2lparser.cpp:3708-3711`；容器 getter `thirdparty/a2llib/include/a2l/compumethod.h:26-31` |
| liba2l DTO 导出 | **错误**将 `lin[0]` 设为 O、`lin[1]` 设为 C、F=0 | `thirdparty/a2l-sdk/src/liba2l_export.cpp:584-597` |
| Bridge DTO 映射 | 原样搬运 c/o/f，无额外修正 | `thirdparty/a2l-sdk/a2lbridge/src/a2l_dto_map.cpp:163-169` |
| Bridge 正逆算 | 正向 `f + raw*c + raw*o`；逆向斜率 `c+o` | `thirdparty/a2l-sdk/a2lbridge/src/compu_method_eval.cpp:353-355,397-404` |
| 温度实算 | 生成系数 `(1,-50)` 被映射为 `c=-50,o=1,f=0`；raw=50 → `50*(-50)+50*1=-2450`，与旧记录一致；标准结果应为 0°C | 上述生成、映射及求值代码的直接推演 |

#### 错误被测试固化

`tests/a2l_gen/gen_a2l.py:91-95` 将参数命名为 `(o,f)` 并输出 `COEFFS_LINEAR offset factor`，`:485-493` 又按 `raw*factor + raw*offset` 计算期望；这是 generator 和语料的同一错序。输入 `tests/a2l_gen/golden_spec_basic.json:13`、`tests/a2l_gen/golden_spec_convert.json:18` 都使用 `[0.0,0.5]`。由此生成的 `build-v09/tests/a2l_gen/golden_convert.a2l:25-27`（`0.0 0.5`）及 `tests/a2l_golden_test.cpp:497-504`（raw=200→100）是自洽但不符合标准的 oracle。另有历史记录 `code-plan/libxcp_测量子系统_L4冒烟_cpp_demo_记录.md:80`、`code-plan/libxcp_测量子系统_L4冒烟_cpp_demo_进度存档.md:51` 曾将 offset-first 称作 ASAM 顺序；该结论现在被 R0 证据取代，R4 应追加更正注记而不是改写历史记录。

#### R1 设计及实施（已批准并完成；见本轮实施记录）

1. 在 `thirdparty/a2l-sdk/src/liba2l_export.cpp`：标准 `COEFFS_LINEAR` 首项映射为比例 C，次项映射为加法偏移 O，F=0；对非标准退化 `COEFFS` fallback 单独检查现有用户/语料，不能未经证据静默套用或交换。
2. 在 `thirdparty/a2l-sdk/a2lbridge/src/compu_method_eval.cpp`：统一 LINEAR 正算 `PHYS = F + C*raw + O`，逆算 `raw = (PHYS-F-O)/C`；O 不得进入斜率。同步修正 DTO/bridge 头文件中 `p=f+i*C+i*O` 等错误注释。
3. 修正 `tests/a2l_gen/gen_a2l.py` 的输入命名、`COEFFS_LINEAR` 输出顺序和 sample oracle 算式；更新 `golden_spec_basic.json`、`golden_spec_convert.json` 并重生成/替换错序 golden 样本；新增独立 factor/offset oracle（含温度 `1,-50` 的 raw 0/50/150→-50/0/100、比例 2、小数比例、负比例、零比例不可逆及正逆边界）；保留 IDENTICAL/RAT_FUNC/TAB 回归。
4. 通过 SDK 官方构建脚本重建 Release DLL/静态依赖，核实进程实际加载新产物；再恢复 cpp_demo 同帧 raw/physical 断言，并做 ON/OFF 回归。

**R0–R1 状态**：R0 技术结论完成；R1 修改已获用户明确授权并实施、测试通过。标准 `COEFFS_LINEAR` 按 factor/offset 修正；非标准 `COEFFS` fallback 保持旧语义且未宣称标准化。完整范围与构建证据见本轮实施记录。

### R1：LINEAR 换算修复与定向回归

经批准在最小责任层实施，候选涉及 SDK 导出映射和 bridge 求值器，实际修改范围由 R0 决定。

测试分三层：

- 求值单测：factor=1/offset=-50、factor=2/offset=-50、负比例、零偏移、小数比例；正向结果与独立 oracle 比对。
- 逆向单测：合法物理值 round-trip、零比例不可逆、NaN/Inf、整数范围与舍入规则按既有契约验证；不能仅以正逆互相抵消作为正确性证明。
- A2L 集成：从真实 cpp_demo 生成件或有来源的等价最小语料加载，断言 factor/offset 及最终值；另加标准语料防止只适配一个对手端。

temperature 确定性样本至少：raw 0→-50、50→0、150→100；clock_ticks 按其注册比例验证代表值。保留 IDENTICAL、RAT_FUNC、表格换算既有回归。

**完成条件**：定向负例先能暴露旧错误，修复后通过；依赖产物重建，确认运行时加载的是新 SDK，不是旧 DLL。

### R2：原版 cpp_demo 测量正确性专项门禁

1. 为 cpp_demo 注册独立专项测试，不依赖默认 ExampleMeasurementDemo 的 xcp_test_slave 路径。
2. 启动原版对手件，获取本次 A2L，显式核对事件绑定与 runtime profile，验证多事件 DAQ、有效样本、时间戳及 Stop/Start。
3. temperature 必须恢复合理值域检查；更关键的是对同一 DTO 样本的原始字节和物理值断言 `physical == raw - 50`，避免“值在范围内但换算仍错”及不同采样时刻的误比较。样本若不暴露 raw，则采用不改变公开 API 的测试观测点，实施前确认设计。
4. 对 speed/counter/sum/两路信号输出保留覆盖和动态变化断言；所有等值比较使用有依据的浮点容差。
5. 短时测试纳入 CTest；600 秒长采作为单独验收项，记录帧数、丢弃、解码错误、回绕及运行环境，不以短测替代。

**完成条件**：temperature 正确性不能跳过；专项日志证明对手端确为 cpp_demo。错误物理值必须使测试失败。

### R3：在线标定→算法效果→DAQ 观测→恢复闭环

推荐最小方案：使用已有 `kParameters.counter_max` 与 `counter`，不引入曲线或新的标定公共 API。

流程：

1. 独立运行目录启动 cpp_demo，确保不存在上次遗留持久化参数；核对所连进程身份、项目/版本和本次 A2L。
2. 从 A2L 核证 counter_max 地址、扩展、U16 宽度和布局；优先使用可证明的成员叶路径，若仍需基址+偏移，必须交叉核验实际 A2L，不照搬历史注释判定叶子能力不可用。
3. 读取并保存原始字节，DAQ 采集 counter；建立足够的基线，确认至少出现大于测试阈值的值。
4. 在 DAQ 不停止的情况下写入安全小阈值（建议 20，R0/设计确认时核定），读回确认。等待排空旧帧并观测新回绕周期后，验证 counter 持续在 0..20 内变化且重复回绕。
5. 恢复原始字节并读回；继续 DAQ，验证 counter 再次超过 20，证明算法效果恢复，而非仅内存读回恢复。
6. Stop、Disconnect、停止对手进程；保存结果与清理状态。

设计要求：

- 不用固定短 sleep 作为唯一判据；采用有界等待、连续帧/周期证据，记录超时原因。考虑旧 DTO 缓冲、事件频率、调度抖动与 UDP 丢包，不要求每个整数都必须被采到。
- 对时间窗和至少回绕次数给出有依据的常量，实施前确认。原参数不满足试验前提时明确失败并报告，不改生产值凑条件。
- 同一 Master 的命令操作与 DAQ 共存遵循现有线程模型；不引入并发 outstanding 命令。
- 任一断言失败或异常都尽力恢复原值；恢复失败必须单独报告，不能吞掉。响应超时 `OperationOutcomeUnknown` 不盲目重发，先按既有策略同步并查询状态；无法证明恢复时标记结果不确定并终止测试进程。
- 仅允许独立本地 cpp_demo 测试地址，不扩展到未知 ECU 或生产标定区。

**完成条件**：同一会话中同时证明“写入成功、运行结果改变、原值与运行结果恢复”。只验证内存字节、只重启后验证或改用测试 Slave 都不算通过。

### R4：回归、能力边界与记录

- Windows Release 先清理再执行完整构建；ON（A2L/XCPlite/examples）运行全量 CTest及原版 cpp_demo 专项；OFF 运行隔离构建与全量核心测试。
- 对 thirdparty SDK 源码修复同步构建实际依赖的 Release 产物，保留编译器/配置/依赖版本和加载路径证据。
- cpp_demo 硬编码端口 5555：测试串行，端口占用明确失败；不终止无关进程。每次用独立工作目录，防旧 A2L/bin 污染。
- 记录总数、PASS/FAIL/SKIP、失败首次结果与复跑结果，不沿用历史 513/460 作为本轮结果。
- 更新旧记录时保留历史事实，增加“由本轮修复与复测替代”的说明，不把历史错误结果改写为当时正确。
- 输出实施记录和能力矩阵：基本标量测量、物理换算、在线参数标定闭环已验证；未测对象/曲线/分页/持久化另列。

## 5. 验收矩阵

| 验收项 | 通过标准 |
|---|---|
| 系数语义 | 标准与 cpp_demo 生成件的顺序、映射、公式逐层有证据，无静默猜测 |
| 正向换算 | 固定样本与独立数学 oracle 一致，temperature raw=50 得到 0°C |
| 逆向换算 | 合法值往返正确，零比例/非法输入按契约拒绝 |
| 原版 cpp_demo DAQ | 专项测试实际运行原版对手件，多事件/时间戳/启停通过 |
| 真实样本正确性 | 同一 temperature 样本 raw 与 physical 精确对应，不移除断言 |
| 在线标定 | DAQ 持续运行时写 counter_max，counter 行为改变且可恢复 |
| 清理安全 | 成功/失败路径均有恢复证据或明确的不确定结果；不影响外部进程 |
| 回归 | 最新 ON/OFF Release 构建与测试结果可追溯，无新增未解释失败/跳过 |
| 结论范围 | 仅声明 cpp_demo 基础标量测量与参数标定闭环，不宣称所有对象或完整 XCP |

## 6. 顺序、停止条件与后续项

执行顺序：R0 → 设计/第三方修改授权 → R1 → R2/R3 → R4。

停止条件：缺少系数规范证据、第三方修改未获授权、运行时依赖无法确认、对手端身份不明、测试地址/布局无法证明、恢复结果不确定。不得以去除正确性断言或切换为模拟对手端规避。

最小闭环通过后，再按需求单独规划 SignalGenerator 幅值/偏移/周期的按名标定、lookup 曲线/轴、分页与持久化；不将这些项目自动追加到本次修复。

## 7. 本轮执行结果（Windows Release）

- R1：修复标准 `COEFFS_LINEAR` 导出顺序与 Bridge 正/逆算，修正 A2L generator、fixture、独立公式 oracle；R2：原版 cpp_demo 同帧 temperature raw/physical 检查；R3：在线修改 `counter_max=20`、DAQ 至少两次回绕、恢复原值并再次验证 counter 超过 20。专项均通过。
- ON（A2L/XCPLITE/examples/tests）：干净 Release 全构建成功；最终全量 CTest 515 项，514 通过、1 项既有 skip、0 失败，65.86s。此前同一最终代码的一次全量运行中，未修改的 `UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection` 短暂超时；单项复跑 1/1 通过，随后完整复跑全部通过。
- OFF（A2L/XCPLITE/examples OFF，tests ON）：干净 Release 全构建成功；全量 CTest 460 项，459 通过、相同 1 项既有 skip、0 失败，30.36s。
- SDK 由官方 `thirdparty/a2l-sdk/build-sdk.ps1` Release 流程从源码重建；运行时依赖根为 `build/liba2l-prepared/msvc-x64-release`。cpp_demo 两项专项使用本仓库 vendored XCPlite 原版 Slave，经本机 UDP，不代表异构 ECU/跨平台/商业工具互操作认证。
- 完整明细、工具链、瞬时失败原文和能力矩阵：`code-plan/XCPlite_cpp_demo_基础测量标定闭环_修复实施记录.md`。
