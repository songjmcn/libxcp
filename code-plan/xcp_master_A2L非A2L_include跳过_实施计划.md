# xcp_master：生成净化版 A2L（跳过非 A2L include）实施计划

> 第二版。第一版采用「加载前临时副本」方案，复核时发现该方案未处理 include 的相对路径
> 基准问题，已按用户意见改为「生成持久化净化版 A2L 并写入 config 目录」。
> **本计划不修改 `thirdparty/a2llib`、`thirdparty/a2l-sdk` 的任何文件。**

## 1. 问题与背景

部分 A2L 在 `MODULE` 内声明：

```text
/begin A2ML
  ...
/end A2ML
include "XCP.aml"
```

当 `XCP.aml` 不存在时，`xcp_master` 启动即失败。

### 1.1 根因（已核实）

上游 a2llib 的 `/include` 是**词法层无条件处理**，且**存在性检查先于扩展名判断**：

| 位置 | 行为 |
| --- | --- |
| `thirdparty/a2llib/src/a2lflexer.l:295` | `"/include" { FixIncludeFile(); }` |
| `thirdparty/a2llib/src/a2lscanner.cpp:340` / `:353` | 先 `exists()`，不存在立即 throw |
| `thirdparty/a2llib/src/a2lscanner.cpp:375` | **之后**才判断是否为 `.a2l` |
| `thirdparty/a2llib/src/a2lfile.cpp` `ParseFile()` | catch 异常，记 `last_error_`，返回 false |

缺失时根本走不到扩展名分支，库内**没有跳过开关**。`A2lParserType` 仅有两值
（`a2lfile.h:35-38`），其中的 `PARSE_MODULE_INFORMATION_ONLY` 会让 `MOD_PAR`
规则 `YYACCEPT`（`a2lparser.y:1001`）并丢掉全部 `MEASUREMENT`/`CHARACTERISTIC`，不可用。

### 1.2 「AML 对本工具无用」已核实

- `a2lparser.y:1460`：`a2ml : A2ML { $$ = std::move($1); };`
- `a2lparser.y:1043`：`module_attribute : a2ml { scanner.CurrentModule().A2ml($1); }`
- `Module::A2ml()`（`module.h:53`）仅保存字符串
- `AmlSection`/`AmlParser`（`amlsection.cpp`）在 `thirdparty/a2llib/src` 中**无任何调用者**

a2llib 从不用 AML 定义解释 `RECORD_LAYOUT`。

### 1.3 既有相关缺陷（本次方案需绕开）

**相对 include 的解析基准是"当前文件所在目录"**（`a2lscanner.cpp:351`
`input_file.replace_filename(fullname)`）。而受控快照把 A2L 复制到
`<exe目录>/tmp/a2l-snapshot-<16位随机hex>.a2l`（`src/trusted_a2l_snapshot.cpp:42`），
文件名与目录都变了。因此在 **TCP 模式**下：

- `include "XCP.aml"` 会去 `<exe目录>/tmp/` 找，而不是用户 A2L 旁边；
- 真正的相对 `.a2l` include 同样会失效。

**本方案生成的净化版 A2L 必须自洽地解决基准问题**，不能沿用"裸复制"。

## 2. 目标

1. A2L 中指向非 `.a2l` 文件的 `include` 不再导致解析失败。
2. 净化版 A2L 落到 **config 目录**持久保存，可复现、可审查。
3. 加载时加载净化版；`save` 时 `a2l_path` 指向净化版。
4. 真正的 `.a2l` include 仍被正确解析（相对路径被改写为绝对路径）。
5. 全程不修改 `thirdparty/`。

## 3. 方案

### 3.1 净化版 A2L 生成规则（用户确认的「形态二」）

对原始 A2L 逐行扫描（复用注释剔除思路，字符串字面量内的 `/include` 不处理）：

| 情形 | 处理 |
| --- | --- |
| `include "X.aml"`（非 `.a2l`） | 整行替换为 `// xcp_master: 已跳过 include "X.aml"` 注释 |
| `include "common.a2l"`（相对路径） | 改写为**绝对路径**（基于原 A2L 所在目录解析后 `weakly_canonical`），保持其它内容不变 |
| `include "<绝对路径>.a2l"` | 原样保留 |
| 注释内、字符串内的 `/include` | 不处理 |
| 无扩展名的 include | 按非 `.a2l` 处理，跳过 |

写绝对路径的理由：净化版放在 `config/`，与原目录无固定相对关系；绝对路径保证
无论从何处加载都能正确解析，且与 SDK 预扫描的 `weakly_canonical` 判定一致。

