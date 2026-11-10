# xcp_master 使用手册

`xcp_master` 是一个面向 Windows、Linux 和 macOS 的 C++20 终端交互工具，可浏览 A2L 符号并连接 XCP on UDP/IP 或 XCP on TCP/IP 从站，执行测量读取、POLL/DAQ 观测和受控标定写入。

本工具只管理 XCP Master 侧连接和操作，不会自动启动、停止或配置外部从站进程，也不依赖 `examples/` 中的演示程序。界面分为变量预览、命令结果和日志视图；异步采样结果由界面统一显示，不会插入正在编辑的命令文本。

## 1. 构建与运行准备

### 构建依赖

- CMake、支持 C++20 的编译器，以及项目要求的 Boost CONFIG 包（`locale`、`filesystem`、`process` 组件）。
- Boost、A2L SDK/桥接库必须与当前编译器 ABI 兼容。工具目标会把 A2L 桥接运行时所需的 `liba2l` 放在可执行文件旁。
- 构建工具时，CMake 会通过 FetchContent 获取固定版本的 FTXUI；离线构建可通过 `FETCHCONTENT_SOURCE_DIR_FTXUI` 指向匹配的本地源码目录。

工具默认不参与构建；`LIBXCP_BUILD_TOOLS` 与 `LIBXCP_BUILD_EXAMPLES` 相互独立，启用工具不会联动构建 XCPlite。

```powershell
cmake -S . -B build -DLIBXCP_BUILD_TOOLS=ON
cmake --build build --config Release --target xcp_master
```

配置生成器要求时，可在 CMake 配置阶段指定 `-DCMAKE_BUILD_TYPE=Release`。使用 Visual Studio 等多配置生成器时，通过 `--config Release` 选择配置。

## 2. 启动参数

```text
xcp_master --a2l <文件路径> [--transport udp|tcp]
           [--host <IPv4> --port <1..65535>]
           [--tcp-profile asam-v1.4|asam-v1.5|auto|auto-legacy]
           [--packet-alignment 8|16|32]
           [--protocol-version <版本>] [--transport-version <版本>]
           [--module <模块名>] [--mode poll|daq] [--interval-ms <毫秒>]
           [--preset xcplite] [--a2l-trusted-snapshot-root <目录>]
```

可通过 `xcp_master --help` 查看可执行文件提供的简要帮助。`--a2l` 是必选项。

| 参数 | 说明 |
|---|---|
| `--a2l <path>` | 必选。待加载的 A2L 文件。 |
| `--transport udp\|tcp` | 传输类型；默认 UDP。TCP 时必须通过 `--tcp-profile`、`--tcp-profile auto` 或 `--preset xcplite` 明确线路布局。 |
| `--host <IPv4>`、`--port <1..65535>` | 目标端点，必须成对提供。UDP 必须显式提供；TCP 可省略，并使用 A2L 中唯一且适用的 `XCP_ON_TCP_IP` 端点。 |
| `--tcp-profile asam-v1.4\|asam-v1.5` | 显式选择 TCP 线路 Profile。不能仅凭 A2L 版本号或 XCP 协议版本推断 Profile。 |
| `--tcp-profile auto` | 严格自动探测：对每个候选布局使用新连接和实际协议栈验证，只接受得到协议层“接受”结论的候选。成功后会关闭验证连接并重新建立工作连接。不能与 `--packet-alignment` 或 `--preset` 一起使用。 |
| `--tcp-profile auto-legacy` | 旧式首成功试连模式。结果不构成唯一 Profile 证据，试连也可能影响从站会话；除非明确需要兼容旧行为，否则优先使用 `auto`。 |
| `--packet-alignment 8\|16\|32` | TCP 数据包对齐位数。ASAM v1.5 必须有可信的对齐信息；可显式提供，或在受信任的 A2L 声明可用时使用。自动 Profile 模式不接受该参数。 |
| `--protocol-version <版本>` | 显式给出 XCP Protocol Layer 版本，例如 `1.3`。 |
| `--transport-version <版本>` | 显式给出 XCP Transport Layer 版本，例如 `1.5`。PL 和 TL 是独立信息；版本号不会替代 TCP Profile 选择。 |
| `--module <name>` | 仅显示指定 A2L 模块中的符号。 |
| `--mode poll\|daq` | 采样模式；默认 `poll`。`poll` 由 Master 发起周期读取；`daq` 使用从站事件驱动的 DTO。 |
| `--interval-ms <ms>` | POLL 目标周期，必须为正整数，默认 500 毫秒。DAQ 模式下不能指定。它不是实时周期保证，实际周期受 ECU 响应和读取数据量影响。 |
| `--preset xcplite` | 使用 XCPlite TCP 部署预设：TCP、`asam-v1.4`、PL 1.2 和 TL 1.4。与预设固定项冲突的显式参数会报错。 |
| `--a2l-trusted-snapshot-root <dir>` | TCP 解析前暂存 A2L 私有副本的目录；默认是可执行文件目录下的 `tmp`。仅当该目录及其父目录权限可证明为可信时，A2L 对齐声明才可能作为可信证据。 |

