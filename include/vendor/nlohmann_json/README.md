# nlohmann/json（纯头文件 vendor）

本目录是 [nlohmann/json](https://github.com/nlohmann/json) 的单头文件分发副本，
以**纯头文件形式**直接入库，供 `tools/xcp_master` 读写工程配置文件使用。

## 版本与来源

| 项目 | 值 |
| --- | --- |
| 版本 | 3.12.0 |
| 许可证 | MIT（见同目录 `LICENSE.MIT`） |
| 文件 | `nlohmann/json.hpp` |
| 字节数 | 210206 |
| SHA-256 | `a6f1d9941c1bc2415772ea0ed109acddf5577ab6c54ce04967768640546767cf` |

`nlohmann/json.hpp` 为单文件分发版本的**逐字节原样副本**，未做任何本地修改。

## 使用方式

本目录**不提供 CMake 目标**（刻意不写 `CMakeLists.txt`，避免产生空目标与命名空间）。
使用方直接指定头文件搜索路径：

```cmake
target_include_directories(<target> PRIVATE
  ${PROJECT_SOURCE_DIR}/include/vendor/nlohmann_json)
```

随后即可 `#include <nlohmann/json.hpp>`。

工程内**不使用** `FetchContent`、`find_package` 或任何指向 GitHub 仓库的链接来获取本依赖，
以保证构建可完全离线、版本永久固定。

## 目录位置说明

本目录刻意放在 `include/vendor/` 而**不是** `include/libxcp/`：

`CMakeLists.txt` 中的安装规则 `install(DIRECTORY include/libxcp ...)` 会把
`include/libxcp/` 整体安装到使用方的 include 根目录。若把第三方头文件放进去，
等于对外宣称 `libxcp` 的公共接口依赖 nlohmann/json，而它实际只是工具层的实现细节。
放在 `include/vendor/` 下则不会被安装，也不影响 `libxcp` 目标的任何 PUBLIC 接口。

## 修改规则

* **禁止修改** `nlohmann/json.hpp` 的任何内容（不打补丁、不做本地适配）。
* 升级时整体替换该文件，并同步更新本 README 中的版本号、字节数与 SHA-256。
  这样上游的安全修复可以直接用 `git diff` 比对。

## 升级步骤

1. 取得目标版本的单文件分发 `json.hpp`。
2. 替换 `nlohmann/json.hpp`。
3. 更新本文档的「版本与来源」表格（版本、字节数、SHA-256）。
4. 若上游许可证文本有变化，同步更新 `LICENSE.MIT`。
5. 运行 `ctest -R "XcpMaster"`，确认工程配置文件的序列化用例仍全部通过。