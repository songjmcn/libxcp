# libxcp LOG 日志功能设计与实施计划

- 日期：2026-10-18
- 状态：已按用户批准方案实施；ON/OFF 配置、目标构建和回归结果见第 11 节。
- 设计确认与实施授权：设计分节已由用户逐项确认；用户于 m00122 明确授权“开始修改”。
- 实施边界：未修改 thirdparty/submodule；仅在 macOS 上验证，其他平台状态见第 11 节。
- 历史说明：初稿阶段仅编写计划、不修改代码的边界已由 m00122 后续授权取代；本文保留原设计与验收条件，并补充实际实施记录。

## 1. 目标与已确认决策

| 项目 | 设计决策 |
| --- | --- |
| 单独头文件 | 新增 `include/libxcp/log.hpp`，集中提供日志宏和后端适配 |
| 日志宏 | `LIBXCP_LOG_INFO`、`LIBXCP_LOG_WARNING`、`LIBXCP_LOG_ERROR` |
| 调用形式 | 函数式、`{}` 格式，例如 `LIBXCP_LOG_INFO("发送完成，长度={}", size)`；不是流式或 printf 格式 |
| CMake 开关 | `LIBXCP_ENABLE_LOG`，默认 OFF，显式 ON 启用 |
| 默认后端 | glog；不新增 submodule、不将下载源码纳入版本控制 |
| 获取策略 | 已有 `glog::glog` → 安装的 glog CONFIG 包 → FetchContent 固定版本按需下载 |
| 格式化 | 使用 C++20 `std::format`；启用日志时探测实际工具链，不支持则明确报错，不静默引入 fmt 等第二个格式化库 |
| 生命周期 | 宿主负责 glog 初始化、输出配置、刷新与关闭；核心不安装信号处理器、不增加公共初始化接口 |
| 自有程序输出 | tools、examples、tests 启用日志时统一配置 glog 仅写文件，INFO/WARNING/ERROR 均不输出终端；正常交互与测试报告保持不变 |
| 分级 | 协议详情 INFO；可恢复异常、能力降级 WARNING；最终失败 ERROR |
| 报文输出 | 普通报文十六进制最多前 64 字节，记录总长度、展示长度及截断标记 |
| 安全 | GET_SEED/UNLOCK 的 Seed、Key 脱敏；不能因上层脱敏而在 UDP 层再次泄露 |
| 测试 | 提供完整测试方案，ON/OFF 独立 Release 构建；三平台只报告实际验证结果 |

更换日志库时，业务调用点保持不变，包含、级别映射和输出适配集中在日志头文件。**更换库的获取、链接及安装依赖仍可能需要调整 CMake**，不承诺任意库只改头文件就能完成全部构建集成。

## 2. 项目现状与接入依据

本节来自实际文件阅读；行号以编写本文档时为准。

- `CMakeLists.txt:20-30`：最低 CMake 3.24，当前没有日志开关。
- `CMakeLists.txt:61-63`：主工程要求 C++20，并禁用编译器语言扩展。
- `CMakeLists.txt:77-119`：核心为静态库，已有线程库/Windows Socket 链接及 archive 安装规则。
- `CMakeLists.txt:120-121`：现有规则安装整个 `include/libxcp` 目录。
- `CMakeLists.txt:174-187`：A2L 是必编依赖；日志关闭不等于整个工程无网络/无第三方依赖。
- `tests/CMakeLists.txt:18-34`：现有 GoogleTest 使用 FetchContent，可参考依赖获取方式，但日志不能因测试开关间接开启。
- `include/libxcp/ixcp_transport.hpp:30-50`：接收回调拿到不含传输头的 XCP Packet；通道关闭和可恢复警告已有接口。
- `src/command_executor.cpp:125-215`：统一解析和分流 RES/ERR/EV/SERV/DTO，存在静默忽略和吞掉回调异常的路径。
- `src/command_executor.cpp:228-231`：`OnTransportWarning` 当前预留日志接入点，没有输出。
- `src/command_executor.cpp:358-369`、`src/command_executor.cpp:410-453`：普通发送和 SYNCH 发送分属两条路径，必须同时覆盖。
- `src/command_executor.cpp:456-531`：超时、恢复、有限重试和不可重放命令行为已经实现，不能为日志改动它们。
- `src/udp_transport.cpp:338-389`：发送成功需以 `sendto` 完整发送校验通过为准。
- `src/udp_transport.cpp:408-604`：接收包括来源校验、Datagram 解码、Frame 上限、CTR 校验和上层回调。
- `src/xcp_error.cpp:36-45`：异常构造只保存字段，不适合无差别加入 ERROR，否则可选能力降级也会误报。
- `src/xcp_master.cpp:130-255`：连接失败清理、析构和断连存在吞异常路径，应补上下文诊断而非改变清理行为。
- `src/measurement/measurement_session.cpp:404-410`：DTO 解码失败目前累计计数后继续，日志应保留该策略。
- `tests/recovery_test.cpp:1-24`：已有超时恢复和错误分派测试，可扩展日志断言并继续验证行为不变。
- 最近工作背景通过 `code-plan/README_构建策略同步_实施记录.md` 及当前 CMake 了解；未运行 git log。初始 `git diff --stat` 无已跟踪差异输出，未据此推断不存在未跟踪文件。

