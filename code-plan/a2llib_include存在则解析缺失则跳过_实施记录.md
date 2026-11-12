# a2llib `/include` 语义改造：存在即解析、缺失的非 A2L 跳过并留痕

- 日期：2026-11-12
- 触发需求（用户原话）："现在的逻辑是直接忽略.aml文件，我看以前的逻辑是如果文件存在则继续解析，如果不存在则抛出异常。我需要的逻辑是如果文件存在则继续解析，如果不存在则跳过解析include的文件，继续后面的操作。"
- 范围：`thirdparty/a2llib`（解析器）+ `thirdparty/a2l-sdk`（留痕通道）+ 对应测试

---

## 1. 需求与最终语义

### 1.1 三个历史阶段

| 阶段 | `FixIncludeFile()` 顺序 | `.aml` 存在 | `.aml` 缺失 |
| --- | --- | --- | --- |
| 上游原始 | 先判存在性、后判扩展名 | 文本 include 进来 | **抛异常，整份 A2L 加载失败** |
| 上一版提交 `a258f05` | 先判扩展名、非 `.a2l` 直接 `return` | **被静默忽略** | 忽略（加载成功） |
| 本次（目标） | 先判存在性、缺失时按扩展名分流 | 文本 include 进来 | 跳过该 include，**加载成功** |

`a258f05` 的动机是对的（AML 缺失不该拖垮加载），但代价是**文件存在时也被忽略**，与上游行为不一致；同时"跳过"完全无声，调用方无从知道模型少了一份声明。

### 1.2 本次语义矩阵

| 目标文件 | `.a2l`（大小写不敏感） | 非 `.a2l`（`.aml`、无扩展名等） |
| --- | --- | --- |
| **存在** | 按原逻辑解析并 `Merge`（行为不变） | **文本 include 进词法器**（行为回到上游） |
| **缺失** | **抛 `File doesn't exist. File: <path>`**（不变） | **跳过该 include 并继续解析**，且记一条 `LoadWarning` |

两条边界是刻意的取舍：

- **缺失的 `.a2l` 必须继续报错**。A2L include 承载 MEASUREMENT/CHARACTERISTIC 等真实声明，跳过等于静默获得一份残缺模型——那比加载失败更危险。
- **缺失的非 A2L 必须留痕**。加载成功不代表"什么都没发生"，被跳过的路径要能出现在加载告警里。

---

## 2. 改动清单

### 2.1 `thirdparty/a2llib`（子模块，`m thirdparty/a2llib`）

**`thirdparty/a2llib/src/a2lscanner.cpp:309` `void A2lScanner::FixIncludeFile()`**

把 try 块内的顺序改为「取扩展名 → 解析目标路径 → 判存在性 → 缺失时按扩展名分流」：

```cpp
file_extension = fullname.extension().string();
path resolved;
if (fullname.is_absolute()) {
  resolved = fullname;
} else {
  resolved = path(InputFile());   // 以当前文件所在目录为基准
  resolved.replace_filename(fullname);
}
if (!exists(resolved)) {
  // 非 A2L 伴生文件缺失不是错误：它只贡献从不被解释的 A2ML 文本，
  // 跳过该 include，继续解析当前文件；`.a2l` 缺失仍然报错。
  if (A2lHelper::stricmp(file_extension.c_str(), ".a2l") != 0) {
    return;
  }
  std::ostringstream err;
  err << "File doesn't exist. File: " << resolved;
  throw std::runtime_error(err.str());
}
include_file = resolved.wstring();
```

try 块之后的收尾逻辑**一行未改**：`.a2l` 走 `A2lFile temp_file; ParseFile(); parent_->Merge(temp_file)`，非 `.a2l` 走词法缓冲区切换（文本 include）。因此"存在则解析"复用的正是上游原本的路径，不是新写的解析器。

`catch` 块也未改，仍为 `Include file path error. Error: `（路径本身非法时）。

### 2.2 留痕通道：`thirdparty/a2l-sdk`

