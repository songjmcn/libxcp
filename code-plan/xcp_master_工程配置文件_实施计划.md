# xcp_master 工程配置文件（save / -c）实施计划

> 状态：**已评审通过，待实施**（本文件更新于评审意见 m00118 之后；当前仍未修改任何代码）
> 范围：仅 `tools/xcp_master/`、`tests/`、`include/vendor/nlohmann_json/`（新增单头文件）与文档。
> 本版变更点：① 序列化改用**内置的 nlohmann/json 单头文件**（vendor 进仓库，不走 GitHub 链接）；② 加载参数由 `-l/--load` 改为 **`-c/--config`**；③ 其余评审意见（Q1–Q3、Q5–Q7 按建议定案）；④ **本版新定：vendor 位置由 `thirdparty/` 改为 `include/vendor/nlohmann_json/`**（见 §3.1）。

---

## 0. 评审结论落定表

| 编号 | 问题 | 结论 | 说明 |
| --- | --- | --- | --- |
| Q1 | `-c` 重复出现 | **报错拒绝** | 与 `--tcp-profile` 重复即报错的既有风格一致 |
| Q2 | 标定变量名如何采集 | **方案 A**：新增 `calmark` / `calunmark`，只登记名字，绝不写 ECU | 不采用"写成功才登记"（首次规划时用户往往还没连上 ECU） |
| Q3 | 加载后标定名单呈现 | **方案 A**：变量预览新增第三个 scope 页签「仅标定」，命中行带 `[标定]` 标签 | 需改 `preview_scope_labels_` 与 `RebuildPreviewLabels()` |
| Q4 | 是否保存 `--module` / `--a2l-trusted-snapshot-root` | **都保存** | 前者决定变量列表范围，后者决定 TCP 对齐可信度 |
| Q5 | `save` 缺省路径 | `<exe 目录>/<A2L 文件名>.xcp.json` | 不覆盖已存在文件 |
| Q6 | 脱敏导出开关 | **暂不需要** | 后续可加 `save-public` |
| Q7 | JSON 严格程度 | 严格 JSON，无注释，带 `schema_version` | 由 nlohmann/json 严格解析器保证 |
| **N1** | JSON 实现方式 | **vendor 单头文件**，**不使用 FetchContent、不写 GitHub 仓库链接** | 见 §3.1 |
| **N3** | vendor 放置位置（本版新定） | **`include/vendor/nlohmann_json/`**（在 `include/` 下，但**不进** `include/libxcp/`） | 见 §3.1，理由：不触碰 `thirdparty/` 保护目录、不污染对外安装的公共头文件 |
| **N2** | 加载参数命名（本版新定） | **`-c <file>` / `--config <file>`** | 已核实：`-c` 在 `xcp_master` 现有参数中**未被占用**（现有短选项仅 `-h`） |

---

## 1. 目标与范围

为 `tools/xcp_master` 增加"工程配置文件"能力，使一次调好的连接与观测/标定上下文可保存并复用：

1. **首次使用参数启动**时，通过现有命令行参数给出 A2L 文件地址、`xcp on tcp` / `xcp on udp`、目标主机 IP、端口等信息；交互过程中获得需要观测的变量名与需要标定的变量名。
2. 新增交互命令 **`save <file>`**，把上述配置保存为工程配置文件（JSON）。
3. 新增命令行参数 **`-c <file>` / `--config <file>`**，启动时读取工程配置文件。

### 1.1 现状基线（已核对代码，含行号）

