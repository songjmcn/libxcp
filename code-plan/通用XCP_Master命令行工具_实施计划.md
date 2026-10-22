# 通用 XCP Master 命令行工具实施计划与实施记录

## 0. 目标与已确认决策

- 目标：新增可独立运行的 XCP Master **tool**，由使用者提供本地 A2L 文件和目标 Slave 的 UDP 地址；浏览 A2L 符号，选择变量轮询观测，选择可修改的标量标定量写入并回读。
- 位置：`tools/xcp_master/`，**不放到 `examples/`**。新增 `tools/CMakeLists.txt` 作为工具目录入口；不复制、不链接、不包含 `examples/` 下任何示例代码，不连接、不启动 XCPlite 的 `cpp_demo`，不依赖它生成 A2L。
- 已确认：命令行交互；必要配置走命令行参数；初版只支持 XCP on UDP；初版只做轮询、不配置 DAQ；修改仅限可换算的标量 `CHARACTERISTIC`；测试选择为自动化测试 + 可选真实 Slave 手工验收，自动化不依赖 `cpp_demo`。
- 用户已于 m00182 明确批准实施；本文下方附实际变更与验证记录，未完成验证的项目会明确标记。

## 1. 项目现状与设计依据

- `CMakeLists.txt` 目前以 `LIBXCP_BUILD_EXAMPLES=ON` 进入 `examples/`，且自动开启 `LIBXCP_BUILD_XCPLITE_SLAVE`；新工具不得挂在此门下或依赖它。A2L 桥接层在主工程必编；核心 `libxcp` 与 A2L 适配器已有独立目标。
- `include/libxcp/udp_transport_config.hpp` 提供可配置的远端 IPv4 地址和 UDP 端口；`include/libxcp/xcp_master.hpp` 已提供 `Connect()`、`Disconnect()`、`GetSessionParameters()`、`ReadMemoryBytes(...)`、`WriteMemoryBytes(...)`、`QueryStatus()` 和 `Unlock(...)`。初版不扩展传输协议或库的公共接口。
- `thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/a2l_bridge.hpp` 提供本地文件 `A2lBridge::Load(...)` 与 `Database()`；`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/ia2l_database.hpp` 提供 `Search/Find/ToPhysical/FromPhysical`。`SymbolInfo` 包含 `kind`、`characteristic_type`、`dimensions`、`read_write`、地址扩展、类型、字节宽和换算信息。
- `include/libxcp/measurement/measurement_session.hpp` 走 DAQ，不用于本阶段轮询；`adapter/a2l/measurement_runtime_profile.hpp` 的 profile 明确为 XCPlite 专用，不得把它当作任意 Slave 的默认参数。
- `tests/CMakeLists.txt` 已使用 GoogleTest 和 A2L 生成件；新测试只依赖适用的公开库接口与新工具自身逻辑，不借用任何 `examples/` 源文件。

## 2. 交付文件与构建边界

| 拟新增或修改的位置 | 用途 |
|---|---|
| `tools/CMakeLists.txt` | 仅挂接 `xcp_master/`，不引用旧示例。 |
| `tools/xcp_master/CMakeLists.txt` | 工具独立目标以及所需的 A2L 运行时动态库部署规则。 |
| `tools/xcp_master/` 下新的 C++20 源文件 | 参数与菜单、符号目录、轮询、标定写入、错误展示；具体文件数量在实现前保持最小。 |
| `tools/xcp_master/README.md` | 三平台 Release 构建、参数说明、连接准备、风险提示和真实设备操作步骤。 |
| 根 `CMakeLists.txt` | 新增独立选项 `LIBXCP_BUILD_TOOLS`（默认 `OFF`）及 `add_subdirectory(tools)`；不改变 `LIBXCP_BUILD_EXAMPLES` 的现有语义，工具启用不自动构建 XCPlite。确保工具目标先于依赖它的测试目标定义。 |
| `tests/` 下新测试与必要的 `tests/CMakeLists.txt` 增量 | 测试目标只在工具选项及测试选项均开启时构建；使用模拟 XCP Transport/可控 UDP 测试对端和自带 A2L 测试文件，不使用 `cpp_demo`。 |
| `code-plan/` 下实施记录 | **实现及验证后**记录实际改动、构建与测试结果，不在本计划阶段伪称已测试。 |

