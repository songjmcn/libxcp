# A2L 模块合并（adapter 并入 a2l-sdk）—— 实施记录

> 关联计划：`code-plan/A2L模块合并与命名空间统一_实施计划.md`
> 决策来源：m03614（adapter/a2l 的 4 个文件；"有关 a2l 的操作都封装到一个模块中"）、
> m03663（"命名空间不再修改，只做代码合并的修改。adapter 目录用 thirdparty/a2l-sdk/adapter/{include/a2l,src}"）

## 1. 本次范围

**只做代码合并**。计划文档中 M1/M2/M3/M6 的命名空间统一（`liba2l` → `calmcar::a2l::dll`、
bridge/adapter → `calmcar::a2l`）与 ABI `v12 → v13` **全部取消**，理由：

- 命名空间现状已满足"对外只有 `calmcar::xcp::a2l` 一层"的可用性，改名影响 50 个文件约 240 处，
  且在 `liba2l_export.cpp` 上存在名字查找劫持风险（146 处裸 `a2l::` 指向上游全局命名空间）；
- 改名会硬断 DLL ABI，需要升版并要求所有使用方重建，收益与代价不匹配。

因此实际执行的是 **M5（搬运）+ 对应 CMake 与文档同步**。

## 2. 目录变更

| 变更前 | 变更后 |
| --- | --- |
| `adapter/a2l/a2l_measurement_database.hpp` | `thirdparty/a2l-sdk/adapter/include/a2l/a2l_measurement_database.hpp` |
| `adapter/a2l/measurement_runtime_profile.hpp` | `thirdparty/a2l-sdk/adapter/include/a2l/measurement_runtime_profile.hpp` |
| `adapter/a2l/a2l_measurement_database.cpp` | `thirdparty/a2l-sdk/adapter/src/a2l_measurement_database.cpp` |
| `adapter/a2l/measurement_runtime_profile.cpp` | `thirdparty/a2l-sdk/adapter/src/measurement_runtime_profile.cpp` |
| —（新增） | `thirdparty/a2l-sdk/adapter/CMakeLists.txt` |

主树 `adapter/` 目录已整体删除（`rmdir adapter/a2l adapter`）。四个文件用 `git mv` 迁移，
git 识别为 rename（`git diff --cached --stat` 显示 4 个 rename，0 行增删）。

## 3. CMake 变更

### 3.1 根 `CMakeLists.txt`

删除原 `libxcp_measurement_adapter` 目标定义（12 行），替换为 3 行指向性注释：

```cmake
# --- 测量适配器 --------------------------------------------------------------
# 已并入 A2L 模块：目标 libxcp_measurement_adapter 由 thirdparty/a2l-sdk/adapter
# 定义，并依赖核心 libxcp，故由 SDK 父工程以 if(TARGET libxcp) 守卫。
```

`git diff --stat`：`15 +++------------`（3 增 12 删）。

### 3.2 `thirdparty/a2l-sdk/CMakeLists.txt`（尾部新增 8 行）

```cmake
# --- A2L 适配层（可选子模块；只在主树构建时可用）-----------------------------
if(TARGET libxcp)
    add_subdirectory(adapter ${CMAKE_CURRENT_BINARY_DIR}/adapter-build)
endif()
```

构建顺序依据：`libxcp` 目标在根 `CMakeLists.txt:177` 创建、`libxcp::libxcp` ALIAS 在 `:178`，
`add_subdirectory(thirdparty/a2l-sdk)` 在 `:361`，adapter 目标在 `:429`（tools）/ `:437`（tests）
/ `:450`（examples）之前就已存在。

### 3.3 新增 `thirdparty/a2l-sdk/adapter/CMakeLists.txt`

- 目标名保持 `libxcp_measurement_adapter`（STATIC），消费方零改动；
- `target_include_directories(... PUBLIC include/)` → 消费方沿用 `#include "a2l/xxx.hpp"`；
- `target_link_libraries(... PUBLIC libxcp::libxcp libxcp::a2lbridge)`；
- **显式 `CXX_STANDARD 20`**：SDK 工程在 `thirdparty/a2l-sdk/CMakeLists.txt:31` 全局设
  `CMAKE_CXX_STANDARD 23`（上游 a2llib 要求），适配器与核心一致必须是 C++20，
  写法与 `a2lbridge/CMakeLists.txt` 相同（目标级覆盖父工程默认值）；
- MSVC 下 `/utf-8 /W4`，与桥接层一致。

## 4. 源码变更（4 行）