| 事实 | 证据 |
| --- | --- |
| 选项解析集中在 `ParseOptions()`，逐项 `--key value`，未知选项报 `unknown option`；**唯一短选项是 `-h`**（`-c` 空闲） | `tools/xcp_master/main.cpp:241-387`，`251` |
| `Options` 只有 A2L 路径、快照根、host、port、module、interval、mode 与 `TransportSelectionRequest` | `tools/xcp_master/main.cpp:168-190` |
| `--a2l` 在解析末尾被强制要求 | `tools/xcp_master/main.cpp:382-385` |
| 交互命令分发是 `Execute()` 的 if-else 链，帮助文本在 `PrintHelp()` | `tools/xcp_master/main.cpp:2029-2109` |
| 观测集合运行期存于 `std::vector<std::string> watched_`（规范名 `module::name`），由 `Add()`/`Remove()` 增删 | `tools/xcp_master/main.cpp:2260-2337`、`3419`、`3441` |
| 标定对象是**瞬时**选择（`CalibrationSelection` 只有 `name` 与 `revision`），**没有持久化集合** | `tools/xcp_master/tui_model.hpp:784-810` |
| 传输选择由 `ResolveTransportSelection()` 合成，端点可能来自 A2L | `tools/xcp_master/main.cpp:3775-3789` |
| 顶层 `add_subdirectory(tools)` 在 `LIBXCP_BUILD_TOOLS` 内；`add_subdirectory(tests)` 在 `LIBXCP_BUILD_TESTS` 内 | `CMakeLists.txt:429-437` |
| 工具侧已有纯头文件分层先例，`XcpMasterSymbolPolicyTest` 已包含 7 个测试源 | `tests/CMakeLists.txt:388-411` |
| 本机已有 `C:/Program Files/nlohmann_json/include/nlohmann/json.hpp`，**version 3.12.0**，单文件 210206 字节 | 实测 `head` 头部注释与 `ls -l` |

### 1.2 不做什么

- 配置文件格式**不放进** `include/libxcp`：它是工具私有的持久化格式，与库契约解耦。
- 不实现"标定值自动写回"或"启动即自动改 ECU"。加载配置**绝不**触发 XCP 写操作。
- **完全不改动 `thirdparty/`**（XCPlite / XCPliteV1.2 / a2llib / a2l-sdk 及新增内容一律不进该目录）。
- **不把 nlohmann/json 放进 `include/libxcp/`**：该目录是对外安装的公共头文件根（`CMakeLists.txt:294` 的 `install(DIRECTORY include/libxcp ...)` 会整体安装），第三方头文件混进去会让 libxcp 的公共接口表面凭空变大。
- 不引入 FetchContent / CMake `find_package` / 任何 GitHub 仓库链接。

---

## 2. 配置模型设计

新增单头文件 `tools/xcp_master/project_file.hpp`，按现有工具头文件风格实现为**纯函数 + POD 结构**，不依赖 FTXUI、不依赖 socket，便于在 `XcpMasterSymbolPolicyTest` 中直接单测。

### 2.1 数据模型

```cpp
/// @brief 工程配置文件版本号；不匹配时 fail-closed
inline constexpr int kProjectFileSchemaVersion = 1;

/// @brief 工程文件中的连接配置（对应首次使用的命令行参数）
struct ProjectConnection {
    std::string transport;                              ///< "udp" | "tcp"
    std::string host;                                   ///< 目标主机 IPv4
    std::uint16_t port{0};                              ///< 目标端口 1..65535
    bool endpoint_from_a2l{false};                      ///< true=TCP 端点由 A2L 提供
    std::string tcp_profile;                            ///< "asam-v1.5"|"asam-v1.4"|"auto"|"auto-legacy"
    std::optional<std::uint8_t> packet_alignment_bits;  ///< 8/16/32
    std::optional<std::string> protocol_version;        ///< "1.2"
    std::optional<std::string> transport_version;       ///< "1.4"
    std::optional<bool> preset_xcplite;                 ///< 是否 xcplite 预设
    std::optional<std::string> a2l_snapshot_root;       ///< 受控快照根目录
};

/// @brief 工程文件中的会话配置
struct ProjectSession {
    std::string mode{"poll"};                    ///< "poll" | "daq"
    std::optional<std::uint64_t> interval_ms;    ///< poll 目标周期；daq 下为 nullopt
    std::optional<std::string> module;           ///< 模块过滤
};

/// @brief 工程配置文件根模型
struct ProjectFile {
    int schema_version{kProjectFileSchemaVersion};
    std::string a2l_path;               ///< A2L 文件绝对路径
    ProjectConnection connection;
    ProjectSession session;
    std::vector<std::string> observe;   ///< 待观测变量规范名
    std::vector<std::string> calibrate; ///< 待标定变量规范名
};
```