### TCP Profile 选择说明

- **显式 Profile**：知道从站线路配置时，使用 `--tcp-profile asam-v1.4` 或 `asam-v1.5`。对于 `asam-v1.5`，若 A2L 对齐信息不可信或未声明，应显式给出 `--packet-alignment`。
- **严格自动模式**：`--tcp-profile auto` 会按候选逐一建立新连接并用协议交互验证。A2L 的 TL 版本和 `PACKET_ALIGNMENT` 只影响候选顺序，不直接决定结果。状态会区分“接受”“被拒”（从站明确返回协议违规）和“未决”（没有协议层回复，常见原因是地址、端口错误或从站未运行）。
  - 退出码 `0`：已解析出 Profile。
  - 退出码 `3`：所有候选均被从站拒绝；检查线路布局和对齐配置。
  - 退出码 `4`：至少一个候选未决；先检查目标地址、端口和从站状态。
- **自动模式的安全限制**：不要同时传入 `--packet-alignment` 或 `--preset xcplite`。如果自动验证结果不是“接受”，不要将超时解释成 Profile 不兼容。
- **受控快照**：TCP 会先把 A2L 复制到受控快照再交给 SDK 解析。快照目录及每一级父目录必须只能由当前用户（以及系统/管理员）写入；若权限或路径安全性无法证明，工具会将 A2L 对齐保持为不可信。UDP 不受此限制。

### 启动示例

UDP，显式指定目标端点：

```text
xcp_master --a2l ./ecu.a2l --transport udp --host 192.0.2.10 --port 5555
```

TCP，选择已知的 ASAM v1.4 Profile：

```text
xcp_master --a2l ./ecu.a2l --transport tcp --host 192.0.2.10 --port 5555 --tcp-profile asam-v1.4
```

TCP，选择 ASAM v1.5 并显式设置对齐：

```text
xcp_master --a2l ./ecu.a2l --transport tcp --host 192.0.2.10 --port 5555 --tcp-profile asam-v1.5 --packet-alignment 8
```

TCP，严格自动探测 Profile：

```text
xcp_master --a2l ./ecu.a2l --transport tcp --host 192.0.2.10 --port 5555 --tcp-profile auto
```

若 A2L 声明了适用的唯一 TCP 端点，TCP 示例可省略 `--host` 和 `--port`。UDP 不可省略端点。

## 3. 界面和键盘操作

界面包含状态区、主面板、命令输入区和提示区。主面板有三个视图：**变量**、**命令结果**和**日志**。启动后默认聚焦变量预览。

| 按键 | 操作 |
|---|---|
| `F2` | 循环切换变量、命令结果和日志视图，不改变当前输入模式。 |
| `F3` | 查看当前符号的 A2L 元数据和静态支持状态；不会读取 ECU。 |
| `F4` | 打开当前符号的标定界面；打开界面本身不会写入。 |
| `F5` | 请求停止当前 POLL/DAQ；若有待确认操作，先取消确认。 |
| `F6` | 将当前支持的符号加入/移出观测集合；不会立即启动采样。 |
| `F7` | 进入命令输入模式。 |
| `Enter` | 在变量列表中展开/收起符号，或触发明确显示的单次读取行；在命令模式中提交命令。 |
| `Tab` / `Shift+Tab` | 在变量视图的搜索框、范围菜单和变量列表之间循环焦点。 |
| `↑` / `↓` | 命令模式中浏览历史命令。 |
| `PageUp` / `PageDown` | 在命令结果或日志视图中滚动。 |
| `Esc` | 关闭弹窗；或退出命令模式但保留未提交的输入草稿。确认弹窗中会取消当前确认。 |

命令模式必须通过 `F7` 显式进入；普通字符不会在变量预览模式下误触发命令。界面需要交互式终端；终端小于 80×24 时，变量详情和危险操作确认不可用。部分终端或笔记本设置可能拦截功能键，请先确认目标终端能够传递 `F2`–`F7`。

## 4. 命令参考

在变量视图按 `F7`，输入命令后按 `Enter`：

