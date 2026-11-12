# xcp_master：A2L 缺失非 A2L include（如 XCP.aml）导致加载失败 —— 修复记录

## 1. 问题

现场 A2L 常以 `include "XCP.aml"` 引用 AML 伴生文件。当伴生文件缺失时，整个 A2L 加载失败：

```text
parse failed at line 358: Include file path error. Error: File doesn't exist. File: XCP.aml
```

而 xcp_master 只需要 MEASUREMENT 与 CHARACTERISTIC，AML 定义对工具毫无用处。

## 2. 根因

`thirdparty/a2llib/src/a2lscanner.cpp` 的 `A2lScanner::FixIncludeFile()` 处理顺序是：

1. 解析 include 路径（绝对路径 / 相对当前输入文件）；
2. **先检查文件存在性**，不存在即 `throw std::runtime_error("File doesn't exist. File: ...")`；
3. **之后**才判断扩展名是否 `.a2l`，非 `.a2l` 则不递归解析。

即：扩展名判断在存在性检查之后，缺失的伴生 AML 在还没机会被判定为「非 A2L」时就抛异常了。

AML 确实不承载任何工具需要的信息，两条证据：

- `thirdparty/a2llib/src/a2lparser.y:1043` 的 `module_attribute : a2ml { scanner.CurrentModule().A2ml($1); }` 只把 A2ML 文本塞进 `Module::a2ml_`；
- 真正解析 AML 的 `AmlSection` / `AmlParser`（`thirdparty/a2llib/src/amlsection.cpp`）在 `src` 下**没有任何调用者**。

## 3. 方案演进（为什么最终改 a2llib）

| 方案 | 结论 |
| --- | --- |
| A. 用 `PARSE_MODULE_INFORMATION_ONLY` | 不可行。`a2lparser.y:1001` 遇 `MOD_PAR` 直接 `YYACCEPT`，会丢掉全部 MEASUREMENT / CHARACTERISTIC。 |
| B. 在 xcp_master 侧生成「净化版 A2L」（注释掉非 A2L include） | 曾完整实现（约 470 行 `a2l_cleaner.hpp` + 21 个单测 + CLI 集成），可用但代价过高：需要自研注释/字符串感知的词法扫描、需要处理 `.clean` 递归叠加、需要处理 Windows 路径反斜杠在 A2L 字符串中的转义问题，且生成文件引入「JSON 与 config 目录绑死」「绝对路径跨机失效」等新限制。用户明确要求回退。 |
| C. 改 `FixIncludeFile()` 的处理顺序 | **采用**。约 25 行，在源头修正，所有使用 a2llib 的路径同时受益，不产生任何新文件与新限制。 |

用户决定（m01421 paragphrased）：先回退方案 B 的全部改动，然后允许修改 a2llib，但必须在其上新建分支。

### 3.1 `.gitmodules` 地址修正

`.gitmodules` 原先声明 `git@github.com:songjmcn/a2llib.git`（fork，SSH 形式），而子模块内的 `origin` 是 `https://github.com/ihedvall/a2llib.git`（上游），两者不一致。已统一为上游地址，避免 `git submodule update --init` 与本地已初始化子模块指向不同仓库。

> 注意：用 `sed -i` 改 `.gitmodules` 会把该文件的行尾从 LF 改成 CRLF，从而让 diff 变成整文件重写。应改用 `printf` 重写或保留行尾的编辑器。

## 4. 改动明细

### 4.1 a2llib 分支

```text
thirdparty/a2llib  分支 feature/skip-missing-non-a2l-include（基于 c4999aa）
```

该子模块原本处于 detached HEAD（`c4999aa`，比本地 `main` 多 1 个提交），新分支以 `c4999aa` 为起点，保留了原本正在使用的提交。

### 4.2 `thirdparty/a2llib/src/a2lscanner.cpp` — `A2lScanner::FixIncludeFile()`

改动前：两条分支各自「查存在性 → 取扩展名」，缺失即抛。

改动后：先把绝对/相对路径统一解析成 `resolved`，**提前**取扩展名，再按规则处理：

```cpp
path resolved;
if (fullname.is_absolute()) {
  resolved = fullname;
} else {
  resolved = path(InputFile());  // Current filename with path
  resolved.replace_filename(fullname);
}
file_extension = resolved.extension().string();
// 非 A2L 且缺失 → 跳过，不报错
if (A2lHelper::stricmp(file_extension.c_str(), ".a2l") != 0 &&
    !exists(resolved)) {
  return;
}
if (!exists(resolved)) {          // 到这里只剩 A2L，缺失仍然报错
  ...throw std::runtime_error(...);
}
include_file = resolved.wstring();
```

规则汇总：

| include 目标 | 文件存在 | 行为 |
| --- | --- | --- |
| `.a2l` | 是 | 递归解析（原行为） |
| `.a2l` | 否 | **报错** `File doesn't exist`（原行为，不静默残缺） |
| 非 `.a2l`（含 `.AML` 大小写变体、无扩展名） | 是 | 按原逻辑 include，但不递归解析（原行为） |
| 非 `.a2l` | 否 | **跳过，加载继续**（本次修复） |

## 5. 测试

新增 `tests/a2l_missing_non_a2l_include_test.cpp`，注册为 CTest 目标 `A2lMissingNonA2lIncludeTest`（`tests/CMakeLists.txt`，紧跟 `A2lMultiEndpointTest`）。