## 3. 架构与日志头文件

### 3.1 调用契约

三个宏均接收格式串和可选格式化参数，支持无参数普通消息、多个参数、`{{`/`}}` 转义及十六进制格式说明。

- 不在业务源码直接调用 glog 的 LOG、全局配置或初始化函数。
- 宏是单条语句语义，安全用于不带花括号的 if/else；参数只求值一次。
- 宏适配保留调用点的源文件和行号，而非统一显示内部辅助函数位置。
- 不新增 FATAL/DEBUG/TRACE 等级；ERROR 不终止进程。
- 对 uint8_t、枚举、BytesView 等数据，在调用处明确转换或使用内部报文摘要，避免格式化为字符或要求并不存在的 formatter。
- 新增宏及必要内部辅助函数使用中文 Doxygen 注释；按现有 `.clang-format` 排版。

### 3.2 OFF 行为

- 宏在预处理阶段消除，不求值格式串和任何参数，不分配字符串，不执行十六进制转换。
- 不包含 glog 或 `<format>`，不实例化格式化模板。
- 条件编译采用数值判断，不能用 `#ifdef LIBXCP_ENABLE_LOG` 误将值为 0 当作启用。
- 日志头单独使用且未获得 CMake 定义时默认关闭；同一目标及其使用者不得自行覆盖传播下来的开关值。
- OFF 测试必须证明具有副作用的参数计数保持为 0，而不仅是没有控制台输出。

### 3.3 ON 行为与异常隔离

- 统一在头文件适配层完成 `{}` 格式化及 INFO/WARNING/ERROR 到 glog 的映射。
- 必要的内部 inline/template 辅助实现同样位于该头文件，不新增日志管理器或公共函数接口。
- 日志宏内部异常保护必须覆盖参数求值、格式化和输出调用，不能只在参数已求值之后的辅助函数内捕获异常。
- 格式化或输出发生可捕获 C++ 异常时，不改变原协议执行或覆盖正在传播的异常；不得用递归日志报告日志自身失败。
- 不承诺捕获进程终止、系统崩溃、后端死锁等非 C++ 异常行为。
- 启用但运行时过滤掉某级别时，尽可能在格式化之前判断后端级别；OFF 的零求值是硬验收项。
- 格式化实现以真实编译测试确定模板和参数转发方式，不凭编译器版本号推断 `<format>` 可用。

## 4. CMake 与依赖管理

### 4.1 开关和传播

1. 顶层添加默认 OFF 的 `LIBXCP_ENABLE_LOG`。
2. 通过目标级编译定义将开关明确传播为 0/1，不使用全局 `add_definitions`。
3. ON 时检查已有 `glog::glog`，没有则查找 glog CONFIG 包，仍没有才进入 FetchContent。
4. 因日志头属于安装的头文件且 ON 时涉及后端头文件，日志定义和所需依赖按公共使用要求传播；静态库最终链接不能只验证 archive 成功。
5. `libxcp_measurement_adapter` 等通过链接核心获得一致配置，不给第三方目标全局注入日志宏。
6. OFF 必须完全跳过日志相关探测、查找、下载、链接；不清除先前 ON 构建缓存，而以全新目录验证无 glog 依赖。

### 4.2 glog 的按需获取

