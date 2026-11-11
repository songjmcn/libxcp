# xcp_master 工程配置文件功能 实施记录

- 计划文档：`code-plan/xcp_master_工程配置文件_实施计划.md`
- 实施范围：S1–S12 全部完成
- 验证环境：Windows 10/11 + MSVC 19.44 (v14.44.35207) + Ninja，Release 配置

## 1. 交付内容概览

| 需求 | 实现位置 | 状态 |
| --- | --- | --- |
| 首次以参数启动提供 A2L/传输/IP/端口，并声明观测与标定变量 | 原有命令行 + 新增 `observe`/`calibrate` 集合 | 完成 |
| `save` 命令保存为 JSON 工程配置文件 | `CliApp::Save` + `ProjectFileToJson` | 完成 |
| 新参数读取工程配置文件 | `-c` / `--config`（用户确认的命名） | 完成 |

> 说明：用户最初建议参数名为 `-l`，评审后确认语义是「加载配置」而非「列出」，
> 最终采用 `-c`/`--config`。本记录以落地实现为准。

## 2. 新增与修改文件

### 2.1 新增

| 文件 | 作用 |
| --- | --- |
| `include/vendor/nlohmann_json/nlohmann/` | nlohmann/json 3.12.0 头文件树（47 个文件，1.1 MB） |
| `include/vendor/nlohmann_json/LICENSE.MIT` | 上游 MIT 许可证 |
| `include/vendor/nlohmann_json/README.md` | 版本、SHA-256、禁止修改与升级流程 |
| `tools/xcp_master/project_file.hpp` | 工程配置文件的模型、序列化、解析、原子写入 |
| `tests/xcp_master_project_file_test.cpp` | 36 个单元测试 |
| `tests/xcp_master_config_option_test.cmake` | CLI 层 `-c`/`--config` 入口错误冒烟 |

### 2.2 修改

| 文件 | 改动 |
| --- | --- |
| `tools/xcp_master/main.cpp` | `Options` 新增字段与显式标记、`-c/--config` 解析、`MergeProjectFileIntoOptions`、`CliApp` 标定集合与 `save`、`calmark/calunmark/callist`、预览第三页签 |
| `tools/xcp_master/CMakeLists.txt` | 增加 vendor include 目录 |
| `tests/CMakeLists.txt` | 新测试源文件、`XcpMasterToolConfigOption` 冒烟 |
| `tools/xcp_master/README.md` | `-c`、`save`、标定命令、仅标定页签、JSON 格式与优先级 |

## 3. 关键技术决策与理由

### 3.1 vendor 目录位置

未放入 `thirdparty/`（AGENTS.md 明确保护），也未放入 `include/libxcp/`——后者被
`CMakeLists.txt` 的 `install(DIRECTORY include/libxcp ...)` 整体安装，会污染用户
include 根并可能与其他依赖的 json 版本冲突。最终放在 `include/vendor/nlohmann_json/`，
其子目录 `nlohmann/` 保持上游布局，使 `#include <nlohmann/json.hpp>` 无需改写。

### 3.2 使用 `ordered_json`

nlohmann 默认 `json` 以字母序排列键，会使 `a2l_path` 排到 `calibrate` 之后，
与文档样例和人工阅读习惯不符。改用 `using ProjectJson = nlohmann::ordered_json;`
后输出顺序与结构体声明一致。

### 3.3 严格解析

未知键一律拒绝而非忽略：配置文件是用户手写与工具写入共存的格式，静默忽略未知键
会让「我改了配置但没生效」难以定位。JSON 语法错误携带行号列号。
重复键沿用 nlohmann 的后值覆盖语义，并以 `DuplicateKeyUsesLastValue` 用例固化，
避免文档承诺一个实现做不到的行为。

### 3.4 命令行优先级

逐字段判定「命令行 > 工程配置文件 > 内置缺省」。传输层是唯一的整体覆盖例外：
只要命令行出现过任一传输选项（`--transport`/`--tcp-profile`/`--packet-alignment`/`--preset`），
整个 `connection` 块以命令行为准。原因是 `--transport tcp` 与文件内的 `tcp_profile`
混搭会产生一个从未真正连通过的组合。

### 3.5 保存实际生效的连接参数