解析器只`return` 之后，调用方拿不到"跳过了谁"。SDK 侧在**已有的 include 图预扫描**上补一个输出参数，避免再遍历一次文件：

| 位置 | 改动 |
| --- | --- |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:448` | 新增 `bool IsA2lPath(const std::filesystem::path&)`（用 `extension().u8string()` 逐字符比对，避开 locale 转换） |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:472` | `WalkIncludes(...)` 末位新增 `std::vector<std::string>* skipped` |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:553` | 目标缺失且非 A2L 时 `skipped->push_back(PathFullUtf8(resolved))` |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:581` | `PreScanIncludes(...)` 末位新增 `std::vector<std::string>* out_skipped`，入口先 `clear()` |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:1495` | `Doc::LoadInternal` 传入 `skipped_includes`，成功后在 `PublishProgressTo(... kProgressStagePreScan)` 之前发布 |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:1578` | `Doc::ListSkippedIncludes(std::vector<std::string>* out) const noexcept override` |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:1829` | `Doc::ResetState()` 清空 `skipped_includes_` |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp:2275` | `Doc` 成员 `std::vector<std::string> skipped_includes_;` |

`ListSkippedIncludes` 沿用 `LastErrorChain` 的 out-param 分配惯例：本侧先 `clear()` 再填充，空指针返回 `kBadArgument` 且**不改写错误通道**（查询方法不得污染 `LastError`/`LastErrorCode`）。

### 2.3 ABI v12

`thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp:98` `kLibA2lAbiVersion` 由 `11u` 升为 `12u`，`:876` 新增纯虚函数：

```cpp
virtual ErrorCode ListSkippedIncludes(
    std::vector<std::string>* out) const noexcept = 0;
```

**必须升版**：`IDoc` 加虚函数改变 vtable 布局，旧调用方按 v11 布局调用会跳到错误的槽位。判断"能否安全升版"的依据是全仓只有一个 `IDoc` 实现（`thirdparty/a2l-sdk/src/liba2l_export.cpp` 的 `class Doc final : public IDoc`），没有 mock/派生类需要同步改。版本注释按既有 v2…v11 的格式追加 v12 段。

### 2.4 告警映射：`thirdparty/a2l-sdk/a2lbridge`

`thirdparty/a2l-sdk/a2lbridge/src/a2l_bridge.cpp:158` 在 `load_warnings = database->LoadWarnings();` 之后追加：

```cpp
std::vector<std::string> skipped_includes;
if (doc->ListSkippedIncludes(&skipped_includes) == liba2l::ErrorCode::kOk) {
    for (const std::string& path : skipped_includes) {
        LoadWarning warning;
        warning.code = ErrorCode::NotFound;
        warning.phase = Phase::Load;
        warning.subject = path;
        warning.message = "A2L 引用的非 A2L 伴生文件不存在，已跳过该 include：" + path;
        load_warnings.push_back(std::move(warning));
    }
}
```

选 `NotFound` + `Phase::Load` 的理由：事实就是"加载期某个引用的目标不存在"，与既有 `LoadWarning` 语义一致，不需要新增枚举值（新增枚举会再牵动 ABI 与所有 switch）。

`tools/xcp_master/main.cpp` **无需改动**：它已经把 `bridge->ListLoadWarnings()` 逐条打印成 `A2L 告警: <message>`，告警自动出现在启动输出里。

### 2.5 测试

`tests/a2l_missing_non_a2l_include_test.cpp` 整体对齐新语义：

- 文件头注释从"守住四件事"改写为现行语义说明（五个方面），并写明"跳过不静默"的实现路径。
- 新增 `kCompanionAml`：**语法合法**的伴生文件内容——`/begin A2ML ... /end A2ML` 加一个 `MEASUREMENT`。
- 新增 `HasSkippedWarning(bridge, path)`：在 `ListLoadWarnings()` 里按子串找路径。
- `SkipsMissingAmlCompanionFile` 增加"必须产生跳过告警"断言。
- `IncludesNonA2lFileWhenPresent`：伴生内容由 `// present companion file\n` 改为合法 A2ML；**并断言 `misinc_m::AmlInjected` 存在**——这是"文件真的被解析"的决定性证据（文本 include 会把伴生文件里的 MEASUREMENT 注入当前模块），同时断言不产生跳过告警。