- 源码只落在构建目录的 FetchContent 管理区域；支持标准 `FETCHCONTENT_SOURCE_DIR_GLOG` 指向用户预置源码。
- 拟以 glog v0.7.1 为初始候选基线；实施第一步核实其可获取性、许可证、选项、目标名和三平台构建能力，再锁定不可变提交或归档 SHA256。未核实前不得声称已验证可用。
- 不跟随 main/master/latest；不能只写浮动分支。
- 下载的 glog 采用适合链接本项目静态库的配置，优先静态且支持 PIC；验证 Windows 导出/编译定义及运行库一致性。
- 在其支持的范围关闭 glog 自身测试、示例及不必要可选依赖；不全局 FORCE 修改父工程的 BUILD_TESTING、BUILD_SHARED_LIBS 等变量。
- 已存在的 glog 目标不重建、不更改其属性，缺失必需能力时明确诊断。
- 显式 ON 且下载失败应配置失败，给出安装包、离线源码或关闭开关的修复方式，不静默变成 OFF。
- 不修改 `thirdparty`、`.gitmodules`，不执行 submodule 操作。

### 4.3 std::format 兼容性

仅 ON 时执行 C++20 编译/链接探测，至少覆盖普通字符串、整型和十六进制格式。缺少 `<format>` 或符号时明确报错并指引使用支持该能力的标准库/工具链。

不默认 GCC/Clang/MSVC 某个版本必然通过；编译器与实际标准库组合才是验证单位。OFF 不因标准库缺少 format 而失败。

### 4.4 安装与消费者边界

- 现有安装规则会安装新增日志头文件，不需要另起一套头文件安装机制。
- 当前工程没有在上述安装规则中导出完整 CMake package，不能把新增日志描述为顺带提供完整 SDK 包管理。
- 增补 ON 时静态 archive 的 glog 链接依赖及配置说明；独立消费者测试明确提供所需 glog、线程库、平台系统库和开关定义。
- 验证构建树通过 `libxcp::libxcp` 传播依赖，以及安装后显式依赖配置的消费者，不能将手工 archive 链接误称为自动包发现。
- 不设置安装/构建绝对路径；不在本轮实际执行安装。

## 5. 日志生命周期与输出

- 外部宿主在启动 libxcp 相关线程/创建活跃连接之前初始化 glog，并自行决定 stderr、文件、目录、阈值、刷新策略；项目自有 tools、examples、tests 则必须遵守下述“仅文件输出”策略。
- 宿主在停止工作线程并销毁可能输出日志的 libxcp 对象之后，再关闭 glog。
- 核心不能修改进程级 glog flags，不能擅自创建日志目录、注册信号处理器或调用 ShutdownGoogleLogging。
- 宿主已有 glog 时直接共用其配置，不重复初始化。
- 后续在自有 tools、examples、tests 的进程入口统一落实初始化、文件输出配置与退出顺序，而不只是演示；这是后端接入边界的显式例外，不是鼓励业务源码直接使用 glog。
- 本轮不约定新的运行时日志配置 API，不加入滚动文件管理、异步队列或 GUI 日志窗口。
- 未初始化时不承诺 glog 的具体输出回退行为；使用说明要求按上述顺序初始化，真实行为实施时验证。

### 5.1 tools、examples、tests 的仅文件输出策略（补充需求）

适用于项目自有可执行程序，包括工具、示例、单元/集成测试及其自有测试辅助进程；目录名称分别为 tools、examples、tests。第三方程序的其他日志系统不在此次修改范围内，不为了统一输出修改 thirdparty。

- `LIBXCP_ENABLE_LOG=ON` 时，进程入口必须显式配置 glog 写文件，不依赖其默认目录与默认 stderr 阈值。INFO、WARNING、ERROR 全部不得复制到 stdout/stderr，尤其防止 ERROR 仍被默认 stderr 策略输出。
- 后续实施需依据实际 glog 版本核实文件输出与 stderr 控制接口。重点检查 `logtostderr=false`、`alsologtostderr=false`、`stderrthreshold` 高于本项目最高 ERROR 等配置的有效性；不能只设置 log_dir 就声称完成。具体符号、命名空间及行为须编译和子进程测试确认。
- 此要求针对 glog 日志，不是禁用整个终端：CLI 帮助、参数错误、工具 TUI、示例业务结果、GoogleTest/CTest 测试报告仍按原方式输出；不得通过重定向整个 stdout/stderr 或关闭终端句柄达到“无日志上屏”。
- 首条业务日志前完成目录准备、文件输出策略及初始化；所有工作线程退出、可能输出日志的对象销毁后刷新文件，再关闭 glog。初始化只能在进程级执行一次，不在每个测试用例或连接中反复初始化。
- 拟定默认目录为相对当前工作目录的 `logs/<模块>/<程序名>/`，模块取 tools、examples 或 tests；这是方案建议，实施时结合实际入口和工作目录确认。文件名由后端包含进程标识/时间等区分信息，避免并行测试和子进程覆盖；不硬编码主机绝对路径。
- 测试夹具应为每次运行指定隔离的可写日志目录，并将实际路径纳入失败报告，不能让并行 CTest 实例依赖同一固定文件名。
- 日志目录不可创建或不可写时，不静默回退为 terminal 日志。自有程序在业务启动前返回明确失败，允许用普通启动错误诊断说明目录和原因，但不得把三级 glog 消息转向终端；测试须区分启动诊断与 glog 日志。
- 文件系统在运行中失效时，检查所选 glog 是否会自行向 stderr 输出内部诊断或回退。若无法保证，应明确记录后端限制并在实施前提出处置方案，不能声称已实现所有故障下终端绝对静默。
- OFF 时不创建日志目录、不初始化/关闭 glog，保留各程序原有行为。

