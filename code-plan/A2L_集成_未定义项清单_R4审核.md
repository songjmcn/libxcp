# A2L 集成设计：未定义项清单（R4 复核）

> 配套文档：`A2L_集成_a2llib选型与适配层详细设计_R4.md`  
> B 类决策基线：`A2L_接口语义_B类决策_R4.md`  
> **R4 更新**：B-1～B-20 已由用户逐项批准并全部关闭；A/C/D/E 类沿用 R3c 状态并修正已知统计。
> 阻塞级别：**P0 = 不定就写不了代码**；P1 = PoC 期间必须定；P2 = 实现前定即可。

---

## A. 决策缺口 —— R3 裁决结果

| # | 未定义项 | R3 状态 | 落地位置 | 级别 |
|---|---|---|---|---|
| A-1 | §5.3.4 选 A 还是先 B 后 A | ✅ **定为 A**，B 档废弃 | §5.3.-1 / §5.3.4 | — |
| A-2 | DLL 还是静态库 | ✅ **DLL** | §5.3.1 | — |
| A-3 | target 名与符号前缀 | ✅ **`liba2l` 前缀**；主树桥接库 `libxcp_a2lbridge` | §3.2 命名统一表 | — |
| A-4 | POD 内存所有权 | ✅ POD 层取消；抽象接口对象由 DLL 创建并通过 DLL 侧 `Release()` 销毁。CRT 自动校验按 A-10 暂不实施，风险由同构建类型约定承担 | §5.3.3 / §5.3.4b | — |
| A-5 | extern "C" vs C++ 接口 | ✅ **C++ 接口**。**代价**：隔离含义从"标准+STL+ABI 三重"收缩为"标准隔离+上游头不外泄"，已在 §5.3.-1 显式声明 | §5.3.3 | — |
| A-6 | 真实 A2L 样本 | ✅ **改为测试内生成**；查实结论见下 | §7.1-A | — |
| A-7 | Linux/macOS | ✅ **暂不支持，Windows-only** | §5.4 | — |
| A-8 | uchardet shim vs 真装 | 🔁 **P0 仍有效**。更正旧结论：v1.0 与 main 都无条件 `find_package(uchardet CONFIG REQUIRED)` + PUBLIC 链接，本机均没有；该依赖从未因版本选择消失。方案 A（shim）已选，但前置 P1 全量 grep 必做；若核心库实际调用符号则重开方案 B | §5.2 / §5.3.9 | **P0** |

### R3b 第二轮裁决结果（A-9 ~ A-13）

| # | 用户裁决 | 我的落地与必须指出的问题 | 状态 |
|---|---|---|---|
| A-9 | 增加 `A2L_INTERFACE` 宏用于跨平台支持 | ⚠️ **与 A-7/A-10「只考虑 Windows」矛盾**。已按"代码留能力、只验 Windows"化解：宏写成三段式（dllexport/dllimport + visibility + 静态空），但非 Windows 分支属**未测试代码**，不得宣称可用。另注意：该宏加在**我们自写的 `liba2l_api.hpp`/`liba2l_export.h`** 上，不改 thirdparty；且它顺带解决了原 A-9 担心的"上游类无 dllexport"——因为我们只导出薄层，不需要导出上游类（§5.3.8 方式 ①） | ✅ 已定（含矛盾说明） |
| A-10 | 先 pass，不考虑跨平台，只 Windows | 已取消 `liba2lConfig.cmake` 的 CRT FATAL_ERROR 校验，降级为 STATUS 提示 + 人工核对。**后果已登记**：误链配置的崩溃风险由流程承担，非工具承担（§8.4） | ✅ 明确接受风险 |
| A-11 | 允许 `std::function` 回调 | ✅ B-20 已关闭：工作线程回调、快照发布后恰好一次；调用方负责切回 UI；安全取消/join | ✅ 已定 |
| A-12 | 假设使用 Boost 静态库 | **不只是假设，已实测证实**：v1.0 `script/boost.cmake` 显式 `set(Boost_USE_STATIC_LIBS ON)`，本机 `C:\boost\lib` 只有 `.lib`。结论：交付面单 DLL。⚠️ 但 boost::locale 是否内部依赖 ICU 仍未证（D-3） | ✅ 已证实 |
| A-13 | 上游版本策略 | ✅ R3c 最终改判为 **main HEAD + gitlink 固定实际 SHA + 人工升级**；不采用 v1.0，也不使用浮动 branch | ✅ 已定 |