`tests/CMakeLists.txt`：`A2lMissingNonA2lIncludeTest` 目标**此前未注册**（构建目录里只有 2026-11-11 的旧 exe，`build.ninja` 已无该目标），本次按 `A2lMultiEndpointTest` 的同款写法补齐 `add_executable` / `cxx_std_20` / `libxcp::a2lbridge GTest::gtest libxcp_test_main` / `/utf-8` / 拷贝 `liba2l.dll` / `gtest_discover_tests(... DISCOVERY_TIMEOUT 120)`。

---

## 3. 设计决策与理由

**D1：只改 `FixIncludeFile()` 的存在性分支，不动 `.a2l` 的 Merge 逻辑。**
`.a2l` include 的 `Merge` 是把子文件的符号并进父文件，语义复杂且有既有用例守着（`tests/a2l_golden_test.cpp` 的 `IncludeTwoLevelLoads` 断言两级符号都在）。本次需求与 `Merge` 无关，改动面越小越好。

**D2：非 `.a2l` 用"文本 include"而不是"当作 A2L 解析后 Merge"。**
上游本来就是这样（扩展名不是 `.a2l` 就切词法缓冲区），保持现状才能让"存在"分支与历史行为逐字节等价。副作用是非 A2L 文件的内容必须符合 A2ML 文本上下文——这正是真实 AML 文件的形态。

**D3：留痕放在 SDK 预扫描，而不是解析器。**
`thirdparty/a2llib` 是被 vendored 的上游库，改它的公开接口成本高；而 SDK 已经为了循环/越根/深度检查遍历过整张 include 图，在同一个循环里"顺手"记下缺失的非 A2L 目标，零额外 I/O。

**D4：ABI 升到 v12 而不是加默认参数/虚函数保持布局。**
纯虚函数是唯一能让"必有实现"由编译器保证的写法；默认参数不能改变 vtable 布局，仍然要升版。既然无论如何都要升版，就选契约最清晰的写法。

**D5：缺失的 `.a2l` 继续报错（用户 Q1 选择）。**
若也为 `.a2l` 跳过，则"加载成功"不再意味着"模型完整"，所有下游（DAQ 布局、标定地址解析）都会在更远处以更难定位的方式失败。

---

## 4. 测试结果

### 4.1 单元用例

```
cmake-build-release/tests/A2lMissingNonA2lIncludeTest.exe
[==========] Running 6 tests from 1 test suite.
[       OK ] A2lMissingNonA2lIncludeTest.SkipsMissingAmlCompanionFile (8 ms)
[       OK ] A2lMissingNonA2lIncludeTest.SkipsMissingAmlWithAbsolutePath (9 ms)
[       OK ] A2lMissingNonA2lIncludeTest.ExtensionMatchIsCaseInsensitive (8 ms)
[       OK ] A2lMissingNonA2lIncludeTest.IncludesNonA2lFileWhenPresent (9 ms)
[       OK ] A2lMissingNonA2lIncludeTest.StillRejectsMissingA2lInclude (8 ms)
[       OK ] A2lMissingNonA2lIncludeTest.SkipsMissingIncludeWithoutExtension (8 ms)
[  PASSED  ] 6 tests.
```

### 4.2 A2L / xcp_master 回归

```
ctest -R "XcpMaster|A2l"  →  100% tests passed, 0 tests failed out of 197
                             （2 项既有 Skipped：Ag1_Cto8、RendersFrameWithFieldSizeCandidateList）
                             总耗时 67.29 s
```

### 4.3 全量回归

```
ctest（全量）
99% tests passed, 1 tests failed out of 1247
Total Test time (real) = 302.70 sec
FAILED: 426 - UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection
```

该项单独复跑通过（`ctest -R "UdpTestSlaveFault"` → `2/2 passed`），随后全量复跑见 §4.4。判定为与本次改动无关的 UDP 端口/时序抖动：本次改动只触及 include 解析与告警装配，`UdpTestSlaveFault` 走的是 UDP 从站故障注入路径。