### 5.2 入口覆盖与复用

- 实施时先盘点三类目录的全部自有可执行目标及实际入口，逐一登记覆盖状态；不以只改一个 demo 或主测试目标代替完成。
- 多个 GoogleTest 目标当前使用 gtest_main 的情形，需显式规划公共自有测试 main 或等价的进程级初始化入口，调整相应链接以避免重复 main；不能假定 GoogleTest 自动配置 glog。
- 兼顾 gtest_discover_tests 运行可执行文件的发现阶段：枚举测试不应启动业务线程；原则上发现模式不初始化日志、不创建文件，正常执行模式在运行用例前初始化。
- 对测试辅助进程及独立示例，保证各进程自行初始化，不能认为父进程的 glog 配置自动作用于新进程；CTest 和子进程启动时明确工作目录/日志目录。
- 允许增加项目内部复用的程序启动辅助代码或测试入口文件，限日志配置用途，不增加 libxcp 核心公共接口、不重构业务流程；所需修改纳入 tools、examples、tests 对应 CMake 与源文件清单。
- 外部宿主仍自主控制输出去向；核心库不因自有程序的默认策略而全局修改宿主 glog flags。

## 6. 数据流、输出内容与安全

### 6.1 发送

- `CommandExecutor::PerformAttempt`：Transport.Send 成功返回后输出协议 INFO，包含 TX、命令名称/数值、Packet 长度和安全摘要。
- `CommandExecutor::SendSynch`：同样在实际发送后记录 SYNCH，避免漏掉恢复流量。
- `UdpTransport::Send`：完整发送确认后记录 UDP 元信息，包括远端端点、LEN、CTR；不重复输出 XCP 原始载荷。
- 发送失败只记录失败原因，不输出“已发送成功”。异步 RX 可能先于发送线程写出 TX，不将跨线程日志显示顺序解释为网络因果顺序。

### 6.2 接收

- UDP 层在解码、校验阶段记录端点、Frame LEN/CTR 和接收/丢弃结果，不转储原始 Datagram 载荷。
- 执行器统一输出协议 RX INFO，分类 RES、ERR、EV、SERV、DTO，并输出 PID、长度和安全摘要；有真实命令上下文时附命令。
- DTO、EV、SERV 是异步包，不把当前 pending command 硬贴成这些包的所属命令。
- RES/ERR 没有待处理命令时标为上下文未知，不把代码当前用于解析的 GetStatus 默认值当作实际请求。
- parser 未就绪、空包、畸形包及额外响应增加 WARNING，保留现有忽略/丢弃策略，不为了输出字段越界读取。
- 字节视图只在有效生命周期内同步读取，不存储指向回调数据的悬空引用。

### 6.3 摘要与脱敏

- 普通载荷最多展示前 64 字节，另记原始长度、展示长度及 truncated=true/false。
- UNLOCK 发送 Key 和 GET_SEED 响应 Seed 不输出字节；只记命令、长度、方向和 redacted 标记。
- GET_SEED 请求及 UNLOCK 响应采用一致的保守摘要规则，避免分段、多包或错误上下文导致泄漏。
- 无法可靠判断上下文的 RES/ERR 不转储响应体，只输出 PID、长度、分类及上下文未知标记；迟到包、恢复阶段和清理边界做专门测试。
- 安全优先：若现有状态无法可靠排除迟到 Seed 响应，应扩大不转储范围，不能靠猜测命令归属来满足普通报文展示。
- UDP 层绝不转储原始载荷，从而不能绕过协议层脱敏。
- 不输出 SeedKeyCalculator 的输入输出或潜在含 Key 的自定义诊断内容。
- 后端自身格式外，消息保持稳定的方向、命令、长度、错误等关键字段，便于测试；不依赖完整时间戳文本做断言。

### 6.4 实时性边界