### A-13 版本对比存档（R3b 方案已废弃；R3c 最终选择 main）

> 下表“影响”列记录的是当时拟选 v1.0 的判断，仅作决策轨迹；其中 uchardet 行已按后续证据更正。当前结论以下方 R3c 改判为准。

| 维度 | v1.0 | main | 当时评估 / R3c 更正 |
|---|---|---|---|
| `CMAKE_CXX_STANDARD` | 20 | **23** | R3c 选 main → SDK C++23 / 主树 C++20 分治重新生效 |
| uchardet 依赖 | **有** | **有**（均为 PUBLIC 链接） | 两版都需处理；我此前称 v1.0 无依赖是误判，已更正 |
| `GetXcpPlusDataBlock()` | ❌ 不存在 | ✅ **有** | R3c 选 main → 满足规范 §8.2，§6.3-A 生效 |
| Label(.lab) / ECU Container | ❌ 无 | 有 | 本里程碑范围外（E-8），无损 |
| `a2lfile.h` | 2,497 B | 4,741 B（有 `ReadAndConvertFile()`/`CheckBom()`） | ✅ R3c 选 main → BOM 能力明确，原 P1b 作废 |
| XCP 核心头（`daq.h`/`event.h`/`protocollayer.h`/`xcponudpip.h`） | 与 main 同 sha | 同 | ✅ 两版本一致 |

### ⚠️ R3c 改判：最终选择 **pin main HEAD**，不选 v1.0

用户裁决"直接使用 main HEAD 不直接使用 commit 号"。两点必须记录：

1. **"不写 commit 号"技术上不可实现**。submodule 在父仓库中只能以 gitlink（40 位 SHA）形式存在；
   `.gitmodules` 的 `branch` 字段仅对 `git submodule update --remote` 生效，普通 clone/update 永远 checkout 记录的 SHA。
   → 已按「固定 SHA + 人工升级 + review diff」落地。**这不是拒绝执行，是 git 机制限制。**
2. **我上轮给的 SHA 有误**：当时写的 "main HEAD `1274fa4a…`" 其实是真 HEAD 的**父 commit**。
   本轮 pin 的是 **`c31057498555cbb29ab48e718329ca3163c10221`**（2026-09-24）。

**选 main 的净效果（双向账）**：

| 换来 | 重新背上 |
|---|---|
| ✅ XCPplus 可用 → 满足规范 §8.2，§6.3-A 优先级逻辑成主路径 | ❌ SDK 树须 `CMAKE_CXX_STANDARD 23`，与主树 20 分治（R3b 曾下调，现恢复为必需） |
| ✅ lexer 数字字面量修复（先试整数、失败再试 double）→ COMPU_METHOD 系数与大地址更准 | ❌ uchardet 硬依赖回来了 → §5.2 shim 变 P0，前置 P1 grep 必做 |
| ✅ BOM/UTF-16 API 明确存在 | ❌ MSVC 17 能否编过上游全部 C++23 TU 未知（P3 实测，失败则重启 v1.0 讨论） |
| ✅ Label/EcuContainer（范围外但可用） | ❌ main TU 集合更大，编译时间增加（需实测替换我误引的 "~150"） |

### 因 R3b/R3c 产生的未定义项 —— 处置结果