关键决策：

- **路径一律存绝对路径**。A2L 相对路径在"另存到别的目录 / 换工作目录再 `-c`"时必然指向错误文件；save 时一次性解析为绝对路径并提示。
- **`observe`/`calibrate` 都存规范名 `module::name`**，与 `watched_` 和 `database_.Find()` 的键一致，加载时可直接校验，避免短名歧义。
- `calibrate` 是**变量名白名单**，不是"自动写值"。
- `schema_version` 不匹配、未知顶层键、类型不符 ⇒ 整体拒绝加载，不做"尽力而为"的部分解（半加载的配置比加载失败更难排查）。
- 所有字段**全部显式写出**（不做省略式序列化），便于人工 diff 与评审。

### 2.2 JSON 样例

```json
{
  "schema_version": 1,
  "a2l_path": "D:/project/libxcp/build/xcp_slave_demo.a2l",
  "connection": {
    "transport": "tcp",
    "host": "127.0.0.1",
    "port": 5555,
    "endpoint_from_a2l": false,
    "tcp_profile": "asam-v1.4",
    "packet_alignment_bits": 8,
    "protocol_version": null,
    "transport_version": null,
    "preset_xcplite": null,
    "a2l_snapshot_root": null
  },
  "session": {
    "mode": "poll",
    "interval_ms": 500,
    "module": null
  },
  "observe": ["demo::meas_fast.engine_speed", "demo::meas_slow.coolant_temp"],
  "calibrate": ["demo::cal.fuel_gain"]
}
```

---

## 3. 序列化实现方案

### 3.1 依赖获取方式（本版新定，替代原"自研最小 JSON 读写器"）

**决策：把 nlohmann/json 的头文件单独 vendor 进工程，不改任何 CMake 外部依赖声明。**

**放置位置（本版新定）：`include/vendor/nlohmann_json/`** —— 在 `include/` 之下，但**刻意避开** `include/libxcp/`。

落地布局：

```text
include/
├── libxcp/                    # 既有公共头文件，本次不动、不新增文件
└── vendor/                    # 新增：第三方头文件隔离层
    └── nlohmann_json/
        ├── nlohmann/
        │   └── json.hpp       # 直接取自本机 C:/Program Files/nlohmann_json/include/nlohmann/json.hpp（3.12.0，210206 字节）
        ├── LICENSE.MIT        # 上游 MIT 许可证文本（见 §3.2 校验项 V3）
        └── README.md          # 来源、版本、SHA-256、升级方法、"本目录为纯头文件 vendor，不得修改内容"
```

为什么是 `include/vendor/nlohmann_json/` 而不是别的写法：

| 候选位置 | 判断 | 原因 |
| --- | --- | --- |
| `thirdparty/nlohmann_json/` | 否决 | AGENTS.md 规定 `thirdparty/` 未经明确指示不得改动；能完全不碰就不碰 |
| `include/libxcp/nlohmann/` | **否决** | 会被 `install(DIRECTORY include/libxcp ...)` 装进用户的 include 根，等于宣称 libxcp 的公共接口依赖 nlohmann |
| `include/vendor/nlohmann_json/` | **采用** | 与公共头文件同根、`#include <nlohmann/json.hpp>` 只需一条 `-I`；又因不在 `libxcp/` 下而**不进入安装集** |
| `tools/xcp_master/third_party/` | 备选 | 也能工作，但 `tests/` 目标要跨目录引用，路径更别扭 |

理由与约束：