### 3.2 config 目录与文件命名

- 目录：`<exe目录>/config/`；不存在则创建。
- 文件名：`<原A2L词干>.clean.a2l`。
- 幂等：内容与磁盘现有文件完全一致时不重写（避免每次启动产生 mtime 抖动）。
- 写盘：沿用 `project_file.hpp` 已有的「写 `.tmp` 后 `rename`」原子写入。
- 记录：净化版文件头部插入一段注释，写明来源路径、生成工具、时间戳，便于审查。

> 该注释必须放在 `ASAP2_VERSION` 之前也不影响解析（A2L 允许前导注释），
> 用例中会验证。

### 3.3 加载路径

`main.cpp` 当前加载链（`main.cpp:3960-3973`）为：

```
a2l_load_path = options->a2l_path
  →（TCP）受控快照 → a2l_load_path = 快照路径
  → A2lBridge::Load(a2l_load_path)
```

改造后：

```
原始 A2L
  → 生成净化版到 config/            ← 新增
  → a2l_load_path = 净化版路径
  →（TCP）对净化版再做受控快照       ← 保持不变
  → A2lBridge::Load(a2l_load_path)
```

**净化先于快照**，这样 TCP alignment 的原文核对拿到的也是净化版字节，
与 SDK 实际解析内容一致（符合既有「校验的和解析的是同一份字节」原则）。

### 3.4 对 options.a2l_path 的处理（影响 save 的关键点）

需要区分两个概念：

| 名称 | 含义 | 用途 |
| --- | --- | --- |
| `source_a2l_path` | 用户给的原始 A2L | 日志、诊断、重新生成时使用 |
| `options_.a2l_path` | 写入工程配置文件的路径 | `save` 写它，`-c` 读它 |

**决定：`save` 写净化版路径**（用户明确要求）。理由：净化版才能被加载成功；
若写原始路径，下次 `-c` 加载会重新触发 include 失败。

代价必须写进 README：`save` 后的配置文件与 `config/` 目录**强绑定**。
迁移到另一台机器时若只拷贝 JSON 会失效。

### 3.5 是否复用已有 `.clean.a2l`

启动时先做一次「是否值得复用」判断：

- 净化版不存在 → 生成；
- 存在且**源文件 mtime/hash 未变** → 直接复用（不重新生成）；
- 存在但源已变化 → 重新生成。

判定用源文件大小 + 最后写入时间，不逐字节比对缓存，避免为省一次扫描反增 IO。

## 4. 新增组件

### 4.1 `tools/xcp_master/a2l_cleaner.hpp`

```cpp
namespace xcp_master {

/// @brief 单条被处理过的 include 记录（供日志与测试断言）。
struct IncludeRewrite {
    std::size_t line_no{0};       ///< 行号（1 起，与原文一致）
    std::string original_token;   ///< 原文路径
    enum class Action { Skipped, Absolutized, Kept } action{Action::Kept};
    std::string result;           ///< 改写后的路径（Skipped 时为空）
};

/// @brief 净化结果。
struct CleanedA2l {
    std::string text;                       ///< 净化后全文
    std::vector<IncludeRewrite> rewrites;   ///< 处理记录
};

/// @brief 生成净化版 A2L 文本。
/// @param text 原始全文（逐字节处理，不转码）
/// @param source_path 原始 A2L 绝对路径（用于解析相对 include 与生成头注释）
/// @return 净化结果
[[nodiscard]] CleanedA2l CleanA2lIncludes(std::string_view text,
                                          const std::filesystem::path& source_path);

}  // namespace xcp_master
```

### 4.2 落盘入口

```cpp
/// @brief 确保 config/ 下存在最新的净化版 A2L。
/// @return 净化版路径；失败返回空并给出中文原因。
[[nodiscard]] std::filesystem::path EnsureCleanedA2l(
    const std::filesystem::path& source_a2l,
    const std::filesystem::path& config_root,
    std::vector<IncludeRewrite>& rewrites,   ///< 输出：本次处理详情
    std::string& error);
```

## 5. 对既有 TCP alignment 核对的影响（必须说明）

`main.cpp:4062` 调用 `ScanA2lTcpCandidate(a2l_snapshot->bytes)` **未传 `IncludeResolver`**，
而该函数契约写明「为空时任何 `/include` 都导致拒绝」
（`include/libxcp/a2l_tcp_alignment.hpp:57`）。

含义：