建议链接现有 `libxcp::libxcp` 与 `libxcp::a2lbridge`；本阶段不修改 `thirdparty/`、已完成的库实现、`examples/` 或 XCPlite 构建逻辑。不在源码、脚本、CMake 中写死绝对路径。Windows/Linux/macOS 均按 C++20 实现；平台相关部署细节在构建时验证。

## 3. 命令行与交互设计

- 启动形式：`xcp_master --a2l <path> --host <ipv4> --port <1..65535> [--module <name>] [--interval-ms <ms>]`。三项必填；`--module` 用于多 MODULE 文件中指定符号范围，避免裸名歧义；轮询间隔有明确正数默认值，最终数值在完整设计审批时确认。支持 `--help`；未知参数、文件不可读、无效端口/间隔返回参数错误并打印帮助。运行时不从 A2L 猜端点。
- 启动后先加载并展示 A2L 摘要、符号总数及加载告警，再连接指定 Slave；连接失败时给出明确诊断并退出/返回重试提示，不展示虚假的实时值。
- 界面形态已确认：初版采用普通命令行交互（`xcp>` 提示符），**不做上下分区 TUI**，不依赖终端清屏、光标移动或窗口重绘；跨平台普通终端与重定向输出均可工作。
- 交互命令：`list [pattern]`（检索分页展示）、`info <module::name>`（类型/地址扩展/单位/支持状态）、`add/remove <module::name>`、`watch`（开始轮询）、`stop`（停止轮询）、`read <module::name>`（单次读取）、`write <module::name> <physical-value>`（写入确认及回读）、`quit`。实际命令拼写可在文档审定后固定；名称以 `module::name` 为优先，裸名仅在全库唯一时接受。
- `list` 只显示 A2L 目录信息，不读取 Slave，也没有变量值刷新周期；区分“存在于 A2L”“本工具可轮询的标量”“A2L 标记可写的标量候选”“暂不支持的数组/曲线/结构体”。支持显式完整列举 A2L 可检索符号；普通展示应分页或限量，避免超大 A2L 一次刷屏；不得因 `Search` 默认上限 200 而无提示地声称展示了全部变量。**A2L 可检索不等于 ECU 当前可读取/写入**。
- `watch` 开始后立即执行一轮所选变量的读取，之后按 `--interval-ms` 指定的间隔逐轮读取并追加带时间的结果行；同一轮的变量依次读取，实际周期可能受响应时间拖慢。主线程持续接受输入，用户键入 `stop` 并按 Enter 才触发停止；待在途 XCP 命令结束或超时后恢复稳定提示符。输出需串行化；普通终端中后台结果可能插入尚未提交的输入行，不承诺像分区 TUI 一样无扰重绘。

## 4. 组件与数据流

1. **CLI/目录组件**：解析参数并调用 `A2lBridge::Load`；`Count/Search/Find` 建立浏览视图。保留桥接对象和只读数据库视图直到工具退出。
2. **连接组件**：以 `UdpTransportConfig` 创建 `UdpTransport`，由 `XcpMaster` 持有并 `Connect()`；按 Slave 返回的会话参数获取 AG/字节序/帧上限，必要时查询资源状态。不从 A2L 字段覆盖 Slave 运行时真值；不支持的配置显式拒绝。
3. **观测组件**：所选符号先 `Find`；初版只对已证实地址、元素宽、标量布局与换算可执行者进行读取。调用 `ReadMemoryBytes(address, extension, byte_count)` 读取 raw，再由 `ToPhysical(name, raw)` 换算，打印时间戳、规范名、物理值和单位；本地时间为展示时间，**不宣称是 ECU DAQ 时间戳**。轮询按间隔运行，记录单项失败和过慢周期；发生失败时该项显示错误，绝不沿用旧值冒充新样本。
4. **修改组件**：只接受 `kind=Characteristic`、`characteristic_type=Value`、`dimensions` 为空、`read_write=true` 且能够按公开接口正/逆换算的标量；对于位掩码、复杂记录布局、无法独立整字段写回的对象保守拒绝，不写未知相邻字节。输入物理值要做严格解析、A2L 限值/转换可逆性校验；`FromPhysical` 得 raw，检查与运行时 AG 对齐及可访问性；写前展示符号与当前值，要求人工确认；调用 `WriteMemoryBytes` 后重新读并 `ToPhysical` 展示实际值。涉及浮点的比较采用明确的类型/精度策略，不以字符串相等作为正确性判断。写入失败或读回失败需分别报告；本阶段不做自动恢复旧值，避免擅自再次写入。
5. **并发/退出**：同一 Master 的读写与轮询串行化；交互输入可以保持响应，但一切 XCP 命令有互斥或单执行队列，写入期间暂停当前符号的轮询，防止交错。`stop/quit` 通知采集停止、等待在途命令结束并 `Disconnect()`；终端输入关闭时走同一路径。线程生命期与输出同步须在实现前详细核对，避免退出卡在阻塞输入。