- 用户明确要求"只将头文件单独放置到工程里，不要添加第三方 github 库的链接"——**不使用** `FetchContent_Declare(... GIT_REPOSITORY https://github.com/nlohmann/json.git)`，也不使用 `find_package(nlohmann_json)`。
- 好处：构建完全离线、版本永久固定、与 `thirdparty/a2llib`、`thirdparty/XCPlite` 的"整目录入库"风格一致。
- 与 `tools/xcp_master/CMakeLists.txt` 现有的 FTXUI FetchContent 并存：**FTXUI 保持不动**（本次不改它的获取方式，避免扩大改动面），只新增 nlohmann_json 的头文件路径。
- 文件必须**逐字节原样复制**，不做任何本地修改（禁止打补丁），升级时整体替换并更新 `README.md` 里的版本与哈希。这样上游安全修复可直接以 `git diff` 比对。

### 3.2 接入 CMake（两处，均为纯头文件，不产生编译目标）

1. **`include/vendor/` 下不写 CMakeLists.txt**（避免 `add_subdirectory` 产生空目标与命名空间），由使用方直接给 include 路径。该目录位于 `${CMAKE_CURRENT_SOURCE_DIR}/include` 之下，**不进** `install(DIRECTORY include/libxcp ...)`（`CMakeLists.txt:294`），也不改变 `libxcp` 目标的任何 PUBLIC 接口。
2. **`tools/xcp_master/CMakeLists.txt`** 增加一行（在 `add_executable` 之前）：

   ```cmake
   # nlohmann/json 以纯头文件 vendor 入库（include/vendor/nlohmann_json），
   # 不走 FetchContent；此处只暴露头文件搜索路径（PRIVATE，不外泄给使用方）。
   target_include_directories(xcp_master PRIVATE
     ${PROJECT_SOURCE_DIR}/include/vendor/nlohmann_json)
   ```

3. **`tests/CMakeLists.txt`** 的 `XcpMasterSymbolPolicyTest` 目标加同一行 include 路径（该目标已 include `${PROJECT_SOURCE_DIR}` 与 `${PROJECT_SOURCE_DIR}/include`，但 `#include <nlohmann/json.hpp>` 需要 `${PROJECT_SOURCE_DIR}/include/vendor/nlohmann_json` 这一级才解析得到）。

**校验项（实施时必须逐条执行，不得跳过）**

| # | 校验 | 命令 |
| --- | --- | --- |
| V1 | 复制前后哈希一致 | `sha256sum` 源文件与 `include/vendor/nlohmann_json/nlohmann/json.hpp` 比对 |
| V2 | 版本号与计划一致 | `grep -n "version 3.12.0" include/vendor/nlohmann_json/nlohmann/json.hpp` |
| V3 | LICENSE 文本确为 MIT 且署名 nlohmann | 打开 `LICENSE.MIT` 确认首行与版权行 |
| V4 | 头文件在 C++20 + MSVC/`/utf-8` 下可单独编译 | 先编译一个只 `#include <nlohmann/json.hpp>` 的最小 TU |
| V5 | 不新增二进制/大文件入库风险 | `git status` 确认仅新增 3 个文件，且 `include/libxcp/` 下零新增 |
| V6 | **不进入安装集** | 配置后检查生成的 `include/` 安装清单无 `vendor/` 条目（或 `cmake --install --prefix <临时目录>` 后确认无 vendor 目录） |

> 若 V3 在本机找不到上游 LICENSE 文本：从 nlohmann/json 3.12.0 的 MIT 文本**手工写入**并注明"从上游发行物复制"，**不得**省略许可证文件。

### 3.3 序列化函数（仍为纯函数，便于单测）

