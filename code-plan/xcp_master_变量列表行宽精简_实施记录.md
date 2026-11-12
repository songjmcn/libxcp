# xcp_master 变量列表行宽精简

- 日期：2026-11-11
- 触发问题：变量预览里一行显示不全，右侧被终端宽度裁掉
- 用户决策（m02861）：A 移除拒绝原因文本、B 类别两字缩写、C 基础类型缩写；名称只显示变量名；保持单行布局

## 1 精简前的一行都由什么组成

改造前 `FormatPreviewLabel()`（`tools/xcp_master/main.cpp`）拼接的是一行
**8 段**文本，宽屏（`compact=false`，宽度 ≥ 100 列）下顺序为：

```
module::name | 类别 · 基础类型 · 形态 | 值 单位（旧值）（已停止） | 时间戳 | 已加入采样 | 不适用周期采集: <30~60 字原因> | 可标定（可标定表单读写）
```

逐段拆解，以实测 A2L（198554 个符号）里的典型行为例：

| 段 | 示例 | 典型宽度 | 功能必需性 |
| --- | --- | --- | --- |
| 全限定名 | `apa_complex::apa_fusion_cal.Fus_External_Calibration.PSD_controlled_offsetBaseOnVPS` | **80～95** | 模块名每行重复，但模块在整份 A2L 里只有一个 |
| 类别 | `CHARACTERISTIC/VALUE` | 20 | 是（决定能不能 F4），但可用短名 |
| 基础类型 | `FLOAT32_IEEE` | 12 | 是，可缩成 `F32` |
| 形态 | `标量` / `一维数组[128]` | 2～10 | 标量占位无信息量 |
| 值 + 单位 | `12.5 V` | 5～10 | **功能必需** |
| 旧值/已停止 | `（旧值）` | 6 | 是（决定可信度） |
| 时间戳 | `2026-11-11 17:56:05.123` | **24** | 非必需 |
| 采样状态 | `已加入采样` / `未采样` | 5～8 | 是（决定按不按 F6） |
| 观测拒绝原因 | `不适用周期采集: DAQ 仅支持 MEASUREMENT` | **35～60** | 否（F3 详情里有） |
| 标定资格 | `可标定（可标定表单读写）` | 13 | 是（决定按不按 F4），可用 1 字标记 |
| 写入拒绝原因 | `写入拒绝: 仅支持 CHARACTERISTIC/VALUE 写入` | **40** | 否（F3 详情里有） |

合计最坏情况 **230+ 列**，而用户终端约 145 列，右侧直接被裁掉。

## 2 精简后的格式

```
name | value unit | 采标 | 类型
```

实例：

```
PSD_controlled_offsetBaseOnVPS | 12.5 V | 采标 | 特性/VALUE F32
SigVolt | — | | 测量 U16
MapTable | [64 elements] | 采 | 特性/MAP F32 MAP[4×8]
```

字段变化：

| 段 | 处理 | 理由 |
| --- | --- | --- |
| `module::` 前缀 | **删除**，模块名移到状态行 `MODULE <name>` | 模块名在整份 A2L 里唯一，逐行重复 12 列纯属浪费；状态行本来就在显示 A2L 文件名，加一格即可 |
| 类别 | `CHARACTERISTIC/VALUE` → `特性/VALUE` | 中文字符按终端宽 2 列计，仍比英文短一半；未知类别原样透出，新增类型不会被静默吞成固定文案 |
| 基础类型 | `FLOAT32_IEEE` → `F32`、`UBYTE` → `U8` | 一对一映射，不做宽度推断 |
| 形态 | 标量**省略** | 无后缀即标量，这是最常见情况 |
| 值/单位/旧值标记 | 保留 | 功能必需 |
| 时间戳 | 取消行内显示 | F3 详情里仍有 |
| 采样状态 | `已加入采样`/`未采样` → `采`/空 | 2 字定点，便于扫列 |
| 观测拒绝原因 | 删除 | 35～60 字长文本，几乎每行都撑爆；F3 与 `read` 命令仍给完整原因 |
| 标定资格 | `可标定（可标定表单读写）` → `标` | 准入判定逻辑未变，只是显示 |
| 写入拒绝原因 | 删除 | 同上，F3 详情里有 |
| 分隔符 | ` | `（单空格） | 原为 ` | ` + ` · ` 混用，统一后每行省 6～10 列 |

最坏情况降到 **约 125 列**，用户终端 145 列可以完整显示。

### 为什么类型放在行尾

终端窄的时候会被右侧裁切。把类型放到行尾，被裁掉的是最不关键的信息
（类型可回 F3 看），而名称、数值、状态标记都在左侧安全区。

### 为什么保留「标」而不保留拒绝原因