开启 INFO 会给高频 DTO 带来格式化和 I/O 开销；本次不增加第四级别，也不悄悄采样省略协议日志。生产环境可关闭编译开关或由宿主调整阈值。

尽量在现有锁外输出，锁内只获取已有状态的快照；不改锁顺序、不新增跨层回调或为日志重构接收流程。若具体位置无法在不改变行为的情况下移出锁，需在实施记录标明锁内日志点并做并发验证。后端 sink 不得回调 libxcp，不能承诺同步日志满足硬实时要求。

## 7. 错误分级与布点范围

| 场景 | 级别和原则 |
| --- | --- |
| 正常 TX/RX、协商结果、恢复成功 | INFO；协议详情始终 INFO |
| 原始 ERR 报文 | INFO 记录协议事实，最终处理结论再按下列规则分级 |
| 可选命令不支持、SHORT_UPLOAD 等降级 | WARNING，不提前误记最终 ERROR |
| 畸形包、来源过滤、CTR 缺口/重复/乱序 | WARNING，保持原过滤/计数/通知行为 |
| 首次超时进入恢复、可继续的回调失败 | WARNING，含命令、阶段、重试次数和原因 |
| 传输故障、不可恢复解析错误、重试耗尽 | ERROR，保留既有异常及上下文字段 |
| 标定变更执行结果未知 | ERROR，明确“结果未知、未重放”，不能误写成已失败或已回滚 |
| 无效参数/状态导致操作被拒绝 | ERROR；由最直接拥有诊断信息的位置记录 |
| 正常主动断连和接收轮询超时 | 正常关闭可 INFO，轮询超时不记故障 |
| 析构/清理吞掉的异常 | 尽力记录 WARNING，不抛新异常、不阻止清理 |
| DTO 解码失败后丢帧继续 | WARNING，同时保留 decode_errors 计数 |

布点策略：

1. 首批重点为 `src/command_executor.cpp`、`src/udp_transport.cpp`：协议全收发、已有 warning、失败/恢复决策和吞异常位置。
2. 补充 `src/session.cpp`、`src/memory_access.cpp`、`src/xcp_master.cpp`、`src/command_codec.cpp`、`src/udp_header_codec.cpp` 中的错误/降级关键路径，不给每条普通 getter 增加噪声。
3. 检查 `src/response_parser.cpp`、`src/daq/`、`src/measurement/` 的失败及丢帧路径：有上层决策位置时由上层记录，独立调用没有上层覆盖的错误则就地记录。
4. 不在 XcpException 构造或全部 Make* 工厂中统一插日志，不为日志重构错误模型；纯抛出/原样重抛不逐层重复 ERROR，增加新决策/清理上下文时才再记。
5. 错误布点审核按“产生错误 → 处理决策 → 是否继续/上抛”逐条登记，尤其检查仍被静默吞掉的 catch 和返回 nullopt 的失败。
6. 本次不全面替换工具的 stdout/stderr 用户交互，也不改第三方 A2L/XCPlite 自带日志系统；适配器自身错误仅在确有核心未覆盖的路径上补充。

## 8. 拟变更清单与实施批次

以下表格记录已批准的实施范围；完成情况和实际测试结果见第 11 节。

| 批次 | 范围 | 完成条件 |
| --- | --- | --- |
| 1 | 新增日志头；顶层 CMake 接入；依赖版本核实和 format 探测 | ON/OFF 能按契约配置，OFF 不触发 glog |
| 2 | 宏契约测试；tools、examples、tests 全部自有进程入口及其 CMake 的日志初始化/关闭接入 | 三级、格式、源位置、异常隔离、零求值通过；ON 三级只写文件且不上屏，OFF 不创建日志目录 |
| 3 | 执行器、UDP 收发/错误布点和摘要 | 普通与 SYNCH 发送、全部 RX 分类、丢包原因可见且脱敏 |
| 4 | Session、内存、门面及解码/测量错误路径补齐 | 按错误归属清单验收，不改变异常/恢复/计数语义 |
| 5 | 回归、消费者、文档说明和实施记录 | 记录真实结果与未验证平台，不将计划当作结果 |

实际新增测试文件：`tests/logging_test.cpp`、`tests/logging_probe.cpp`、`tests/logging_behavior_test.cmake` 和自有 GoogleTest 入口 `tests/test_main.cpp`；在 `tests/CMakeLists.txt` 注册宏契约和文件输出/不可写目录/发现模式测试，并扩展协议探针覆盖迟到 Seed。复用既有 Session、Command、Recovery、Memory、UDP 与测量测试。