| # | 新问题 | 状态 |
|---|---|---|
| **A-14** | 将来是否升 main 取 XCPplus | ✅ **已由 R3c 直接实现**（一开始就 pin main），此项关闭 |
| **A-15** | `.gitmodules` 写 branch 还是仅靠 gitlink | ✅ **定为仅 gitlink，不写 branch**。理由：A-5 用 C++ 接口 + A-10 pass 掉 ABI 校验，浮动引用会让 DLL 与头文件不同步且无拦截手段 |
| **A-16** | 上游 SHA 升级规程 | ⏸ 当前最小开发验证集不设计升级自动化；人工升级时重建 SDK。移出当前范围 |
| **A-17** | MSVC 17 无法编译上游 C++23 时的回退路线 | ⏸ 不预先扩展 CMake 方案；仅当 PoC P3 真实失败时重新决策 |

### A-6 的查实结论（回答"Windows 平台是否有生成 A2L 的能力"）

**有，但不能用现成开源库，需自研极简生成器。**

| 候选 | 能否生成 | 证据 | 判定 |
|---|---|---|---|
| pyA2L | ✅ 功能完整 | `pya2l/imex/a2l_exporter.py` 70,610 B，导出 `export_db()` | ❌ **GPL-2.0**（GitHub API `"license":{"key":"gpl-2.0"}`），不引入 MIT 工程 |
| a2llib | ❌ 只读 | 全仓库无 writer | 不可用 |
| 商业工具 | ✅ | CANape/INCA/a2lgen | 不可自动化，排除 |
| **自研 Python 模板生成器** | ✅ 够用 | A2L 纯文本块语法，只需 ~15 类块 | ✅ 采用；本机 Python 3.13.2 可用，且 pypi 同样被 TLS 拦（更印证零依赖路线） |

⚠️ **必须知情的弱点**：循环自证。生成器只会包含我们想到要测的语义。三条缓解见 §7.1-A。

### R3 提出的未定义项 —— R3b 处置结果

| # | 原问题 | R3b 状态 |
|---|---|---|
| **A-9** | 上游类无 `__declspec(dllexport)`，整库编 DLL 可能不可行 | ✅ **已定（用户给方案）**：引入 `A2L_INTERFACE` 宏。且我核对后认为该问题的严重性被高估——只要采用 §5.3.8 方式 ①（薄壳 SHARED），我们只导出自己的类，根本不需要导出上游类。降级为 P1（P4 实测确认） |
| **A-10** | CRT / IDL / `/GL` 一致性由谁保证 | ✅ **用户裁决 pass**，风险显式接受并登记（§8.4）。不再是阻塞项 |
| **A-11** | 是否允许 `std::function` 回调 | ✅ 允许；线程契约仍归 B-20 |
| **A-12** | `liba2l.dll` 是否连带 Boost/ICU DLL | 🔁 **Boost 部分已证实为静态**（v1.0 `boost.cmake` 实证）；**ICU 部分仍未证** → 合并进 D-3 |
| **A-13** | pin 哪个版本 | 🔁 **R3c 改判：pin main HEAD `c3105749…`，固定 SHA + 人工升级**（详见上节双向账） |


## B. 接口语义决策（R4：B-1～B-20 全部关闭）

> 完整决策、失败策略和测试门禁见 `A2L_接口语义_B类决策_R4.md`。以下只保留审计摘要；具体枚举名和字节规则仍需 D 类工具验证，但不再是设计决策缺口。

