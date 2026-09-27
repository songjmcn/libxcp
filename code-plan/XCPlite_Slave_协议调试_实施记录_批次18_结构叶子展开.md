# XCPlite Slave 协议调试 实施记录 批次18 —— 结构成员叶子路径（STRUCTLEAF，接管批次16）

> 日期：2026-07-29。
> 目标来源：`XCPlite_Slave_协议调试_后续路线_批次18-21_计划.md` §1（批次18 全量接管批次16）。
> 语料门：批次16 的 16-0001"真实脱敏 A2L"硬门由批次17 Slave 运行时 A2L 解除，本批入库为固定语料。
> 设计回写：R4 §9.1 新增 **R4.9** 行、§6.3 B-12 行标记 leaf 展开器落地、§7.2 验证表新增 T5.5 行。

## 0. 交付概览

| 项 | 结果 |
|---|---|
| 叶子路径闭环 | `MODULE::INSTANCE.member` / `.arr[idx]` / `.nested.member` / `.nested_arr[idx].member` 四形态 + 结构数组实例逐元素 `inst[i].path` + CalSeg 标量成员：Find/寻址/尺寸/换算/读写全通 |
| SDK ABI | v5 → **v6**：`TypedefMeasurementDto` + `IDoc::ListTypedefMeasurements` + `StructInfoDto.matrix_dim`（INSTANCE 级矩阵维透传） |
| 真实语料告警 | 18-A′ 前恰 2 条 InvalidLayout（`kDefaultCalParams.cal_factor/cal_offset` 不可解）→ 18-A′ 后 **0 条**（42 符号基线） |
| 黄金用例 | +2：`StructLeafRealCorpusFourForms`、`StructLeafBrokenCorpusWarnsEveryRejection`；A2lGolden 39 = 38 PASS + 1 已知 SKIP（UTF-16，§6.3 缺口） |
| E2E | `XcpliteA2lReadTest.StructMemberLeafPaths` SKIP→PASS（叶子↔基址+offset 交叉核证）；`XcpliteWriteTest.LeafMemberWriteMatchesBaseOffsetView` 新增；`ctest -R Xcplite` **25/25** |
| 门禁 | ON ctest **377 全绿**（唯一不计入 = Ag1_Cto8 设计性 skip）；OFF ctest **352/352**；Release+Debug prepared root 同批重建；clang-format 22.1.8 0 违规；本工具链写入损坏 token 扫描干净 |
| B-12 边界 | **不变**：整块 STRUCTURE/INSTANCE 的 ByteSizeOf/ToPhysical/FromPhysical 继续 `UnsupportedOperation/Query`，叶子不进基址反查表 |

## 1. 本批新增对手端/上游事实核证表（F13–F17，写码前实测）

| # | 事实 | 证据 | 影响 |
|---|---|---|---|
| F13 | CalSeg 校准成员在 A2L 中只以 `TYPEDEF_CHARACTERISTIC`（`C_cal_factor`/`C_cal_offset`）引用，XCPlite 无 TYPEDEF_MEASUREMENT 写法 | 运行时语料 corpus:14-32；探针 2 告警 subject | 逼出 18-A′（标量事实注册表并入 TYPEDEF_CHARACTERISTIC）；否则真实对手端 cal 叶子永不可解，而 CalSeg 是 Phase2 校准写核心通路 |
| F14 | TYPEDEF_CHARACTERISTIC 元素类型可由 `Deposit` RECORD_LAYOUT 的 `FNC_VALUES`（Position 0/1 + DIRECT + 标量）证明，与批次10 CHARACTERISTIC 符号判定同一口径 | `thirdparty/a2l-sdk/src/liba2l_export.cpp` 复用 `ClassifyRecordLayout`/`BuildConversion` | `cal_factor`=UWORD(2B) limits 0..65535；`cal_offset`=U32(4B) unit=mm limits ±1000，换算/单位/边界全量透传实测成立（探针输出钉值） |
| F15 | XCPlite 仅对 `XcpAddrIsAbs \|\| IsApp \|\| IsDyn` 寻址的 MEASUREMENT/INSTANCE 打印 `READ_WRITE`；CalSeg 走分段相对寻址（ext=0）→ A2L 缺省**只读** | `thirdparty/XCPlite/src/a2l.c:1092-1116`（A2lCreateInstance_）、`:1156-1169`（A2lCreateMeasurement_） | cal 叶子 `read_write=0`；`FromPhysical` 被只读门拒（`compu_method_eval.cpp:571-573`，先于限值检查）。**决策：不伪造 rw 位**，golden 如实钉实测语义（UnsupportedOperation/Conversion+"只读"）；物理可写性以批次17 裸地址 CalSeg 写通路为既有证据 |
| F16 | 经 `A2lTypedefMeasurementArrayComponent` 注册的**结构体内标量数组成员**（本批新增 `g_outer.outer_arr[4]`）产出成员级 MATRIX_DIM；叶子 dims=extent 4/stride 1 | Slave 增补后重捕获语料 + 探针 dims 打印 | 补齐第四形态"标量数组成员"；E2E 与 golden 双覆盖 ByteSizeOf==4/ComputeElementAddress/ToPhysical |
| F17 | 结构数组 INSTANCE 的 `MATRIX_DIM n m` 是逐元素展开 `inst[i].path` 的唯一依据；TYPEDEF_STRUCTURE 上恒空 | 上游 `a2lstructs.h` `A2lObject::MatrixDim` + 探针（`g_struct_array[2].simple_u32`@0x19074 命中） | SDK v6 `StructInfoDto.matrix_dim` 仅对 is_instance 透传；resolver 按维度乘积（checked mul，≤1024 元素）展开 |