文档更新：`README.md` 说明开关、依赖、宏示例、宿主初始化、tools/examples/tests 仅文件输出策略、日志目录和静态库消费者链接；本文件第 11 节记录实现范围与真实测试结果。未新增业务公共初始化接口、未修改第三方代码。

补充拟变更范围：tools、examples、tests 各自的可执行入口、必要的共用日志启动辅助代码及对应 CMake；测试中使用 glog 的目标需保证进程级初始化和文件刷新。具体入口清单实施前通过实际目标/源文件盘点确定，不在未读取文件前杜撰文件名。

## 9. 完整测试方案

### 9.1 宏单元测试

- INFO/WARNING/ERROR 映射正确；ERROR 不终止进程。
- 无参数消息、多参数、空字符串、转义花括号、十六进制、中文、常用整数宽度均正确。
- 调用点源文件和行号正确；消息为一条完整记录，不混入其他线程片段。
- if/else 语句安全，每个参数只求值一次。
- OFF：副作用参数、昂贵摘要以及会抛异常的参数全部不执行；头文件不依赖 glog/format。
- ON：有效的自定义 formatter/参数求值抛出运行时异常时，不影响后续协议逻辑；编译期格式检查失败与运行时输出失败区分测试，不要求非法常量格式必须能编译。
- glog sink 只用于测试捕获，不新增生产日志查询 API；多线程 sink 用互斥保护且不重入库。

### 9.2 构建与依赖矩阵

| 配置 | 验证点 |
| --- | --- |
| 新目录 OFF，测试关闭 | 无 glog 查找/下载/链接，也不要求 format；不混淆 A2L 等既有依赖 |
| 新目录 OFF，测试开启 | 原协议测试能工作，独立 disabled 宏测试无日志依赖 |
| ON，父工程提供 glog::glog | 复用、不下载、不篡改已有目标 |
| ON，已安装 glog | CONFIG 包解析及最终可执行文件链接成功 |
| ON，无 glog | 固定来源按需下载构建，版本/校验固定 |
| ON，离线预置源码 | 标准 FetchContent 源路径覆盖有效 |
| ON，下载不可达 | 明确失败提示，不静默关日志 |
| ON，不支持 std::format | 配置阶段明确失败；OFF 对同一工具链不执行该探测 |
| 同一构建目录 ON→OFF | 定义/链接不残留；以最终链接命令检查，不要求删除缓存源码 |
| 静态库独立消费 | 构建树目标传播、安装头文件和显式 archive 依赖链接可用 |

Windows/MSVC、Linux、macOS 分别运行 Release；缺少环境则记录“未验证”，不能类推其他平台已经通过。

### 9.3 协议与错误集成测试

1. Mock Transport 验证正常发送成功后才出现 TX；Send 抛异常时只有失败日志，没有成功日志。
2. 普通命令、SYNCH、重试、恢复 MTA/DAQ 指针所产生的发送均有日志。
3. RES、ERR、EV、SERV、DTO 五类 RX 日志可区分；异步包没有伪造的 pending command 归属。
4. 空包、parser 未就绪、畸形包、响应槽冲突可诊断，现有忽略策略不变。
5. UDP 回环检查 LEN/CTR、非配置来源、超长/畸形 Datagram、重复/乱序/缺口以及正常关闭与轮询超时区分。
6. 超时恢复成功、重试耗尽、未知错误码和可选能力降级分别断言级别及原异常字段。
7. DOWNLOAD/MODIFY_BITS 等结果未知场景仍不重放，并输出“结果未知”结论。
8. GET_SEED/UNLOCK 多包交互及迟到/未知上下文下，用可识别的测试 Seed/Key 扫描所有捕获日志，确认协议层和 UDP 层均不泄露。
9. 普通报文超过 64 字节时，摘要只含前 64 字节且标明截断；短包边界无越界。
10. 多连接并发、接收线程/测量线程及清理异常场景不死锁、不因日志改变回调、丢帧和计数行为。
11. 原始 ERR 可有 INFO 记录，但降级结论不能出现虚假的最终 ERROR；同一次失败的纯重抛不多层重复 ERROR。

### 9.4 回归与运行纪律

- 优先运行宏专项、recovery、seed_key、UDP、Session、内存、DAQ、measurement 相关测试，再扩展到既有核心测试集。
- XCPlite 等依赖环境具备且开关开启时执行相应集成回归；不能因未启用而声称通过。
- 使用独立的 `cmake-build-log-off` / `cmake-build-log-on` Release 目录隔离配置；完整编译前先执行对应构建目录的 clean，新目录先配置生成再 clean。
- 不使用 Debug 编译，不运行 `install/Debug/bin` 全部测试，不更改代理/系统配置来掩盖依赖失败。
- 耗时可记录 ON/OFF 对比作为参考，不在没有基线时捏造性能阈值或硬实时保证。
- 安装消费者优先用隔离暂存前缀验证，明确审批后实施；不污染系统安装目录。