6 个用例：

| 用例 | 断言 |
| --- | --- |
| `SkipsMissingAmlCompanionFile` | 核心修复：`.aml` 缺失时加载成功，且 `misinc_m::SigVolt` 可查 |
| `SkipsMissingAmlWithAbsolutePath` | 绝对路径写法的缺失 AML 同样跳过 |
| `ExtensionMatchIsCaseInsensitive` | `.AML` 大写扩展名同样跳过 |
| `IncludesNonA2lFileWhenPresent` | 伴生文件存在时行为不回退，仍正常 include |
| `StillRejectsMissingA2lInclude` | 回归保护：缺失的 `.a2l` include **必须**报错，且文本含 `File doesn't exist` |
| `SkipsMissingIncludeWithoutExtension` | 无扩展名的缺失 include 视为非 A2L，跳过 |

### 区分度验证（关键）

把 `a2lscanner.cpp` 的改动 `git stash` 掉重新构建后重跑：

- 原版 a2llib：**4 个用例失败**（`SkipsMissingAmlCompanionFile`、`SkipsMissingAmlWithAbsolutePath`、`ExtensionMatchIsCaseInsensitive`、`SkipsMissingIncludeWithoutExtension`），`IncludesNonA2lFileWhenPresent` 与 `StillRejectsMissingA2lInclude` 通过；
- 补丁版 a2llib：**6 个用例全部通过**。

证明用例确实覆盖并锁定了本次行为变化，而非恒真断言。

> 验证时的坑：`liba2l.dll` 会被 `add_custom_command(POST_BUILD copy_if_different)` 复制到 `tests/` 目录，ninja 只在 dll 时间戳变化时才复制。手工替换 dll 做对照实验时必须手动 `cp`，否则测的仍是旧副本——第一次对照实验就因为这一点得出了「补丁无效果」的错误结论。

### 回归

| 范围 | 结果 |
| --- | --- |
| `ctest -R "XcpMaster|A2l"` | **193/193 通过**（2 项既有 Skipped） |
| 全量 `ctest` | **1221/1221 通过**（298.39 s，6 项 Disabled/Skipped，均为既有状态） |

## 6. 环境问题排查记录

回退方案 B 后，`ctest` 仍报告 `A2lCleanerSdk.OriginalFileWithMissingAmlFails` 失败。原因是**陈旧构建产物**：`cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe` 及其 discovery 缓存 `XcpMasterSymbolPolicyTest[1]_tests.cmake` 都停留在回退前的 12:43，源文件已删但 ninja 认为无变化、不重链接。

清理方式：

```text
rm -f "cmake-build-release/tests/XcpMasterSymbolPolicyTest[1]_tests.cmake"
rm -f "cmake-build-release/tests/XcpMasterSymbolPolicyTest[1]_include.cmake"
rm -f  cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe
cmake --build cmake-build-release --target XcpMasterSymbolPolicyTest -j 4
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release   # 重新生成 discovery
```

删除 `[1]_include.cmake` 后必须重新 configure，否则 `CTestTestfile.cmake` 的 `include()` 会报文件缺失。

## 7. 全量回归结果

- 全量 `ctest --test-dir cmake-build-release --output-on-failure`：**1221/1221 通过**，用时 298.39 s。
- 未运行（均为改动前既有的 Disabled / Skipped 状态，与本修复无关）：`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`（Disabled）、`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`（Skipped）、`SnapshotTest.StagesReadableCopyOfSource`、`SnapshotTest.CreatesDistinctFilesPerCall`、`SnapshotTest.DeletesSnapshotWhenGuardReleased`、`XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`（Skipped）。

## 8. 已知限制与后续

1. **分支未推送**。`feature/skip-missing-non-a2l-include` 只存在于本地。`.gitmodules` 已与子模块 `origin` 统一为 `https://github.com/ihedvall/a2llib.git`（上游），该仓库无推送权限，因此分支无法直接推送。要让修复在本仓库内可复现，需把主工程的 gitlink 指向本分支的提交（提交存在于 `thirdparty/a2llib` 的本地对象库中，`git clone` 本主工程后能检出），或者等上游合并后整体回退本分支。
2. **建议向上游提 PR**。`thirdparty/a2llib/src/a2lparser.y:44` 定义了 `%token A2L_INCLUDE`，但 `a2lparser.y` 与 `a2lflexer.l` 中均无任何使用——上游原本设计在语法层处理 include，现在成了词法层无条件动作，`FixIncludeFile` 的顺序问题正是这一演化的产物。
3. **`thirdparty/a2l-sdk/CMakeLists.txt:18-20`** 的注释仍写着「子模块只消费不修改」，现已与该目录下的实际状态（含 `a2llogstream_boost_locale.cpp` 目标级替换）以及本次分支改动不一致，属于文档欠账，未在本次修改。
4. 非 A2L 伴生文件**存在**时仍会被读入（原行为，未改），因此不影响已有工程。

## 9. 复现命令

```bash
# 构建
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j 4

# 单测
export TMPDIR="..."; export TEMP="$TMPDIR"; export TMP="$TMPDIR"
./cmake-build-release/tests/A2lMissingNonA2lIncludeTest.exe

# 回归
ctest --test-dir cmake-build-release -R "XcpMaster|A2l" --output-on-failure
```