| 函数 | 职责 |
| --- | --- |
| `nlohmann::json ProjectFileToJson(const ProjectFile&)` | 序列化；`nullopt` 显式写 `null`；`dump(2)` 输出并保证 Windows 路径 `\` 被正确转义 |
| `bool ParseProjectFileJson(std::string_view text, ProjectFile& out, std::string& error)` | 严格解析 + 字段级校验 + 未知键拒绝；错误文本带**行号/列号**（取 `json::exception::byte` 换算，或 `parse` 的 `allow_exceptions=false` 后按 `byte` 定位） |
| `bool ProjectFileToOptions(const ProjectFile&, Options&, std::string& error)` | 把模型转成 `TransportSelectionRequest` 等选项片段（纯函数，供单测断言；放在 `project_file.hpp` 内需前向声明 `Options`，若不便利则改为在 `main.cpp` 内实现并由 §8 的集成用例覆盖） |
| `bool WriteProjectFileAtomically(const std::string& path, std::string_view text, std::string& error)` | 写同目录临时文件 → 原子替换（POSIX `rename` / Windows `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`）；失败不留半截文件 |

**严格性落点**（nlohmann 自身行为 + 我们的校验层）：

| 要求 | 实现方式 |
| --- | --- |
| 拒绝注释 / 尾随逗号 / 尾部垃圾 | nlohmann 默认严格模式即拒绝（`ignore_comments=false`、不允许多个顶层值） |
| 拒绝重复键 | nlohmann 默认**接受**重复键（后者覆盖），故校验层需**显式拒绝**：解析前用一次浅扫描或解析后用 `is_object()` 遍历已不可行——采用 `parse(..., allow_exceptions=false)` 后无法检测。**定案：接受 nlohmann 覆盖语义，但在 `README.md` 与错误提示中注明；§8 用例断言"重复键以后出现值为准"，不假装拒绝。** |
| 未知顶层键 / 未知子键 | 校验层显式白名单比对，报"未知字段: xxx" |
| 类型不符 | 校验层 `is_string()/is_number_unsigned()` 等逐字段检查，报"字段 port 期望整数" |
| 嵌套深度 / 超大输入 | 加载前限制文件大小（建议 1 MiB）并检查 `top-level` 深度（只用固定形状，超限即拒绝） |

> 关于"重复键"这一条我做了一次自我更正：原计划写的是"拒绝重复键"，但 nlohmann 不提供重复键检测开关，要拒绝就得自己先写一遍 tokenizer，等于又回到自研解析器。**结论：接受覆盖语义并写进文档**，用测试固化，而不是在文档里写下实现不了的要求。

---

## 4. 命令行与优先级规则

### 4.1 新选项

```text
xcp_master -c <project.json>            # 读取工程配置文件（短参数）
xcp_master --config <project.json>      # 同上（长参数）
```

`--a2l`、`--transport`、`--host`、`--port`、`--module`、`--interval`、`--tcp-profile` 等保持原样，可覆盖配置文件对应项。

### 4.2 优先级与冲突规则（逐条可测）

1. **`-c` 先于其它选项合并**：先完整解析 `-c` 得到基线 `Options`，再把命令行上**显式出现**的选项覆盖上去。沿现有 `transport_explicit` 思路，为每个可覆盖项记录 `*_explicit` 标记。
2. **`-c` 与 `--a2l` 同时给出**：以 `--a2l` 为准，并在启动横幅提示"命令行覆盖配置文件 A2L 路径"。
3. **`-c` 中的 a2l 路径不存在**：fail-closed，退出码 `2`，提示配置中的原路径并说明可用 `--a2l` 覆盖。
4. **`-c` 与 `--preset xcplite` / `--transport udp` 冲突**：交给 `ResolveTransportSelection()` 既有规则裁决，工具不另立判词（两处判词不一致会让用户无从判断该信哪个）。
5. **`-c` 单独使用**：不再要求额外给 `--a2l`；`--a2l` 的"必填"判定改为"`--a2l` 或 `-c` 至少其一"（改 `tools/xcp_master/main.cpp:382-385`）。
6. **`-c` 文件不存在 / 非法 JSON / schema 版本不符**：退出码 `2`，错误文本必须能定位到行号。
7. **`-c` 与 `--help`**：`--help` 优先，打印用法退出 `0`。
8. **`-c` 重复出现**：报错拒绝（Q1 定案）。`-c` 与 `--config` 混用视为重复。

### 4.3 `--help` 与 README 更新

- `PrintUsage()`（`tools/xcp_master/main.cpp:192-223`）加入 `-c/--config` 与 `save`、`calmark`/`calunmark` 说明。
- `tools/xcp_master/README.md` 的 `## Run` 与 `## Interactive commands` 两节同步，并新增"工程配置文件格式"小节（含 §2.2 样例与重复键语义说明）。