| 命令 | 用法与作用 |
|---|---|
| `help` | 显示命令简表。 |
| `find [pattern]` | 搜索 A2L 测量量和标定特性；不访问 ECU。省略 pattern 时显示全部候选。 |
| `list [pattern]` | 按模式列出 A2L 名称，每页 40 项；只查询 A2L，不读取 ECU。 |
| `list all` | 在命令结果视图保留完整符号列表，独立于普通结果的 512 行缓存；下一条命令会返回普通结果视图。 |
| `more` | 显示上一次 `list` 的下一页。 |
| `info <module::name>` | 查看符号元数据、事件声明以及保守判定的读写支持状态。 |
| `read <module::name>` | 单次读取标量并按 A2L 转换为物理值。一维测量数组请在变量预览中展开数组并触发其单次读取行；数组按每批 8 个元素读取。 |
| `remove <module::name>` | 从观测集合移除符号。 |
| `watch` | 启动当前观测集合的 POLL 或 DAQ。 |
| `stop` | 停止当前采样。POLL 会等待当前在途 XCP 命令返回或超时。 |
| `write <module::name> <physical-value>` | 请求写入标量标定值；显示当前值和目标值并要求明确确认，成功后回读验证。 |
| `quit` / `exit` | 退出工具。 |

`module::symbol` 表示带模块限定名的 A2L 符号，例如 `ECU::VehicleSpeed`。在 DAQ 运行期间，手动 `read`、`write` 和 `remove` 会被拒绝；先执行 `stop` 再操作。

## 5. 常用操作流程

### 5.1 浏览符号并单次读取

1. 启动工具并加载目标 A2L。
2. 在变量视图使用搜索框，或按 `F7` 执行 `find VehicleSpeed` / `list VehicleSpeed`。
3. 选中符号后按 `F3` 查看元数据。该操作不会访问 ECU。
4. 对可读标量执行 `read ECU::VehicleSpeed`。一维 MEASUREMENT 数组需在变量列表中展开并选择单次读取行。

### 5.2 POLL 周期观测

1. 在变量列表选中可观测符号，按 `F6` 加入观测集合；也可用 `remove <module::name>` 移除。
2. 执行 `watch`。工具先读取一次，再按目标间隔持续读取。
3. 执行 `stop` 或按 `F5` 停止。已取得的最新快照保留并标记为已停止；读取失败时旧值会标记为过期。

可通过 `--interval-ms` 指定 POLL 目标周期，例如 `--mode poll --interval-ms 1000`。实际周期取决于 ECU 响应速度、符号数量和每次读取的数据量，不是硬实时保证。

### 5.3 DAQ 事件观测

1. 以 `--mode daq` 启动。
2. 在变量列表按 `F3` 检查符号是否支持观测；符合条件的符号按 `F6` 加入观测集合。
3. 执行 `watch`。DAQ 使用 ECU 事件驱动 DTO，不使用 `--interval-ms`。
4. 工具会在任何 DAQ 配置下发前检查布局和运行期能力，并提示 `FREE_DAQ` 将清除 ECU **全部动态 DAQ 表**。只有输入 `y`/`Y` 才继续；`n`/`N`、`Esc` 或 `F5` 会取消。
5. 结束时执行 `stop` 或按 `F5`。工具不会恢复 ECU 原有的 DAQ 配置。

DAQ 的支持范围比 POLL 更严格：通常要求 MEASUREMENT 有唯一且可匹配的 `FIXED_EVENT_LIST`/`EVENT_CHANNEL` 声明，并通过从站运行期能力验证。DAQ 数组仅在特定 XCPlite 兼容路径和受限布局下支持。POLL 可读不代表 DAQ 可用；静态准入也不保证 ECU 一定接受配置或产生 DTO。

### 5.4 标定读写

1. 选中支持标定的符号，按 `F4` 打开标定界面。
2. 按界面提示读取当前值、输入目标物理值并检查 A2L 限值和访问状态。
3. 确认界面显示的符号、当前值和目标值正确后，明确输入 `y`/`Y` 执行写入；`n`/`N` 或 `Esc` 取消。`Enter` 不会确认写入。
4. 工具会回读结果，但**不会自动恢复旧值**。回读失败时，目标 ECU 值可能已经改变。

也可使用 `write <module::name> <physical-value>` 发起标量写入。写入还受 A2L 属性、符号类型、地址/字节序布局、运行期 XCP 页面权限等检查约束；A2L 标记为 `READ_WRITE` 并不保证 ECU 一定允许写入。

## 6. 组件与数据流