## 2. 新增文件

| 文件 | 内容 |
|---|---|
| `thirdparty/a2l-sdk/a2lbridge/src/structure_layout.hpp/.cpp` | `detail::BuildStructureLeaves(typedef_structs, instances, typedef_measurements)` → `{leaves: vector<SymbolInfo>, warnings: vector<LoadWarning>}`；注册表键 `module\x1Fname`；递归展开（深度≤32、每实例预算 4096 叶子、结构数组≤1024 元素、环检测用链向量、成员界内检查 offset+size≤父 SIZE、ProductDims 防乘溢出）；标量成员经 TYPEDEF_MEASUREMENT/CHARACTERISTIC 事实成叶（kind=Measurement、xcp_address=基址+Σ偏移、extension 透传、dims 压单维 stride=宽度、换算/单位/limits 复制自类型事实、read_write=实例位）；拒绝一律 `LoadWarning(InvalidLayout, phase=Load, subject=module::path)`，不猜测（B-3） |
| `tests/data/a2l/xcplite_structleaf_corpus.a2l` | 批次16 16-0001 真实语料：Slave 运行时生成的 `xcp_test_slave.a2l` 入库（批次17 夹具产物，符号为纯测试名，天然脱敏）；含四形态+结构数组实例+CalSeg 实例；地址值为某次运行快照，**只用于结构判定**，E2E 每次现场生成不依赖本文件数值 |
| `tests/data/a2l/XCP_104.aml` | 语料 include 依赖的空壳 stub（C 注释），使离线加载通过 SDK include 预扫描 |
| `tests/data/a2l/xcplite_leaf_broken.a2l` | 手写负例语料（MODULE `LEAF_FIX`，无 IF_DATA→测试用 `require_if_data_xcp=false`）：恰好 6 种拒绝形态——元素数超界(big_dim 100000)/递归环(Outer→LoopA→LoopB→LoopA)/引用不存在的 TYPEDEF(h_ref→NoSuchType)/成员缺类型(ok_ctrl.ghost)/成员越出父尺寸(ok_ctrl.wide 0x2+0x8>0x4、oob.big)/MATRIX_DIM 含 0(zero_dim)；另含可证存活叶子与双路径同址别名（`cyc.cyc.v1` 与 `cyc.cyc.inner.v2` 同指 0x6002，验证"不猜归属"时合法别名都保留） |

## 3. 修改的既有代码

### 3.1 SDK 导出层（`thirdparty/a2l-sdk`，自维护子工程，ABI v5→v6）

- `include/liba2l/liba2l_api.hpp`：`kLibA2lAbiVersion = 6u`；新增 `TypedefMeasurementDto`（name/description/module_name/data_type/element_size_bytes/compu_method_name/内嵌 `ConversionDto conversion` 快照/phys_unit/byte_order/matrix_dim/resolution/accuracy/have_limit+lower+upper/bit_mask/address_type/layout/read_write）；`IDoc::ListTypedefMeasurements(std::vector<TypedefMeasurementDto>*) const noexcept`（纯虚，v6 接口面）；`StructInfoDto.matrix_dim`。
- `src/liba2l_export.cpp`：`BuildTypedefMeasurementDto`（TYPEDEF_MEASUREMENT 直读类型/宽度经 `MapDataType`+`TypeWidthBytes`，换算经 `BuildConversion`）；**18-A′** `BuildTypedefCharacteristicDto`→同一 DTO 形态：`c.Type()==VALUE` 且 Deposit 版式可证才填 data_type/宽度，否则 kUnknown/0 交 resolver 按 B-3 拒绝；typedef 级 `read_write=true`（真实读写门在实例层）；采集循环保留测量先入序、特性并入其后，容器 `std::sort`→`std::stable_sort`（同名冲突 first-wins 且确定）；`ResetState`/成员/查询实现同批。
- 动机链：真实语料恰 2 条 InvalidLayout 探针 → 双注册表合并方案（resolver/桥接查询层零改动）。