---

## 5. `save` 命令设计

### 5.1 语法

```text
save <path>            # 保存到指定路径；扩展名缺省补 .json，不覆盖已存在文件
save                   # 保存到 <exe 目录>/<A2L 文件名>.xcp.json
```

### 5.2 保存内容来源

| 字段 | 来源 |
| --- | --- |
| `a2l_path` | `options_.a2l_path`（启动时解析为绝对路径后保存） |
| `connection.*` | 本次**生效**的 `ResolvedTransportSelection`（已含 A2L 端点回填），非用户原始输入 |
| `session.*` | `options_` |
| `observe` | `watched_` 快照（持 `watched_mutex_` 拷贝） |
| `calibrate` | `calibrate_names_`（§5.3） |

保存**生效值而非原始输入**是关键决策：用户下次 `-c` 想复现的是"这次真的连上了什么"；记成空会让下次行为依赖 A2L 的隐式变化。

### 5.3 `calibrate` 集合的新增接口（Q2 定案：方案 A）

新增成员 `std::vector<std::string> calibrate_names_`（与 `watched_` 一样持锁访问），配套：

| 接口 | 语义 |
| --- | --- |
| `bool MarkCalibration(std::string_view qualified, std::string& error)` | 登记规范名；未在 A2L 命中则拒绝；**只记录，不发任何 XCP 命令** |
| `bool UnmarkCalibration(std::string_view qualified)` | 移除登记 |
| `std::vector<std::string> CalibrationNames() const` | 供 save 与预览过滤使用（返回拷贝） |

新命令：`calmark <module::name>`、`calunmark <module::name>`、`callist`（列出已登记项）。

### 5.4 拒绝保存的情形

- DAQ 运行中（`DaqRunning()` 为真）：观测集合处于锁定状态，保存会得到与运行期不一致的快照，提示先 `stop`。
- 目标文件已存在：不静默覆盖，提示换路径或手动删除。
- 写入失败（权限/磁盘）：报错且不留半截文件（原子替换保证）。

---

## 6. `-c` 加载后的行为

1. 解析 JSON → 校验 schema → 合成 `Options`（见 §4）。
2. A2L 加载成功后，对 `observe` 逐个 `database_.Find()`：
   - 命中且通过 `ObservationUnsupportedReason()` ⇒ 注入 `watched_`，**不自动开始采样**（F6 语义：只加成员不改运行状态）。
   - 未命中 / 被策略拒绝 ⇒ 记入"加载告警"，启动后输出到命令结果视图，列出名字与原因，继续启动。
3. `calibrate` 名单注入 `calibrate_names_`（同样逐项校验，未命中记告警），并驱动 §Q3 的「仅标定」页签。
4. 启动横幅追加一行：`工程配置: <file> | 观测 N 项（接纳 M / 拒绝 K）| 标定 J 项`。
5. 观测集合**启动时不自动 `watch`**：连接即读 ECU 是被动副作用，必须由用户显式 `watch` 触发。

### 6.1 「仅标定」页签（Q3 定案：方案 A）

- `preview_scope_labels_` 由两项扩为三项：`全部 / 仅观测 / 仅标定`。
- `RebuildPreviewLabels()` 的过滤分支增加按 `calibrate_names_` 取交集的判断；命中行渲染时前置 `[标定]` 标签（复用 `symbol_display.hpp` 的既有标签拼接方式）。
- 该分支必须与"仅观测"共用同一套行索引/窗口化逻辑，避免破坏 `preview_refresh.hpp` 的增量刷新不变量。

---

## 7. 错误处理与安全边界