| # | 已批准决策摘要 | 状态 |
|---|---|---|
| B-1 | ECU_ADDRESS 原值；extension 独立；`byte_offset=index*size` 且须整除 AG；地址递增 `byte_offset/AG` | ✅ 已关闭 |
| B-2 | 0-based API；保留原始下界/extent/stride/主序；执行标量、1D、规则连续2D | ✅ 已关闭 |
| B-3 | 按实际 A2lDataType 穷举固定宽度；禁止 sizeof/ordinal/未知默认 | ✅ 已关闭，枚举全集待 D-6 |
| B-4 | 执行 VALUE、连续 VAL_BLK、标准连续 FNC_VALUES；复杂布局拒绝 | ✅ 已关闭，字段名待源码核实 |
| B-5 | 首里程碑仅启用 STATIC DAQ；DYNAMIC 只保留能力元数据 | ✅ 已关闭 |
| B-6 | 实际 READ_DAQ/WRITE_DAQ 账本为真值；A2L ELEMENT_LIST 仅作候选；无布局只输出 raw | ✅ 已关闭，entry 能力待 D-5 |
| B-7 | 冻结 DtoFrameLayout，先解 envelope 再解 entry；冲突/截断整帧拒绝 | ✅ 已关闭，精确字节规则待验证 |
| B-8 | 支持 IDENTICAL/LINEAR/RAT_FUNC/TAB_INTP/TAB_NOINTP/TAB_VERB；FORM 不执行 | ✅ 已关闭 |
| B-9 | BIT_MASK、DAQ BIT_OFFSET、ERROR_MASK 分离；首版连续 mask，不由 ERROR_MASK 推 valid | ✅ 已关闭 |
| B-10 | PHYS_UNIT 优先，UNIT_REF 回退；保存 SI 指数但不自动换算 | ✅ 已关闭 |
| B-11 | 五型建模；执行 Value/连续 ValBlk/定长 Ascii；Curve/Map 仅元数据 | ✅ 已关闭 |
| B-12 | STRUCTURE/INSTANCE/TYPEDEF 仅元数据，不展开、不执行 | ✅ 已关闭 |
| B-13 | `module::symbol`；Find 大小写敏感且唯一；Search 大小写无关、稳定排序后截断 | ✅ 已关闭 |
| B-14 | PhysicalValue 使用 int64/uint64/double/string/bool variant，接口返回 Result | ✅ 已关闭 |
| B-15 | FromPhysical 默认 Reject；未来 ClampPolicy 必须显式 | ✅ 已关闭 |
| B-16 | 运行时为真值；关键布局冲突 Error，容量/能力差异 Warning，A2L-only Info；t1~t7 不比 runtime | ✅ 已关闭 |
| B-17 | 保留全部 MODULE；单模块自动 active，多模块显式选择 | ✅ 已关闭 |
| B-18 | INCLUDE 相对当前文件；canonical 循环检测；深度32；默认禁止越根，可显式开放 | ✅ 已关闭 |
| B-19 | C++20 Result/Error；LastError 仅展示；异常不跨 DLL | ✅ 已关闭 |
| B-20 | builder 后一次发布 immutable snapshot；并发读；NotReady；one-shot；安全取消/join | ✅ 已关闭 |

## C. 构建 / 工程化未定义

| # | 未定义项 | 现状 | 级别 |
|---|---|---|---|
| C-1 | submodule pin 策略 | ✅ **已关闭**：拉取时采用 main 实际 HEAD，并由父仓库 gitlink 固定；不写浮动 branch。设计时观测 SHA `c31057498555cbb29ab48e718329ca3163c10221` 仅作快照，若落地时 main 已前进则复核差异后记录实际 SHA | **已关闭** |
| C-2 | submodule 与构建输出 | ✅ `.gitmodules` 和 gitlink 入库；SDK/build/generated A2L 输出不入库并加入 ignore | **已关闭** |
| C-3 | SDK 与主树 ABI 兼容 | ✅ **已关闭**：C++ 导出头定义 `kLibA2lAbiVersion`，工厂创建时校验；升级上游 SHA 必须同步重建 SDK 并由 P11 演练。CRT 自动校验按 A-10 暂不实施 | **已关闭** |
| C-4 | Debug/Release CRT 混用防护 | ✅ **由 A-10 裁决关闭**：本里程碑不做自动校验，采用同构建类型人工约定；风险已在 §5.3.4b 登记 | **已关闭（接受风险）** |
| C-5 | SDK 告警级别 | ✅ 最小集不把上游生成代码纳入 `/W4` 或 `/WX` 门禁；自有薄壳保持工程默认告警 | **已关闭** |
| C-6 | SDK 根目录 | ✅ `LIBXCP_LIBA2L_ROOT` 必须显式指定；为空直接 FATAL_ERROR，不设计回退顺序 | **已关闭** |
| C-7 | install 规则 | ⏸ 当前最小开发验证集不做 install/export/package，移出范围 | **范围外** |
| C-8 | CTest 组织 | ✅ 当前只建一个 `A2lSmoke`，不设计完整 label 体系 | **已关闭** |
| C-9 | CI 门禁 | ⏸ 当前最小开发验证集不做 CI | **范围外** |
| C-10 | 构建耗时与缓存 | ⏸ 当前不定 SLA、不做制品缓存 | **范围外** |
| C-11 | DLL 测试部署 | ✅ `POST_BUILD copy_if_different` 到 `$<TARGET_FILE_DIR:A2lSmoke>` | **已关闭** |
| C-12 | A2L 生成器 | ✅ `find_package(Python3 REQUIRED COMPONENTS Interpreter)`；CTest fixture 运行时生成到 build tree，不入库 | **已关闭** |