### 3.2 桥接层（`a2lbridge`）

- `src/a2l_bridge.cpp` `PublishSnapshot`：`ListTypedefMeasurements` → 结构定义/实例按 `is_instance` 分流 → `detail::BuildStructureLeaves` → leaves/warnings 传入 `MakeDatabase`。
- `src/a2l_database_impl.{hpp,cpp}`：构造函数增 `leaf_symbols`/`leaf_warnings`；叶子注册进 `m_symbols_` 时同名冲突**跳过并告警**（B-13 不猜）；排序后重建索引并记录 `std::unordered_set<std::size_t> m_leaf_positions_`；`BuildAddressIndex` 跳过叶子位置与 Structure 本体 → 基址反查永不含叶子/整块（B-12 不变）；`src/a2l_dto_map.hpp` `MakeDatabase` 签名扩展；`CMakeLists.txt` 增 `src/structure_layout.cpp`。
- 查询语义复用既有实现（`Find`/`ByteSizeOf`/`ComputeElementAddress`/`ToPhysical`/`FromPhysical` 均经 `GetSymbol`），叶子即普通 kind=Measurement 符号——限定路径 `MODULE::INSTANCE.member[索引]` 直接命中，无新接口。

### 3.3 测试 Slave（`tests/xcp_test_slave/`）

- `xcplite_test_types.hpp`：`OuterStruct_t` 增 `outer_arr[4]`（期望值 {11,22,33,44}，`kExpectOuterArr`）。
- `main.cpp`：`g_outer` 初始化补齐；`A2lTypedefMeasurementArrayComponent(outer_arr, ...)` 注册标量数组成员（F16 形态来源）。

### 3.4 测试用例

- `tests/a2l_golden_test.cpp`：T5.5 两组正式用例（17 叶子路径/地址/宽度/rw 钉值表 + B-1 交叉核证 `g_outer`@0x19040 + ByteSizeOf 本体拒 vs `outer_arr`==4 + cal limits/unit 元数据 + F15 只读门断言；负例语料 7 告警恰形、15 符号、8 条坏路径 Find→NotFound、zero_dim 实例元数据保留）。迭代过程用临时探针 `P18ProbeCorpus`/`P18ProbeBroken` 取实测基线（b9 记录），钉值全部并入正式用例后探针已删除。
- `tests/xcplite_a2l_read_test.cpp`：`StructMemberLeafPaths` 从 SKIP 桩改为全断言（逐元素/嵌套/数组成员路径 Find、叶子地址==实例地址+offsetof、ext==0x01、Master 实读值匹配）。
- `tests/xcplite_write_test.cpp`：新增 `LeafMemberWriteMatchesBaseOffsetView`（经叶子 FromPhysical→写→读回，与基址视图交叉一致，邻居不受扰）。

## 4. 关键实现要点与语义钉值

1. **地址公式**（B-1）：叶子 `xcp_address = INSTANCE 基址 + Σ ADDRESS_OFFSET（+ 索引×元素宽度）`；MATRIX_DIM 只作元素计数/步进，绝不乘入基址；E2E 用协议层"基址+offset"读作交叉基准，两条通路必须一致（实测一致）。
2. **extension 透传**：绝对寻址测量叶子 ext=0x01（F1），CalSeg 叶子 ext=0x00（分段寻址）——两种都按 A2L 原样，不合并语义。
3. **换算链复用**：叶子 `ConversionInfo`/limits/unit 全部来自类型事实（LINEAR 经 `tm.a/tm.b`、IDENTICAL、NO_COMPU_METHOD→Kind::None）；`FromPhysical` 门序实测为 **rw→（类型）→限值**，越限 2000.0 在 cal_offset 上因 F15 只读门先拒，故 golden 断 UnsupportedOperation+message 含"只读"（限值元数据断言仍保留——事实链完整）。
4. **冲突纪律**：同名（同模块）测量/特性/结构/实例/叶子任何碰撞 → 保留先注册者 + `LoadWarning`，绝不静默覆盖；递归展开的预算超限同样告警化（防放大攻击语料）。
5. **结构数组实例**：`g_struct_array`（matrix_dim 3×1）展开为 `g_struct_array[0..2].member`；`SimpleStruct_t` 宽 8 → [2].simple_u32@0x19074 实测命中。
6. **B-12 不变式**：本批只新增"叶子可寻址"，未放开任何整块操作；`StructureMetadataOnly` 用例保持绿。

## 5. 门禁执行记录（Windows/MSVC/CMake 4.0，串行构建，禁 -j）