### 9.5 自有程序文件输出专项测试

- 对 tools、examples、tests 的自有进程入口建立覆盖清单；可复用启动辅助代码的单元测试，但三类模块均需实际子进程冒烟验证，不能只用内存 sink 代替落盘测试。
- ON：在隔离可写目录启动，触发带唯一标识的 INFO/WARNING/ERROR，退出前刷新并检查文件，三级消息均存在；分别捕获 stdout/stderr，确认均不含这些日志消息及其 glog 前缀。
- 不能以关闭某级别或 `minloglevel` 过滤消息通过测试：验证“写入文件”和“不出现在终端”必须同时成立；尤其单独覆盖 ERROR 的 stderr 阈值。
- 工具帮助/TUI、示例结果、GoogleTest 成败摘要与退出码保持原样；故意失败的测试仍按原方式报告，不因抑制日志而隐藏测试失败。
- OFF：相同程序不依赖 glog、不创建日志目录，终端业务输出与已有行为一致。
- 覆盖并行测试、多进程以及测试辅助进程，文件互不覆盖、进程各自初始化，退出后日志可读。
- 验证测试发现模式、正常用例执行模式、提前退出及异常清理路径的初始化/刷新/关闭顺序。
- 不可写目录：业务不启动、不回退为终端日志；普通启动错误诊断可见且退出码明确。运行中写盘失败的后端回退/内部诊断行为需实际观察并报告限制。
- Windows/Linux/macOS 分别核查目录创建、文件命名、权限错误及输出捕获，仅报告实际执行的平台结果。

## 10. 验收标准

- 三个指定函数式宏全部可用，日志后端适配集中在单独头文件。
- `LIBXCP_ENABLE_LOG=OFF` 时不获取/依赖 glog，不求值日志参数；ON 的依赖与格式化不支持时有明确诊断。
- glog 非 submodule，首次缺失时按需获取且来源固定，可复用已有目标、安装包和离线源码。
- 正常/恢复发送及协议五类接收都有 INFO 详情，错误可诊断，不改变异常、超时、恢复、重试及计数语义。
- Seed/Key 不从任何日志层泄露；普通报文摘要遵守 64 字节上限。
- 宿主控制日志生命周期，库不接管进程级日志策略；tools、examples、tests 自有程序作为宿主，ON 时显式配置三级 glog 日志仅写文件、不输出 terminal，正常交互/测试报告不受影响。
- 自有程序的入口覆盖、文件落盘、stdout/stderr 无日志、OFF 无目录副作用及不可写路径均有专项验证。
- 完整记录真实编译/测试结果、平台范围和未通过/未验证项。

## 11. 实施记录、测试结果与未验证项

### 11.1 已实施内容