```text
A2L 文件
  └─ A2L Bridge：解析模块、符号、地址、类型、换算和事件元数据
       ├─ 变量浏览/静态读写与 DAQ 准入检查
       └─ 传输选择：命令行请求 + A2L 端点事实 + TCP 线路证据
            └─ UDP/TCP Transport → XcpMaster 会话与协议命令
                 ├─ POLL：读取响应 → A2L 物理值换算
                 └─ DAQ：DTO 布局/净荷解码 → 物理值换算
                      └─ 变量预览中的最新值快照
```

工具由 [main.cpp](./main.cpp) 负责命令行、会话生命周期和 TUI；[symbol_policy.hpp](./symbol_policy.hpp) 和 [calibration_policy.hpp](./calibration_policy.hpp) 提供界面使用的符号/标定策略；面板行模型和确认状态由 [tui_model.hpp](./tui_model.hpp) 支撑。A2L 元数据用于规划和本地校验，但从站的运行期能力与响应仍是最终依据。

## 7. 安全边界、能力限制与故障排查

### 安全与能力边界

- 只连接已获授权的 ECU/台架。标定写入前核实目标符号、物理单位、限值和现场安全流程；优先使用非关键测试值。
- 仅显式输入 `y`/`Y` 才会确认危险操作。取消或停止若发生在命令下发前会撤销许可；已经发送到 ECU 的 XCP 命令无法撤回。
- `FREE_DAQ` 可能清空从站全部动态 DAQ 表；停止 DAQ 不会恢复先前配置。
- 写入后不会自动回滚。网络中断或回读失败不代表写入没有生效，应按设备流程检查 ECU 状态。
- 读写会保守拒绝未知数据宽度、不支持的类型/布局、越界地址、字节序或地址粒度不匹配等情况。工具不会自行生成或执行 Seed&Key 解锁算法。
- DAQ 不承诺兼容任意 XCP 从站。未知 DTO 头、时间戳模式、事件声明或资源布局均可能被拒绝；CAN 不在本工具支持范围内。
- 一维 MEASUREMENT 数组可在 POLL 中作为完整快照观测。DAQ 数组支持仅限经验证的 XCPlite 兼容场景；不支持通用数组、跨 ODT 拼接或任意 MAP/结构标定。
- 终端日志和普通命令结果各最多保留 512 行；`list all` 的完整目录使用单独结果快照。

### 常见问题

| 现象 | 检查方法 |
|---|---|
| CMake 找不到 Boost 或链接失败 | 安装带 `locale`、`filesystem`、`process` 组件的 Boost CONFIG 包；确认 Boost、编译器和 A2L SDK 使用兼容 ABI。 |
| 首次构建无法获取 FTXUI | 检查网络；离线时将 `FETCHCONTENT_SOURCE_DIR_FTXUI` 指向匹配版本的本地源码。 |
| UDP 无法连接 | 确认 `--host` 与 `--port` 均已提供、地址和端口正确，且目标从站正在运行。 |
| TCP 连接参数报错 | TCP 必须明确选择 `asam-v1.4`、`asam-v1.5` 或 `auto`；`--host`/`--port` 必须成对出现，除非使用 A2L 中的 TCP 端点。 |
| TCP v1.5 对齐不可信 | 显式给出正确的 `--packet-alignment`；不要仅凭 A2L 版本或从站名称猜测线路 Profile。 |
| `auto` 返回退出码 3 | 所有候选都被协议层拒绝，核对 Profile 和 alignment。 |
| `auto` 返回退出码 4 或显示“未决” | 先核实 IP、端口、从站运行状态和网络可达性；无协议回复不能证明候选 Profile 错误。 |
| A2L 快照被判为不可信 | 将 `--a2l-trusted-snapshot-root` 指向只有当前用户可写的目录，或为 v1.5 明确指定 `--packet-alignment`。 |
| 变量不能读、写或加入 DAQ | 按 `F3` 查看具体拒绝原因；检查 A2L 类型、布局/事件声明和 ECU 运行期能力。被 POLL 读到不代表可标定或可 DAQ。 |
| `F2`–`F7` 无响应或确认界面不可用 | 检查终端功能键/Fn 设置，并确认窗口不小于 80×24。 |

## 8. 相关示例

- [UDP Master 示例](../../examples/xcp_master_udp/README.md)：连接 XCPlite 演示从站的自动化 UDP 流程。
- [TCP Master 示例](../../examples/xcp_master_tcp/README.md)：TCP 连接、Profile 配置和受控标定写入示例。
- [项目总览](../../README.md)：库能力、架构和构建开关说明。