### 4.4 全量复跑

```
ctest（全量，同一构建产物再跑一次）
100% tests passed, 0 tests failed out of 1247
Total Test time (real) = 301.25 sec
未运行 6 项均为既有 Disabled / Skipped
```

即 §4.3 的 `UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection` 是一次抖动，复跑全绿。

### 4.5 真实 A2L/AML 配对验证（一次性探针）

用 `xcplite_runs/round12/`（本地运行产物，已被 `.gitignore` 排除）里的真实配对做端到端验证：`cpp_demo_V201.a2l` + 1329 行的 `XCP_104.aml`。探针是一个临时 `cl` 程序，直接链接 `libxcp_a2lbridge.lib` + `liba2l.lib` 调用 `A2lBridge::Load`。

```
1) AML 存在
   LOAD OK: symbols=32 warnings=0

2) AML 缺失（只把 .a2l 复制到空目录）
   LOAD OK: symbols=32 warnings=1
     WARN: A2L 引用的非 A2L 伴生文件不存在，已跳过该 include：<dir>\XCP_104.aml

3) .a2l include 缺失（把 include 改成 NotThere.a2l）
   LOAD FAILED: parse failed at line 0: Include file path error. Error: File doesn't exist. File: "<dir>/NotThere.a2l", File: NotThere.a2l
```

三点结论：真实的 1329 行 AML 被解析后**不会破坏加载**（symbols 与缺失时同为 32——AML 只贡献不透明 A2ML 文本，不含符号）；缺失时用真实路径产生告警；`.a2l` 缺失仍然失败。探针程序与临时目录用完即删，不入库。

---

## 5. 已知限制

1. **同一个缺失 include 出现多次会产生多条告警**。`WalkIncludes` 按出现位置逐条记录，没有去重；这是有意的——重复引用本身是 A2L 的写法问题，逐条报出更容易定位到行。
2. **A2ML 块内部的 `/include` 不受本改动影响**。`ReadA2ML()` 是原始字符扫描，不走词法规则，因此 A2ML 内部的 `/include` 从来不会触发 `FixIncludeFile()`。
3. **"存在则解析"会把伴生文件按 A2L 词法上下文解析**。非 A2L 文件的内容若不含合法 A2ML/模块属性，会像上游一样报语法错误——这是上游语义，不是本次引入的。
4. **告警只覆盖非 A2L 缺失**。`.a2l` 缺失直接 `kParseFailed`，不进告警列表。
5. **ABI v12 与旧 `liba2l.dll` 不兼容**。仓库内构建会同时重编 DLL 与使用方；若有外部程序按 v11 头文件链接旧 DLL，需要重新编译。

---

## 6. 复现命令

```bat
:: 构建（MSVC Release，必须经 vcvars64 初始化环境）
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\project\libxcp
cmake --build cmake-build-release -j 4

:: 单元用例
cmake-build-release\tests\A2lMissingNonA2lIncludeTest.exe

:: 回归（先固定 TMPDIR，否则本机受限令牌下既有用例会误报 Access is denied）
set TMPDIR=C:\Users\songj\AppData\Local\Temp\dsh-S7tOzw\xcp_verify
set TEMP=%TMPDIR%
set TMP=%TMPDIR%
cd /d D:\project\libxcp\cmake-build-release
ctest -R "XcpMaster|A2l" --output-on-failure
```

人工验收（用真实的缺失 AML 场景）：

1. 把 `apa_complex.a2l` 与它的 `XCP_104.aml` 放在同一目录，确认两者都在 → 启动 `xcp_master` 应无 `A2L 告警`。
2. 删除 `XCP_104.aml` 后重启 → 应加载成功，且输出一条 `A2L 告警: A2L 引用的非 A2L 伴生文件不存在，已跳过该 include：<路径>`。
3. 把 `/include "XCP_104.aml"` 改成 `/include "NotThere.a2l"` → 应加载失败并报 `File doesn't exist. File: ...`。