- 新增 `include/libxcp/log.hpp`，提供 `LIBXCP_LOG_INFO`、`LIBXCP_LOG_WARNING`、`LIBXCP_LOG_ERROR` 函数式宏；启用时用 C++20 `std::format` 和 glog 写入，catch-all 隔离格式化/写日志异常；关闭时宏不展开参数、不包含 `<format>` 或 glog。
- `CMakeLists.txt` 增加默认 OFF 的 `LIBXCP_ENABLE_LOG`；ON 时实测 `<format>`、要求 glog 0.7.1+，优先复用目标/CONFIG 包，否则以固定 URL 和 SHA256 `c17d85c03ad9630006ef32c7be7c65656aba2e7e2fbfc82226b7e680c771fc88` FetchContent 获取 v0.7.1；版本缺失或低于 0.7.1 时配置失败。核心静态库 PUBLIC 传播宏与 glog 依赖。
- 新增内部 `src/program_logging.hpp/.cpp`，为自有进程建 `logs/<模块>/<程序名>/`，在初始化前实际测试文件写入权限，配置 glog 只写文件，关闭 INFO/WARNING/ERROR 的 stderr 回显；日志关闭时不初始化、不建目录。自有 GoogleTest main 在 discovery 模式跳过初始化。
- 修复 glog 文件名以点开头的缺陷：glog 保存 `InitGoogleLogging(argv0)` 中 basename 的指针，不能传入会在初始化函数结束时销毁的局部字符串；`ProgramLogSession`现将 argv0 保存在生命周期覆盖 glog 的成员中，并显式配置每级日志文件名为 `<程序名>.<级别>.<时间>.<PID>.glog`，关闭默认无后缀软链接。
- tools、四个自有 examples、A2lSmoke、GoogleTest 入口均接入文件日志辅助代码；原有 CLI、示例结果、测试报告终端输出保持。第三方 `cpp_demo` 和 `xcp_test_slave` 未改。
- `src/command_executor.cpp`、`src/udp_transport.cpp`覆盖普通/SYNCH TX、RES/ERR/EV/SERV/DTO RX、传输 warning/close、错误与恢复决策；普通摘要最多64字节并标记截断。由于XCP无响应事务ID，RES/ERR正文一律脱敏，避免迟到Seed被错配到其他pending命令时泄露；Seed/Key的TX/RX数据不输出，UDP层从不输出payload。
- `src/session.cpp`、`src/command_codec.cpp`、`src/memory_access.cpp`、`src/xcp_master.cpp`、`src/measurement/measurement_session.cpp`补充状态/参数拒绝、可选能力降级、清理吞异常和DTO丢帧等日志；保持既有错误传播、恢复、重试、计数和清理流程，不在异常工厂统一记录。
- 新增日志宏/文件输出/不可写目录/发现模式/非法CLI隔离测试；更新 `README.md` 与 `docs/modules/构建系统.md` 的开关、依赖、使用、生命周期、目录与静态库消费说明。

### 11.2 实际验证

- 实际环境：macOS；Apple clang 21.0.0、CMake 4.2.3；C++20 `<format>` 探测通过。Windows、Linux 未验证。
- 最终ON与OFF Release CMake配置均成功。ON按需获取并编译 glog v0.7.1；本机预装 glog 0.5.0 不满足生命周期 API，正确回退到固定版本源码。
- ON/OFF 对应的核心、日志探针、自有工具/示例及测试目标增量构建成功；ON目标包括 `libxcp_logging_probe`、`libxcp_tests`、`A2lSmoke`、`xcp_master`、`xcp_master_udp`、`measurement_demo`、`xcp_integration_demo`、`xcp_slave_demo`。OFF目标构建也成功。
- ON全量 clean Release build 和 OFF全量 build均被现存 `tests/measurement_adapter_test.cpp:56` 的 `FakeA2l` 抽象类编译错误阻塞：未实现第三方接口新增纯虚 `ToPhysicalElement`、`FromPhysicalElement`（`thirdparty/a2l-sdk/a2lbridge/include/libxcp/a2l/ia2l_database.hpp:79,98`）。未修改该无关测试或 thirdparty。
- 执行 `ctest --test-dir cmake-build-logging-release --output-on-failure -R 'LoggingMacrosTest|LibxcpLog|ExampleXcpIntegrationDemoInvalidCliLogIsolation|Session|Command|Recovery|Memory'`：**200/200 通过，6.78秒**。覆盖协议/Session/Memory/recovery/UDP/测量及日志专项。
- OFF 构建执行 `ctest --test-dir cmake-build-release --output-on-failure -R 'LoggingMacrosTest'`：**1/1 通过**，确认宏参数不求值。
- 日志专项验证：三等级落盘且 stdout/stderr 只有程序自身标记；Seed `A5 5A`、Key `C1 5E`、迟到Seed响应正文不泄露；DTO摘要限64字节；不可写目录失败不回退终端；`--gtest_list_tests`不建目录；非法CLI不触发 glog 初始化前 stderr 警告。
- 文件名回归：`LibxcpLogFileOutput`额外断言日志文件不以点开头、名称包含程序名和级别且以`.glog`结尾；实测生成`libxcp_logging_probe.INFO.20261020-003200.17079.glog`等INFO/WARNING/ERROR文件。修复后ON日志专项6/6通过，`xcp_master` Release目标重建成功。
- `git diff --check`通过；新增文件及修改区按 `.clang-format` 局部检查/格式化。整文件格式检查会命中多个既有基线格式问题，因此未对长文件全量重排。

### 11.3 尚未验证

- 未在 Windows/Linux 构建；未验证已安装 glog 0.7.1 包、父工程预建 target、`FETCHCONTENT_SOURCE_DIR_GLOG` 本地替代源码及安装静态库消费工程。
- 全量构建仍受上述 FakeA2l 第三方接口变化与旧测试桩不兼容影响；该阻塞与日志改动无关，日志相关自有目标和选定回归测试均已单独构建/运行。