`#include` 改用公开头根相对路径（原先是"与自己同目录"的裸文件名，迁移后目录拆分为
`include/a2l/` 与 `src/`，裸文件名不再可解析）：

| 文件 | 变更 |
| --- | --- |
| `thirdparty/a2l-sdk/adapter/src/a2l_measurement_database.cpp` | `"a2l_measurement_database.hpp"` → `"a2l/a2l_measurement_database.hpp"` |
| `thirdparty/a2l-sdk/adapter/src/measurement_runtime_profile.cpp` | `"measurement_runtime_profile.hpp"` → `"a2l/measurement_runtime_profile.hpp"` |
| `tests/measurement_adapter_test.cpp` | `"adapter/a2l/..."` → `"a2l/..."`（2 行） |

`tests/measurement_adapter_test.cpp` 是唯一使用旧目录前缀的消费方（其余 5 个消费方本来就写
`"a2l/xxx.hpp"`，未受影响）。**未改动任何命名空间、类名、函数签名。**

## 5. 文档变更

| 文件 | 变更 |
| --- | --- |
| `README.md` | 目录树移除顶层 `adapter/a2l/`、`thirdparty/a2l-sdk/` 下新增 `adapter/`；组件表改为 `thirdparty/a2l-sdk`（含 `adapter/`）；隔离说明中的 target 来源路径更新（4 处） |
| `docs/modules/A2L集成.md` | 分层图路径更新；适配器表格行改为 `thirdparty/a2l-sdk/adapter/`；新增一段说明 `if(TARGET libxcp)` 守卫与独立 SDK 构建跳过（3 处） |
| `docs/modules/代码结构总览.md` | 目录树同步（1 处） |
| `docs/API_说明.md` | "不要将 `adapter/` 实现细节视为稳定接口" → `thirdparty/a2l-sdk/adapter/`（1 处） |

## 6. 验证结果

### 6.1 构建

`cmake --build cmake-build-release -j 4`（MSVC 19.44 / Release）：
第一次 26 个目标后因测试残留旧包含路径失败，修正后 **9/9 全部链接通过**，零错误。
产物：
- `cmake-build-release/thirdparty/a2l-sdk/adapter-build/libxcp_measurement_adapter.lib`（1030530 字节）
- `cmake-build-release/tests/MeasurementAdapterTest.exe`、`tools/xcp_master/xcp_master.exe`、
  `examples/measurement_demo/measurement_demo.exe` 均重新链接成功。

构建目录中旧位置残留的 `cmake-build-release/libxcp_measurement_adapter.lib` 与
`CMakeFiles/libxcp_measurement_adapter.dir` 已删除，避免误链旧产物。

### 6.2 定向回归

`ctest -R "A2l|XcpMaster|Measurement|Adapter"` → **255 项，252 通过，3 失败**：

```
1227 - ExampleXcpMasterUdp (Failed)
1228 - ExampleMeasurementDemo (Failed)
1229 - ExampleCppDemoMeasurement (Failed)
```

三项失败均为**环境原因**，与本次改动无关。同一个失败输出原文：

```
[env] 端口 5555 已有 XCP 应答（残留 Slave？）——拒绝在脏端口上取证
```

实测 `netstat -ano | findstr ":5555"` → `UDP 127.0.0.1:5555` 被 PID 14340 占用，
该进程为 `D:\project\libxcp\cmake-build-release\examples\xcp_slave_demo\xcp_slave_demo.exe`，
启动时间 `2026/11/12 22:39:09`（早于本次改动的 23:09，属此前运行遗留的 Slave）。
这三个 example 用例自带"脏端口拒绝取证"的保护逻辑，因此失败是预期行为。

### 6.3 全量回归（排除受端口阻塞的 example 用例）

`ctest -E "Example"` → **1231/1231 全绿**，237.23 s。
未运行 6 项均为既有状态（1 Disabled、4 Skipped、1 Skipped 能力基准）。

### 6.4 关键用例复跑（EOL 归一后重编译）

`ctest -R "MeasurementAdapterTest|A2lIsolation|A2lIncludeLog|A2lMissingNonA2l"` → **10/10 通过**（2.21 s）。
其中 `A2lIsolation` 通过说明隔离门禁在目录迁移后仍然成立
（S2 扫描主树 `include/`+`src/` 未命中 A2L 符号；S1 扫描桥接层未包含上游头）。

## 7. 行尾（EOL）事故与结论

补丁脚本按"探测到的行尾原样写回"处理，导致 8 个原本索引为 `i/lf` 的文件工作区仍是 `w/crlf`，
`git diff` 表现为整文件重写（`CMakeLists.txt 893 行`、`tests/measurement_adapter_test.cpp 1556 行`）。