---

## 7. 改动文件清单

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `thirdparty/a2llib/src/a2lscanner.cpp` | 修改 | `FixIncludeFile()`：存在即解析，缺失的非 A2L 跳过 |
| `thirdparty/a2l-sdk/src/liba2l_export.cpp` | 修改 | `IsA2lPath`、`WalkIncludes`/`PreScanIncludes` 增 `skipped`、`Doc::ListSkippedIncludes` |
| `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp` | 修改 | ABI v11 → v12，新增纯虚函数 |
| `thirdparty/a2l-sdk/a2lbridge/src/a2l_bridge.cpp` | 修改 | 跳过项转 `LoadWarning`（`NotFound` / `Phase::Load`） |
| `tests/a2l_missing_non_a2l_include_test.cpp` | 修改 | 语义对齐 + "真的被解析"的决定性断言 |
| `tests/CMakeLists.txt` | 修改 | 补注册 `A2lMissingNonA2lIncludeTest` 目标 |

### 7.1 实施中修正的一件事：行尾被整体改写

第一版补丁脚本用 `crlf = '\r\n' in raw` 判断行尾，而 `liba2l_export.cpp` / `a2l_bridge.cpp` / `liba2l_api.hpp` 是 **LF** 文件、仅个别位置含 CR，于是被判为 CRLF 并**整体写成 CRLF**，`git diff` 表现为整文件重写（4569 / 1133 / 2004 行）。已把 4 个文件（含此前同样被误转的 `tools/xcp_master/symbol_display.hpp`）恢复为 LF，`git diff --stat` 回到 17 / 28 / 71 行的真实改动量。

**教训**：判断行尾不能只看"是否含 CRLF"，应判断"是否**全部**行都是 CRLF"（`raw.count('\r\n') == raw.count('\n')`），本仓受版本管理的源码一律 LF。

---

## 8. 复跑记录

- 2026-11-12 全量 `ctest` 第一次：`1246/1247`，唯一失败为 `UdpTestSlaveFault.ResponseCounterStartsFreshAfterEachInjection`（UDP 时序抖动，单独复跑通过）。
- 2026-11-12 全量 `ctest` 第二次：`1247/1247` 全绿（见 §4.4）。

---

## 9. 补充改动：include 报错改走 a2llib 日志通道（`A2L_ERROR`）

### 9.1 动机

`FixIncludeFile()` 原来用 `std::ostringstream err` 自行拼接错误文本后直接抛出：错误只出现在异常消息里，a2llib 自己的日志通道（`A2lLogStream` / `A2L_ERROR()`）**看不到任何记录**。即使调用方注册了日志回调，也拿不到 include 失败的原因。

### 9.2 改动内容

`thirdparty/a2llib/src/a2lscanner.cpp`（文件头新增 `#include "a2l/a2llogstream.h"`）：

1. **跳过分支补日志**——缺失的非 A2L include 被跳过时，明确写出跳过了哪个文件：

```cpp
if (A2lHelper::stricmp(file_extension.c_str(), ".a2l") != 0) {
  // Say which include was skipped: ...
  A2L_INFO() << "Skipped missing non-A2L include: " << resolved;
  return;
}
```

2. **内层只负责异常消息，不再记日志**：

```cpp
// File doesn't exists. Only the exception message is built here: the
// failure is logged once by the catch below, with the final text.
std::ostringstream err;
err << "File doesn't exist. File: " << resolved;
throw std::runtime_error(err.str());
```

3. **`catch` 成为唯一的错误日志点**，日志文本与抛出的异常文本逐字节相同：

```cpp
catch (const std::exception& error) {
  // Single logging point for every include failure: ...
  std::ostringstream err;
  err << "Include file path error. Error: " << error.what()
      << ", File: " << file_path.str();
  const std::string include_error = err.str();
  A2L_ERROR() << include_error;
  throw std::runtime_error(include_error);
}
```

要点：