## 5. 错误、安全和非目标

- A2L 加载/解析错误、名称歧义、缺少地址或宽度、类型/换算不支持、运行时字节序或地址粒度不兼容、超时、断线、XCP 权限拒绝分别呈现；业务分流依据结构化错误码，不解析错误消息文本。
- A2L `read_write` 只是**候选条件**，不是 Slave 对 CAL/PAG 写权限的保证。`ERR_ACCESS_LOCKED` 或同类拒绝不自动猜 Seed&Key 算法、也不自动重试写入；需要供应商密钥算法的 ECU 应提示当前工具不支持该步骤。
- 对超过 32 位 XCP 地址、宽度为零、尺寸溢出、数组/结构、位域和无法确认地址模式的符号，初版拒绝内存操作，不截断地址、不猜记录布局；打印拒绝原因。
- 非目标：TCP/CAN、DAQ、自动生成/上传 A2L、对 Slave 进程的管理、XCPlite 专属 profile、数组/曲线/结构体读写、标定页切换、自动 Seed&Key 解锁、无确认批量写入。工具不保证任意合法 A2L 符号都可执行内存访问。

## 6. 测试与验收（已选方案 A）

- 自动化测试放入 `tests/`：CLI 必填项及错误参数；A2L 加载成功/失败和含重复裸名的符号查找；分页/限量提示与支持状态分类；标量 raw→物理值显示；选中/移除后轮询调用与读失败不沿用旧值；写入前对只读、数组/结构、位域、逆换算失败、地址越界、AG 不对齐的拒绝；确认取消不发写命令；成功写入、读回，以及写拒绝/读回失败的明确区分；退出/断线资源释放。
- 使用测试目录的合成 A2L 与可控的模拟 Transport/UDP 测试对端；新工具测试不需要 `LIBXCP_BUILD_XCPLITE_SLAVE`，不依赖真实硬件，不引用 `examples/` 文件。
- Release 验证：启用新工具和测试，先按仓库约定清理再完整构建，运行工具相关测试及受影响回归；另以工具选项 `OFF` 验证既有构建门不发生 XCPlite 依赖变化。实际构建命令视生成器与平台调整，不在计划阶段声称通过。
- 可选真实设备手工验收：用户提供兼容 UDP 的 Slave、匹配 A2L、地址/端口及允许修改的安全标定量；演示加载、浏览、选择观测、手动确认写入、读回、必要时由用户决定是否恢复原值。没有真实 Slave 时仅标记此项“未执行”，不能把模拟测试当成真实设备测试。
- 文档和代码符合 `.clang-format`、中文 Doxygen 注释、跨平台 C++20；每次实现改动后补充 `code-plan/` 的变更与真实测试结果。

## 7. 实施步骤与批准边界