| 门 | 命令/动作 | 结果 |
|---|---|---|
| SDK Release prepared | `pwsh -File thirdparty/a2l-sdk/build-sdk.ps1 -BoostRoot C:\boost\lib -Config Release -OutRoot build/liba2l-prepared`（job pwsh-26） | exit 0 |
| SDK Debug prepared | 同上 `-Config Debug`（job pwsh-36） | exit 0，`msvc-x64-debug` 三产物齐 |
| 黄金套件 | `ctest -R A2lGolden`（cmake-build-xcplite，LIBXCP_BUILD_A2L=ON） | 39 = 38 PASS + 1 SKIP（Utf16LeBomExploratory，已知缺口） |
| 对手端 E2E | `ctest -R Xcplite` | 25/25 PASS，0 SKIP（协议 10 + A2L 读 6 + 写 9） |
| ON 全量 | cmake-build-xcplite 全量 build + ctest | **377 全绿**（Ag1_Cto8 设计性 skip 不计） |
| OFF 基线 | cmake-build-release 全量 build + ctest | **352/352**（A2L 关闭面零回退） |
| 格式 | clang-format 22.1.8 `-i --style=file`（13 个新增/修改文件） | 0 违规（格式化后两侧全量重验通过） |
| 工具损坏扫描 | grep `xe2l|e2lite|E2l|\.e2l` | 干净 |

## 6. 与批次16 计划的映射与偏差

| 批次16 任务 | 落点 | 状态 |
|---|---|---|
| 16-0001 真实语料门 | §2 corpus（批次17 运行时 A2L 解锁） | ✅ 解除并入库 |
| 16-0002/0003/0004 前置核证/基线/失败先行 | F13–F17 + 探针红灯实录（18-A′ 前恰 2 告警即"实现前必须失败"样本）+ StructMemberLeafPaths SKIP 桩 | ✅ |
| 16-A SDK 事实导出（v6） | §3.1 | ✅ |
| 16-B 解析器 | §2 structure_layout | ✅ |
| 16-C 查询/读写语义 | §3.2 + §4（复用既有执行器，无新公开接口） | ✅ |
| 16-D fixture/测试/门禁 | §3.4/§5 | ✅（除 16-D08） |
| 16-D08 DAQ 叶子进 ODT | — | **有意延后**：按批次16 §1.2"不自动改变"，随批次20 DAQ 实时一并验收 |
| 16-D01 tests/a2l_gen 扩展 | — | **偏差**：合成形态改由真实 Slave 四形态语料 + 手写负例 fixture 覆盖（覆盖等价，生成器不动） |
| 16 §6 DoD-7 实施记录 | 本文件（批次18 记录代批次16 记录） | ✅ |

## 7. 遗留与后续

| 项 | 去向 |
|---|---|
| UTF-16 语料仍 §6.3 已知缺口 | 不变（上游限制） |
| `a2l_golden_test.cpp:1602` C4100 'res' 未引用告警 | 批次13 历史遗留，非本批引入，不处理 |
| CalSeg 叶子在 A2L 元数据层只读（F15） | 批次19 Seed&Key 用 Slave 实测校准写仍走裸地址/段地址通路；若需"A2L 名义可写"的解锁路径，届时以 override 配置构造 abs 寻址实例取证，不改 thirdparty |
| 语料文件中的地址为单次运行快照 | E2E 全部现场生成，golden 只锁结构/换算事实；已知且接受 |

## 8. 复现步骤（相对路径）

```powershell
# SDK 双侧 prepared（18-E）
pwsh -File thirdparty/a2l-sdk/build-sdk.ps1 -BoostRoot C:\boost\lib -Config Release -OutRoot build/liba2l-prepared
pwsh -File thirdparty/a2l-sdk/build-sdk.ps1 -BoostRoot C:\boost\lib -Config Debug   -OutRoot build/liba2l-prepared
# ON 侧（需已配置 LIBXCP_BUILD_A2L + LIBXCP_BUILD_XCPLITE_SLAVE 的 cmake-build-xcplite）
cmake --build cmake-build-xcplite --config Release
ctest --test-dir cmake-build-xcplite -C Release            # 377 绿 + 1 设计性 skip
ctest --test-dir cmake-build-xcplite -C Release -R "StructLeaf|Xcplite"   # 本批增量面
# OFF 基线
cmake --build cmake-build-release --config Release
ctest --test-dir cmake-build-release -C Release            # 352/352
```

## 9. 变更记录

| 轮次 | 内容 |
|---|---|
| B18-1 | 批次18 实施完成：SDK v6（TYPEDEF 标量事实 + 18-A′ TYPEDEF_CHARACTERISTIC 并入，探针驱动）、桥接 `StructureLayoutResolver` 四形态+结构数组展开、真实语料/负例 fixture、golden T5.5 两组 + E2E SKIP→PASS、ON 377/OFF 352 双门禁、Release+Debug prepared、R4 回写 R4.9。批次16 全量接管闭合（DAQ 兼容项按 §6 延后至批次20）。 |