- 跳过用 **`A2L_INFO()`**、失败用 **`A2L_ERROR()`**，两者都是本库 `src/` 下既有的宏；宏自带 `std::source_location`，日志能定位到 `a2lscanner.cpp` 的行号（a2llib 没有 `A2L_WARNING()` 宏，跳过属于正常降级，故用 INFO）；
- **一次失败只记一条**：`try` 内抛出的异常必定被同一函数的 `catch` 再包一层，所以日志点只放在 `catch`；内层若也记，同一失败会重复两行；
- 日志与异常**同源**：只格式化一次，`err` 仅负责拼接（`std::filesystem::path` 的 `operator<<` 会加引号，换成字符串拼接会丢引号）；
- 先取 `str()` 再 `throw`：`A2L_ERROR()` 的临时对象在语句结束时析构并派发日志，**不会**在栈展开过程中运行用户回调。

### 9.3 兼容性

| 关注点 | 结论 |
| --- | --- |
| 异常文本 | 与改动前**逐字节一致**（仍是 `File doesn't exist. File: "<path>"`） |
| 未注册日志回调 | 与改动前完全一致：`A2lLogStream::LogString()` 只遍历回调列表，默认空表 → 不输出 |
| `.a2l` 缺失仍报错 | 不变（`StillRejectsMissingA2lInclude` 继续通过） |
| `catch` 块 | 改为在 `catch` 记日志：try 内抛出会被它再包一层，只有这里记才能做到"一次失败一行" |
| 跳过的 include | 新增一条 `INFO` 日志，包含被跳过文件的完整路径 |

### 9.4 新增测试 `tests/a2l_include_log_test.cpp`

**为什么不在桥接层用例里覆盖**：`A2lLogStream` 没有从 `liba2l.dll` 导出，桥接层用例注册的回调落在测试进程里的**另一份静态副本**上，永远等不到 DLL 内扫描器的日志。因此新用例直接链接 a2llib 的静态目标 `a2l`（与 DLL 内是同一份源码），在 **a2llib 层**验证日志行为；目标定义照抄既有 `A2lLogTimestampTest` 的写法（`a2l Boost::locale GTest::gtest libxcp_test_main`）。

| 用例 | 断言 |
| --- | --- |
| `MissingA2lIncludeIsLoggedAsError` | 解析失败；`LastError()` 含 `File doesn't exist`；日志回调收到一条 `kError` 记录 |
| `SkippedNonA2lIncludeIsLogged` | 解析成功；日志里有一条说明"跳过"且含被跳过文件名（`XCP.aml`）的记录 |
| `LogTextMatchesLastError` | 每次失败**恰好**一条错误日志；且 `LastError()` 包含该日志文本 |

同时在 `tests/CMakeLists.txt` 注册 `A2lIncludeLogTest`（`/utf-8`、`DISCOVERY_TIMEOUT 60`）。

### 9.5 测试结果

- `cmake-build-release/tests/A2lIncludeLogTest.exe` → **3/3 PASSED**（19 ms）
- `cmake-build-release/tests/A2lMissingNonA2lIncludeTest.exe` → **6/6 PASSED**（49 ms）
- `ctest -R "XcpMaster|A2l"` → **200/200 通过**（67.64 s，2 项既有 Skipped：`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`、`XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`）

### 9.6 本节改动文件

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `thirdparty/a2llib/src/a2lscanner.cpp` | 修改 | 引入 `a2llogstream.h`；缺失 `.a2l` 分支改用 `A2L_ERROR()` |
| `tests/a2l_include_log_test.cpp` | 新增 | a2llib 层验证日志派发（2 个用例） |
| `tests/CMakeLists.txt` | 修改 | 注册 `A2lIncludeLogTest` |

### 9.7 复现

```bash
cmake --build cmake-build-release --target A2lIncludeLogTest A2lMissingNonA2lIncludeTest -j 4
./cmake-build-release/tests/A2lIncludeLogTest.exe
```

> Windows 沙箱下跑 ctest 前需把 `TMPDIR`/`TEMP`/`TMP` 指到可写目录（见 §4.3.1），否则既有用例会误报 `Access is denied`。