诊断命令：

```
git diff --ignore-cr-at-eol --stat CMakeLists.txt   # 只剩真实改动 3 增 12 删
git ls-files --eol CMakeLists.txt                   # i/lf w/crlf
```

处理：把本次触碰的 10 个文件统一 `CRLF → LF`（与索引一致）后，`git diff --stat` 恢复为
**9 文件 26 增 24 删**，与实际改动完全吻合。

**结论（第二次踩坑，写入长期约定）**：本仓索引一律 LF；编辑既有文件后必须用
`git diff --stat` 复核行数量级，若出现整文件行数即为 EOL 事故，用
`raw.replace(b'\r\n', b'\n')` 归一即可。

## 8. 已知限制

1. **独立 SDK 构建未被实测**：`if(TARGET libxcp)` 分支在 `build-sdk.ps1` 的独立构建里为假，
   未实际跑一次 `build-sdk.ps1` 验证"跳过 adapter 后 SDK 仍可独立构建"。逻辑上 adapter 目录
   不参与任何 SDK 产物（`build-sdk.ps1` 只拷贝 `liba2l.dll/.lib` + `libxcp_a2lbridge.lib` + 头树），
   但建议后续单独跑一次确认。
2. **适配器头文件仍以 `a2l/` 为包含前缀**：这是为了消费方零改动；代价是它与桥接层的
   `libxcp/a2l/` 前缀不一致。若要统一路径语义，需要同步改 6 个消费方文件（本次未做）。
3. **命名空间未合并**：`calmcar::xcp::a2l` 保留原样，`liba2l` 仍为 DLL 契约命名空间，
   ABI 仍为 v12。
4. `code-plan/` 下的历史记录（`libxcp_测量子系统_v0.4/v0.5_记录.md`、`DAQ布局自动兼容_实施记录.md`、
   `CMake_A2L集成简化_add_subdirectory_实施记录.md`）中出现的 `adapter/a2l/` 路径为当时事实，
   未回改，属历史记录。

## 9. 复现命令

```bash
# 构建
printf '@echo off\r\ncall "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul 2>&1\r\ncd /d D:\\project\\libxcp\r\ncmake --build cmake-build-release -j 4\r\n' > /tmp/b.bat
cmd.exe /c "$(cygpath -w /tmp/b.bat)"

# 测试（必须先设 TMPDIR/TEMP/TMP，否则既有用例因沙箱 %TEMP% 写权限失败）
printf '@echo off\r\nset TMPDIR=C:\\Users\\songj\\AppData\\Local\\Temp\\<可写目录>\r\nset TEMP=%%TMPDIR%%\r\nset TMP=%%TMPDIR%%\r\ncd /d D:\\project\\libxcp\\cmake-build-release\r\nctest -E "Example" --output-on-failure\r\n' > /tmp/ct.bat
cmd.exe /c "$(cygpath -w /tmp/ct.bat)"
```

人工验收：

1. `xcp_master.exe --a2l <真实A2L> --transport tcp --host 127.0.0.1 --port <port>` 能正常加载并显示变量；
2. 变量预览按 F4 仍能打开标定表单、`save` 仍写出 `calibrate` 字段；
3. 确认 `git status` 中不再出现 `adapter/` 目录；
4. 终止残留 `xcp_slave_demo.exe` 后重跑 `ctest -R "Example"`，三个 example 用例应通过。

## 10. 改动文件清单

| 文件 | 类型 |
| --- | --- |
| `thirdparty/a2l-sdk/adapter/include/a2l/a2l_measurement_database.hpp` | 迁移（rename） |
| `thirdparty/a2l-sdk/adapter/include/a2l/measurement_runtime_profile.hpp` | 迁移（rename） |
| `thirdparty/a2l-sdk/adapter/src/a2l_measurement_database.cpp` | 迁移 + 1 行 include |
| `thirdparty/a2l-sdk/adapter/src/measurement_runtime_profile.cpp` | 迁移 + 1 行 include |
| `thirdparty/a2l-sdk/adapter/CMakeLists.txt` | 新增 |
| `thirdparty/a2l-sdk/CMakeLists.txt` | +8 行守卫 |
| `CMakeLists.txt` | -12 +3 行 |
| `tests/measurement_adapter_test.cpp` | 2 行 include |
| `README.md` | 4 处 |
| `docs/modules/A2L集成.md` | 3 处 |
| `docs/modules/代码结构总览.md` | 1 处 |
| `docs/API_说明.md` | 1 处 |