1. 冻结参数、交互命令和符号资格规则；复核现有接口对位域、地址模式、数据大小与写权限的真实契约。若发现某路径无法安全通用实现，先向用户说明并缩小范围，**不静默绕过**。
2. 建立独立 `tools/` 目标和启动入口；验证 `LIBXCP_BUILD_TOOLS=ON` 不触发 XCPlite，`OFF` 不改变既有目标。
3. 完成 A2L 目录浏览、符号能力标注与 UDP 连接。
4. 完成标量单读/定时轮询、交互选择和可靠停止。
5. 完成标量 `CHARACTERISTIC` 写前校验、人工确认、写入及回读报告。
6. 增加 `tests/` 自动化、README 和实施记录；Release 构建/回归、可选真实设备人工验收。

**审批状态**：完整功能范围已由用户批准；实现采用 500 ms 轮询默认间隔及本文所列命令。以下是实际实施状态。

## 8. 实施记录

### 变更批次 1：独立工具骨架与 CLI 功能

- 根 CMake 新增默认关闭的 `LIBXCP_BUILD_TOOLS`，启用时仅添加 `tools/`；目标 `xcp_master` 链接通用 `libxcp` 与 A2L bridge，不引用 `examples/` 或 XCPlite。生成后复制 A2L bridge 运行时库到工具目录。
- 新增 `tools/xcp_master/` 下的 UDP CLI、命令文档及保守符号资格策略。支持加载 A2L、列举/检索/分页、info、选取及移除轮询变量、read/watch/stop、标量 VALUE 标定写入确认及写后回读。地址越界、未知宽度、数组、结构和位域拒绝操作；显示 A2L 写标志仅为候选，写入前仍提示并须用户确认。
- 新增 CLI 参数 smoke tests，以及 `tests/xcp_master_symbol_policy_test.cpp` 对读/写分类和危险布局拒绝的自动化测试；策略实现与 CLI 共用 `tools/xcp_master/symbol_policy.hpp`。整数写入输入使用 `from_chars` 保留有符号/无符号 64 位物理整数精度，带小数或指数的值才解析为有限 `double`。
- 本批次涉及根 `CMakeLists.txt`、`tools/`、`tests/CMakeLists.txt`、新测试源及本实施记录。未修改 `thirdparty/` 或 `examples/`。

### 验证记录

- 配置命令：`cmake -S . -B cmake-build-release -DLIBXCP_BUILD_TOOLS=ON -DLIBXCP_BUILD_EXAMPLES=OFF -DLIBXCP_BUILD_TESTS=ON -DLIBXCP_BUILD_XCPLITE_SLAVE=OFF`，此前已成功配置。
- Release 工具目标构建：`cmake --build cmake-build-release --config Release --target xcp_master` 成功，生成 `cmake-build-release/tools/xcp_master/Release/xcp_master.exe`。首次编译发现会话参数实际位于 `session.connect` 下，已修正访问路径后重建通过。随后 `cmake --build cmake-build-release --config Release` 完整构建所有已配置目标成功；`--help` 输出正常。
- 自动化验证：定向工具测试 6/6 通过。之后运行全套 `ctest --test-dir cmake-build-release -C Release --output-on-failure`：484/484 通过，含工具策略和 CLI smoke tests；其中 1 个已有 AG 参数化用例按测试设计标记 Skipped，其余全部 Passed。第一次全套 CTest 在尚未构建其他测试可执行文件时执行，曾显示 15 项 Not Run；补齐 Release 全量构建后重跑，全套通过。
- `LIBXCP_BUILD_TOOLS=OFF` 独立配置（`cmake-build-tools-off`）成功，且生成目录中没有 `tools/` 构建目标；该配置同时显式关闭 examples、tests 与 XCPlite。真实 ECU 手工验收未执行。

## 9. 文档自检

- 目标落点仅 `tools/`；没有把旧 `examples/` 的源码或 XCPlite/cpp_demo 引入新工具。
- “通用”限定为外部 A2L + 可配置端点 + 符合本工具能力边界的 UDP Slave；不误称支持所有传输/DAQ/符号类型。
- 自动化测试是必需项；真实设备手工验收是可选项且结果需如实记录。
- A2L 声明和运行时能力分开验证；所有写操作须显式确认，不宣称多块写回原子性。
- 没有使用未核证的调用签名、硬编码 ECU 地址、特定 Slave 端口或绝对目录。