- 原 A2L 只要含**任何** include，TCP alignment 核对结论就是
  `"A2L 原文不在受限子集内，无法证明 alignment 声明位置"`，随后回落到需要显式
  `--packet-alignment`。这是**既有行为**。
- 本次净化会跳过非 A2L include，但**保留的 `.a2l` include 仍会让该核对拒绝**。
  对于只含 AML include 的 A2L，净化后不再含 include，核对反而更容易通过。

本计划**不**改动该核对的保守策略（传不传 resolver 是另一个安全决策），
仅在测试中固化当前行为，避免净化后行为变化被忽视。

## 6. 测试计划

### 6.1 单元测试 `tests/xcp_master_a2l_cleaner_test.cpp`

编入 `XcpMasterSymbolPolicyTest`。

1. 无 include → `rewrites` 为空，正文与原文逐字节一致
2. 单个 `include "XCP.aml"` → 被注释，`action == Skipped`，行号正确
3. 多个非 A2L include → 全部跳过
4. `include "common.a2l"` → `action == Absolutized`，`result` 等于规范化绝对路径
5. 绝对路径 `.a2l` include → `action == Kept`，内容不变
6. 大小写：`.A2L` / `.AML`
7. 无扩展名 include → 跳过
8. 行注释内的 include 不受影响
9. 块注释内的 include 不受影响
10. 字符串字面量内的 `/include "x.aml"` 不被误改
11. 行数不变
12. BOM 保留
13. 头注释插入位置不影响 `ASAP2_VERSION` 解析（配合 6.2 的真实解析）
14. 同一输入两次调用输出一致（确定性）

### 6.2 真实解析集成测试 `tests/xcp_master_aml_include_test.cmake`

用合成 A2L（含不存在的 `include "XCP.aml"` + 一个 MEASUREMENT）跑真实可执行文件：

- 断言退出码为 0（或进入交互后正常超时），且输出含「净化版 A2L」字样；
- 断言 `config/<词干>.clean.a2l` 已生成且含跳过注释；
- 反向用例：缺失的 `.a2l` include 仍应报错（不误伤）。

### 6.3 回归

- `ctest -R "XcpMaster|A2l"`（须把 `TMPDIR`/`TEMP`/`TMP` 指向可写目录，
  否则既有用例会因沙箱临时目录不可写而误报）
- 全量 `ctest`

## 7. 实施步骤

| 步骤 | 内容 |
| --- | --- |
| A1 | 新增 `tools/xcp_master/a2l_cleaner.hpp`，实现 `CleanA2lIncludes` |
| A2 | 实现 `EnsureCleanedA2l`（config 目录、原子写、复用判定） |
| A3 | `main.cpp` 接入生成逻辑并调整加载链（净化 → 快照 → Load） |
| A4 | `options_.a2l_path` 语义调整，确认 `save`/`-c` 一致性 |
| A5 | 失败诊断改进（`include_chain` 优先，`File doesn't exist` 文本兜底） |
| A6 | 新增单元测试并编入 `tests/CMakeLists.txt` |
| A7 | 新增 CLI 集成测试并注册 |
| A8 | 更新 `tools/xcp_master/README.md`（行为、config 目录、迁移限制） |
| A9 | Release 构建 + 定向 ctest + 全量 ctest |
| A10 | 在 `code-plan/` 写实施记录 |

## 8. 验收标准

1. 含缺失 `.aml` include 的 A2L 能正常加载并列出符号。
2. `config/<词干>.clean.a2l` 被生成，内容可读、可审查。
3. 其中相对 `.a2l` include 已改为绝对路径，且能被 SDK 正确解析。
4. 缺失的 `.a2l` include 仍按原样失败。
5. `save` 产出的 JSON 中 `a2l_path` 指向净化版；`-c` 加载该 JSON 能直接启动。
6. 既有测试全部通过。
7. `thirdparty/` 零改动。

## 9. 已知限制与风险

| 风险 | 说明 |
| --- | --- |
| 配置文件与 config 目录绑死 | `save` 写净化版路径，拷贝 JSON 到新机器需同时拷贝 `config/` |
| 绝对路径迁移失效 | 改写成绝对路径后，跨机器迁移需重新生成 |
| 编码 | 逐字节处理，不做 UTF-16/UTF-32 转码 |
| 行扫描非完整词法 | 与 SDK 既有预扫描同级近似；字符串内 `/include` 由用例守住 |
| config 目录写入失败 | 只读安装目录下无法生成；需明确诊断并允许 `--a2l-clean-root` 覆写 |