## D. 待工具验证的事实（不是设计缺口，但我曾据以下结论行动过，须落实）

| # | 待验证事实 | 当前依据强度 | 验证手段 |
|---|---|---|---|
| D-1 | uchardet 未被核心库引用 | **弱**——只读了 `src/a2lfile.cpp` 一个文件 | 源码到位后 grep 全量（P1） |
| D-2 | 上游 ~150 TU 能在 MSVC 17 `/std:c++23` 编过 | **无**——纯推测 | P3 实编 |
| D-3 | boost::locale 本机是否为 Windows-API 后端（无 ICU） | **中**——由 release 库仅 4.7 MB + 无捆绑 icu*.dll 推断，未看其 cmake 依赖声明 | 读 `boost_locale-config.cmake` 实际内容 + 冒烟转换 UTF-16 串 |
| D-4 | `Module::GetXcpDataBlock()` 是否真的解析 IF_DATA（README 说是，函数可能返回 nullptr） | **中**——README 示例代码如此描述 | T4 用例实测 |
| D-5 | STATIC DAQ 的 ODT entry 级地址是否被上游解析出来（B-6 的前提） | **无**——未读 `daqlist.h`/`event.h` 全文 | 源码到位后读，或直接写探针用例 |
| D-6 | `A2lDataType` 枚举成员全集（B-3 的前提） | **无**——未读 `a2lenums.h` | 同上 |
| D-7 | 上游对 `IF_DATA XCP` 的 `BYTE_ORDER`/`ADDRESS_GRANULARITY` 是否落到 `ProtocolLayer`（我已看到 setter/getter，但未验证实际解析路径） | **中** | T4 用例 |
| D-8 | 我对 `cmake/a2lConfig.cmake.in` 缺陷的判断 | **强**——原文已逐行读到 | 无需再验 |
| D-9 | 我对 XCPplus 的更正 | **强**——`a2lstructs.h` 原文已读到方法签名 | 但"方法内部是否真填充 `xcp_plus_data_block_`"仍需读 `a2lhelper.cpp`/`ifdatablock.cpp` 确认 |

## E. 范围外但应显式记账（避免日后被当成遗漏）

| # | 事项 | 处置 |
|---|---|---|
| E-1 | A2L 解压/解密外部函数（GET_ID Type=4，规范 §11） | 另立里程碑，本设计不含 |
| E-2 | Seed&Key / Checksum 外部 DLL 的平台化加载（Win/Linux/macOS 三套） | 另立里程碑；现保持 `xcp_master.hpp` 注入式接口不变 |
| E-3 | PROGRAMMING / MEMORY_SEGMENT / PAG / PGM 刷写流程 | 能力具备，范围排除 |
| E-4 | ECU_STATE 与资源锁定策略 | 范围排除 |
| E-5 | TIME_CORRELATION 精度校准 | 范围排除（只取元数据） |
| E-6 | TRANSFORMER（ASAP2 二进制交换格式） | 上游有 `transformer.h`，未评估，暂不纳入 |
| E-7 | A2ML 段解析（上游有独立 AML parser） | 明确不需要（README 亦称普通用户可跳过） |
| E-8 | Label(.lab) / ECU Container 文件 | 上游支持，本里程碑不碰 |
| E-9 | CCP 分支（`src/ccp/*`） | 只需 XCP；SDK 构建时是否能把 ccp 从编译列表剔除（不改 thirdparty 前提下不可剔，接受多余产物） |