`SymbolWriteUnsupportedReason()` 的结果决定 F4 能否进入写入流程——这是
**功能必需**信息，但只需要一个布尔结论；而它的文字版本是 40 字符的解释，
属于「为什么不行」，可以推迟到用户真正想按 F4 时再看。两者的信息量不同级，
不能一起保留也不能一起删。

## 3 改动文件

| 文件 | 改动 |
| --- | --- |
| `tools/xcp_master/symbol_display.hpp` | `CompactKindName` → `ShortKindName`（两字中文）；新增 `ShortDataTypeName`（一对一缩写表）；`FormatSymbolTypeLabel` 改单空格分隔、标量省略、窄屏去「数组」修饰 |
| `tools/xcp_master/main.cpp` | `FormatPreviewLabel()` 重写；状态行加 `MODULE`，新增成员 `preview_module_name_`；失效候选行补「Del 清理」提示 |
| `tests/xcp_master_symbol_display_test.cpp` | 5 条既有断言更新到新格式；新增 3 个用例 |
| `tools/xcp_master/README.md` | Variables 段行格式说明同步 |

## 4 测试

### 4.1 新增／更新用例

| 用例 | 钉住的行为 |
| --- | --- |
| `ShortDataTypeNamesAreOneToOne`（新） | 14 个基础类型全部有短名，`Unknown` → `?`，不回落成长名 |
| `ShortKindNamesCoverAllKinds`（新） | 6 种 CHARACTERISTIC 形态 + MEASUREMENT + STRUCTURE；未知类别原样返回 |
| `ScalarShapeIsOmitted`（新） | 标量行无形态后缀，但 `DescribeSymbolType().data_shape` 与 `AsamDataTypeName()` 仍保留完整值（详情弹窗依赖它们） |
| `DescribesOneDimensionalArray`（改） | 宽屏 `测量 U32 一维数组[128]`；窄屏 `测量 U32 [128]` |
| `DescribesMapDimensions`（改） | `特性/MAP F32 MAP[4×8]` |
| `DescribesFixedAxisCurve`（改） | `特性/CURVE F64 曲线[4点]` |
| `DoesNotInferUnknownTypeOrDimensions`（改） | 窄屏 `测量 U32 维度未解析` |
| `MapsAsamDataTypesToNames`（不变） | 长名映射保留——详情弹窗仍用它 |

结果：`XcpMasterSymbolDisplayTest.*` → **11/11 通过**。

### 4.2 回归

`ctest -R "XcpMaster|A2l"` → **191/191 通过**（75.44 s，2 项既有 Skipped）。

## 5 已知限制

1. **模块名丢失的单模块前提**：状态行显示的是候选集里**首个**模块名。多模块
   A2L 里不同符号属于不同模块时，这一格会有误导。当前用 `--module` 指定时
   状态行显示的是显式指定的那个。真正的解法是行内保留模块名或加 `--module`
   必填校验，属于设计取舍，未在本轮展开。
2. **窄屏仍可能超宽**：最坏情况约 125 列，80 列终端下名称长的行仍会被裁。
   若需要，下一步可加按终端宽度做名称中段省略（`head…tail`）。
3. **时间戳彻底不进行**：当前由 F3 详情提供。如果希望「值多久没更新」一眼可见，
   可以把 `sample->stale` 扩展成 `（0.8s 前）` 这类短形式，但会增加宽度。
4. 标定视图（`calibration_view.hpp`）的行格式**未改**，仍是
   `名称 | 类型 · 基础类型 · 形态 | 值`。它只有已登记清单（通常十几行），
   宽度压力小；若也需要精简，需要同步改 `DefaultCalibrationViewFormatter`。

## 6 复现命令

```bash
# 构建
cmake --build cmake-build-release --target xcp_master XcpMasterSymbolPolicyTest -j 4

# 单元测试
./cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe \
    --gtest_filter="XcpMasterSymbolDisplayTest.*"

# 回归（Windows：先把 TMPDIR/TEMP/TMP 指向可写目录，否则既有用例误报）
ctest -R "XcpMaster|A2l"
```

## 7 人工验收

1. `xcp_master.exe -c apa_complex.xcp.json`
2. 变量预览里确认每行满足：**名称在最左、值在名称后、`采`/`标` 标记、类型在行尾**
3. 任选一行按 F3，确认详情弹窗仍能看到完整类别名、`FLOAT32_IEEE`、
   `一维数组[N]` 与**完整的拒绝原因文本**
4. 用 F6 加入采样，确认行内 `采` 标记出现且位置不跳动（宽度固定 2 列）
5. 把终端宽度调到 80 列，确认被裁掉的是行尾类型，而不是名称与数值