`save` 写入的是 `ResolveTransportSelection` 之后的实际端点，而非原始输入，
因此「A2L 里声明端点」这种情形也会被固化为可复现的具体 host/port，
并以 `endpoint_from_a2l` 标记来源。DAQ 运行期间拒绝保存，因为此时端点可能
因会话重建而尚未稳定。

## 4. 测试结果

### 4.1 单元测试

`XcpMasterSymbolPolicyTest.exe --gtest_filter="ProjectFile*:ProjectConnection*:ProjectFileIo*"`

```
[==========] 36 tests from 3 test suites ran.
[  PASSED  ] 36 tests.
```

用例分布：`ProjectFile` 27 项（往返 3、缺失/版本 4、类型错误 4、未知键 2、
语法错误 4、边界值 4、枚举与区间 6）、`ProjectConnection` 5 项、`ProjectFileIo` 4 项。

### 4.2 CLI 冒烟

```
XcpMasterToolHelp .............................   Passed
XcpMasterToolRejectsDuplicateProfile ..........   Passed
XcpMasterToolConfigOption .....................   Passed
XcpMasterToolRejectsMissingArgs ...............   Passed
XcpMasterToolRejectsPollingIntervalInDaq ......   Passed
XcpMasterToolRejectsRemovedDaqProfileOption ...   Passed
```

`XcpMasterToolConfigOption` 断言的四条入口诊断：

1. `-c a.json -c b.json` → `参数错误: --config must not be specified more than once`
2. `-c ""` → `参数错误: --config must not be empty`
3. 无参数 → `参数错误: --a2l is required (or supply --config/-c)`
4. `-c <不存在的路径>` → `读取工程配置文件失败: ...`，且诊断包含原路径

### 4.3 全量回归

```
100% tests passed, 0 tests failed out of 1215
Total Test time (real) = 291.74 sec
```

跳过 6 项，均为仓库既有的 Disabled/Skipped 用例，与本次改动无关。

## 5. 实施过程中发现与修正的问题

| 问题 | 现象 | 处理 |
| --- | --- | --- |
| `windows.h` 的 `min`/`max` 宏 | 引入 `<windows.h>` 取可执行文件路径后，`main.cpp` 爆出 100+ 个 `error C2589: "(":"::"右边的非法标记` | 在 include 前 `#define NOMINMAX` 与 `WIN32_LEAN_AND_MEAN` |
| 单文件版 nlohmann 假设错误 | 本机 json.hpp 并非单文件分发版，单独复制后 `fatal error: nlohmann/adl_serializer.hpp: No such file` | 改为整树复制子目录 |
| 测试临时目录在沙箱下不可写 | `create_directories: Access is denied` | 用例辅助函数依次尝试系统临时目录与当前目录，用 `error_code` 不抛异常 |
| MSVC 下 ninja 零动作 | `cmake --build` 长时间无编译产出 | 需先在 `vcvars64.bat` 初始化的环境里构建；在 MSYS 下不能内联 `cmd /c` 引号，改为写临时 bat 文件执行 |

> 关于沙箱：本会话中直接运行 `ctest` 时 `C:\Users\songj\AppData\Local\Temp` 不可写，
> 导致 20 余个与本改动无关的既有用例失败。将 `TMPDIR`/`TEMP`/`TMP` 指向沙箱允许的
> 临时目录后全部通过。这是环境限制，不是代码缺陷。

## 6. 已知限制

1. 工程配置文件 schema_version 固定为 1，尚无迁移机制；版本不符直接拒绝。
2. 不保存加密相关的种子/密钥状态，也不保存 DAQ 事件选择。
3. `observe` 中的符号只登记不自动启动采样，需显式 `watch`。
4. 已存在的目标文件不会被 `save` 覆盖（与其他破坏性操作的保守风格一致）。
5. 标定集合在内存中，只有 `save` 才落盘；退出程序不会自动保存。

## 7. 复现命令

```bash
# 构建
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_TOOLS=ON
cmake --build cmake-build-release -j 4

# 单元测试
cmake-build-release/tests/XcpMasterSymbolPolicyTest.exe \
  --gtest_filter="ProjectFile*:ProjectConnection*:ProjectFileIo*"

# CLI 冒烟与全量回归
ctest --test-dir cmake-build-release -R "XcpMasterTool" --output-on-failure
ctest --test-dir cmake-build-release
```