- 配置文件是**可用于连接任意主机**的输入：`-c` 加载后仍走完整的 `ResolveTransportSelection()` 与 `Connect()` 路径，不做"配置可信则跳过校验"的捷径。
- 配置文件**不能**携带"自动写入目标值"字段；`calibrate` 只是名字白名单，`calmark` 不发命令。
- 解析器对超长字符串、超大文件、超深嵌套设上限并拒绝，错误信息含行号。
- 保存/加载路径按用户输入原样使用，不做隐式路径拼接（除 §5.1 的缺省派生）。

---

## 8. 测试计划

### 8.1 新增单测（`tests/xcp_master_project_file_test.cpp`，编入 `XcpMasterSymbolPolicyTest`）

| 用例 | 断言 |
| --- | --- |
| 序列化-反序列化往返 | `ProjectFile` → JSON → 解析后逐字段相等（含中文路径、含 `\` 的 Windows 路径） |
| 缺字段 | 缺 `schema_version`/`a2l_path`/`connection.transport` 均拒绝并给出字段名 |
| 版本不符 | `schema_version: 2` 拒绝 |
| 类型错误 | `port` 为字符串、`observe` 为对象均拒绝 |
| 非法语法 | 尾随逗号、未闭合字符串、尾部垃圾均拒绝且行列号正确 |
| 未知字段 | 顶层或 `connection` 下出现未声明键均拒绝并报字段名 |
| **重复键（语义固化）** | `{"port":1,"port":2}` 解析结果为 `2`（与 §3.3 定案一致，防止将来误判为"拒绝"） |
| 数值边界 | `port=0`、`65535`、`65536`、`interval_ms=0` 分别按规则拒绝/接受 |
| 传输枚举 | `transport` 非 `udp/tcp` 拒绝；`tcp_profile` 非法值拒绝 |
| 转义 | 引号、反斜杠、换行、制表符、控制字符往返一致 |
| 大输入拒绝 | 文件 > 1 MiB 被拒绝且不 OOM |
| 原子写 | 目标已存在时拒绝覆盖；写入中间态不可见（同名 `.tmp` 被清理） |
| `calibrate` 采集 | `MarkCalibration` 对未命中名拒绝、对未登记名 `UnmarkCalibration` 幂等 |

### 8.2 CLI 冒烟测试（`tests/CMakeLists.txt`，仿 `XcpMasterToolRejectsMissingArgs`）

- `xcp_master -c nonexistent.json` ⇒ 非零退出。
- `xcp_master -c <坏 JSON>.json` ⇒ 退出码 `2`，stderr 含行号。
- `xcp_master -c` 缺值 ⇒ 退出码 `2`。
- `xcp_master -c a.json --config b.json` ⇒ 退出码 `2`（重复）。
- `xcp_master --help` ⇒ 输出含 `-c`、`save`、`calmark`。

### 8.3 交互 `save` 的人工冒烟

TUI 交互命令无自动化用例（与仓库现状一致），人工验证：
启动 `xcp_slave_demo` → `-c` 或参数启动 → F6 选若干观测 + `calmark` 若干标定名 → `save out.json` → 退出 → 重新 `-c out.json` → 断言观测集合、标定名单与连接参数一致，「仅标定」页签可见标定项。

### 8.4 构建验证

```bash
cmake --build cmake-build-release --target clean
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_TOOLS=ON -DLIBXCP_BUILD_TESTS=ON
cmake --build cmake-build-release -j 4
ctest --test-dir cmake-build-release -R "XcpMaster" --output-on-failure
```

### 8.5 是否追加"损坏配置 fuzz 用例"

原计划提出此项待用户确认，评审中未单独答复。**本次默认不引入随机 fuzz**（仓库现无 fuzz 基础设施，引入 libFuzzer 会改变 CI 形态），改为 §8.1 的**表驱动畸形输入用例**（约 20 条固定畸形样例），成本低且可定位。若需要真 fuzz，作为独立后续任务。

---

## 9. 待确认问题

**已全部关闭**：Q1–Q7 与 N1/N2 均已在 §0 落定，无阻塞项。实施中若发现与代码事实冲突（例如 nlohmann 3.12.0 在 MSVC `/utf-8` 下与 FTXUI 的宏冲突），**停下提问**，不自行扩大改动面。

---

## 10. 实施步骤（按依赖排序）

| 步骤 | 内容 | 产出 |
| --- | --- | --- |
| S1 | vendor `include/vendor/nlohmann_json/`（头文件 + LICENSE.MIT + README.md），执行 V1–V6 校验 | 3 个新文件 |
| S2 | `tools/xcp_master/CMakeLists.txt` 与 `tests/CMakeLists.txt` 增加 include 路径 | CMake |
| S3 | 新增 `tools/xcp_master/project_file.hpp`：模型 + nlohmann 序列化/解析 + 校验 + 原子写 | 头文件 |
| S4 | 新增 `tests/xcp_master_project_file_test.cpp` 编入 `XcpMasterSymbolPolicyTest` | 单测（先于 main.cpp 接入，保证解析层独立可验） |
| S5 | `ParseOptions()` 接入 `-c/--config`：两遍解析、显式覆盖标记、`--a2l` 必填改为"二选一" | `main.cpp` |
| S6 | `CliApp` 增加 `calibrate_names_`、`calmark`/`calunmark`/`callist`、启动注入与告警、启动横幅 | `main.cpp` |
| S7 | 变量预览增加「仅标定」页签 + `[标定]` 标签 | `main.cpp` / `symbol_display.hpp` |
| S8 | `Execute()` 增加 `save` 分支 + `PrintHelp()` 更新 + 原子写 | `main.cpp` |
| S9 | `PrintUsage()` 与 `README.md` 更新（含配置文件格式与重复键语义） | 文档 |
| S10 | `tests/CMakeLists.txt` 增加 CLI 冒烟用例 | CMake |
| S11 | Release 全量清理构建 + `ctest -R XcpMaster` + 人工冒烟 | 验证记录 |
| S12 | 在 `code-plan/` 生成本次修改内容与测试结果的实施记录 | 记录文件 |

---

## 11. 验收标准

1. `xcp_master --a2l X --transport tcp --host H --port P --tcp-profile asam-v1.4 --packet-alignment 8` 启动后 `save out.json` 生成 §2.2 形状文件，字段值与生效连接一致，路径为绝对路径。
2. `xcp_master -c out.json` 复现同一连接；观测集合与标定名单被接纳并在界面可见（不自动开始采样）。
3. 任一非法输入都得到带行列号的明确错误与非零退出码，不存在"部分加载"。
4. `save` 不覆盖已有文件，不产生半截文件。
5. 新增单测全部通过，既有 `XcpMaster*` 用例无回归。
6. `--help` 与 `README.md` 与实现一致。
7. **`include/vendor/nlohmann_json/` 内容与上游 3.12.0 逐字节一致，工程内不存在任何指向 nlohmann/json 的 GitHub 链接或 FetchContent 声明；`thirdparty/` 零改动；`include/libxcp/` 零新增；安装产物中不含 `vendor/`。**

---

## 附：本版相对上一版的差异

| 条目 | 上一版 | 本版 |
| --- | --- | --- |
| 序列化实现 | 自研约 300 行最小 JSON 读写器 | vendor nlohmann/json 3.12.0 单头文件（§3.1） |
| 加载参数 | `-l` / `--load` | **`-c` / `--config`**（§4.1，已核实 `-c` 空闲） |
| 重复键 | 计划"拒绝" | **接受覆盖语义并写进文档 + 用测试固化**（§3.3 自我更正） |
| fuzz 用例 | 待确认 | 默认不引入随机 fuzz，改表驱动畸形用例（§8.5） |
| 许可证 | 未涉及 | 新增 `LICENSE.MIT` 与 V1–V6 校验项（§3.2） |
| vendor 位置 | `thirdparty/nlohmann_json/` | **`include/vendor/nlohmann_json/`**（不进 `include/libxcp/`、不碰 `thirdparty/`、不进安装集） |