---

## 汇总（R4：B 类全部定稿）

| 类别 | P0 | P1 | P2 | 未决合计 | R4 变化 |
|---|---:|---:|---:|---:|---|
| A 决策缺口 | 1 | 0 | 0 | 1 | 仅 A-8：shim 前提需源码 grep；A-16/A-17 移出当前范围 |
| **B 接口语义** | **0** | **0** | **0** | **0（20 项已定）** | B-1～B-20 已逐项获得用户批准，基线见 `A2L_接口语义_B类决策_R4.md` |
| **C 构建工程化** | **0** | **0** | **0** | **0（当前范围内全部定稿）** | install/export/CI/cache/SLA 已移出当前范围 |
| **未决合计** | **1** | **0** | **0** | **1** | 仅 A-8；D 类事实验证不计入设计未决 |

> D 类仍需实测，尤其是 B-3 所依赖的枚举全集、B-4 的实际 RECORD_LAYOUT 字段、
> B-6 的 entry 可见性和 B-7 的 DTO envelope 精确规则；这些是实现证据，不再是接口决策。

### 事实项变动（R3c）

| # | 事实 | 状态 |
|---|---|---|
| D-1 | uchardet 是否被核心库引用 | 🔴 **重开为 P0**。两版都 REQUIRED，本机无。P1 grep 决定 shim 是否可用；不成立则链接失败 |
| D-2 | 上游能否在 MSVC 17 编过 | 🔴 **重开**。现在是"C++23 + main 的完整 TU 集合（含 label/logstream）"，比 R3b 的版本更难 |
| D-11 | v1.0 是否有 BOM 转换 | ✅ **作废**：改 pin main，`ReadAndConvertFile()/CheckBom()` 已逐行读到，能力明确 |
| D-12 | main 的实际 TU 数量 | ❓ 待 P3 记录（我此前引用的 "~150" 不可靠，需以实编为准） |

### R4 后仍阻塞实现的关键问题

1. ✅ **B-1～B-20 已全部关闭**；不再存在接口语义 P0。完整基线见 `A2L_接口语义_B类决策_R4.md`。
2. **A-8 / D-1（P0）** —— uchardet 是否被核心库实际调用。源码到位后必须先全量 grep；若 shim 前提不成立，需改用真实 uchardet。
3. ✅ **C-11 已关闭**：`POST_BUILD copy_if_different` 把 DLL 放到 `A2lSmoke` 目录。
4. **D-2/D-5/D-6/D-7/D-9/D-12（事实验证）** —— C++23 可编性、STATIC DAQ entry、类型枚举、IF_DATA 字段、XCPplus 填充、实际 TU 数；它们决定实现细节，但不再改变已批准的语义原则。

### R4 评审结论与专业意见

1. **"不写 commit 号"技术上无法实现**：submodule 必须由父仓库 gitlink 固定 SHA；采用 pin main 当前 SHA + 显式升级流程，可兼顾跟随 main 和构建可复现性。
2. **选 main 是合理取向，但有构建代价**：XCPplus 满足规范 §8.2，lexer 修复也直接有益；C++23、uchardet 和更大的 TU 集合需由 PoC 验证，其中 uchardet 是第一检查点。
3. **B 类已经收敛**：20 项语义全部批准。下一步不再讨论接口取向，而是拉取 submodule，按 P1→P2→P3 验证 uchardet、依赖和 C++23 可编性，再把 D-5/D-6/D-7/D-9 的事实结果回填设计。
4. **整体设计仍未获得最终批准**：依照 AGENTS.md，PoC 和实现代码必须等用户明确批准设计后才能开始。

