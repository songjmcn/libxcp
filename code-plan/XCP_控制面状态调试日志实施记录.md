# XCP 控制面状态调试日志：设计与实施记录

## 背景与范围

用户指出现有协议收发日志不足以定位内部状态变化，选择“控制面状态”范围，并批准实施与测试。沿用 `LIBXCP_ENABLE_LOG` 和既有 `LIBXCP_LOG_INFO/WARNING/ERROR`；不增加日志等级、公开接口或第三方依赖。高频 DTO 保持原有逐包收发摘要，只在既有异常/丢弃路径记录原因，不为每个样本另记入队、解码、回调日志。

## 架构与布点

- `src/session.cpp`：记录成功的连接、断连、恢复、失败与重置状态迁移；连接成功只记录协商上限等元信息；Pending 命令设置/清除记录命令号及状态。失败原因可能包含外部输入，日志只记状态和资源清理结果。状态日志在现有互斥区内读取当前快照，以免并发操作后误报顺序；不增加新锁或改变状态操作。
- `src/command_executor.cpp`：记录 RES/ERR 与异步 EV/SERV 的分流、响应槽位、等待开始/结果、EV_CMD_PENDING 计时器重启以及 SYNCH 恢复确认。遵循现有时序与原分支，不重新关联迟到响应，不把异步消息记作当前命令的响应。
- `src/xcp_master.cpp`：记录连接/断连、DAQ 清单配置与启停、动态 DAQ 生命周期及能力降级的控制面阶段；`src/memory_access.cpp` 记录状态拒绝/不可重放操作结果未知等决策。协议实际 TX/RX 摘要仍由原日志负责。
- 级别：正常状态/协议决策 INFO；可恢复的异常、能力降级 WARNING；最终失败或拒绝 ERROR。日志调用继续由宏隔离异常，OFF 不执行日志参数。

## 安全与性能边界

- 不记录 Seed/Key、RES/ERR 响应正文、内存读写载荷、DTO 样本、可由外部控制的失败详情；只使用状态枚举、命令编号、超时毫秒数、DAQ 编号/计数等安全元数据。
- 不改变传输、超时、重试、DAQ 帐本、计数器和错误传播语义。部分 Session 状态变更日志处于原有锁内，可能带来 ON 时额外 I/O 延迟；只在低频状态边界记录，不在高频 DTO 数据路径增加控制日志。核心不初始化 glog，宿主原有文件日志策略保持。

## 测试与结果

- 黑盒文件日志探针覆盖连接成功、Pending 设置/清除、RES/EV/SERV 分流、响应槽位、64 字节 DTO 上限与 Seed/Key 脱敏，拒绝日志污染 stderr/stdout；新增恢复黑盒检查 Connected → Recovering → Connected、SYNCH 确认及 EV_CMD_PENDING 重启计时器且不重发。
- 回归选择 Session、Recovery、EventCmdPending、EventDispatch、DAQ、内存、Seed/Key 等现有测试；OFF 使用独立 Release 构建验证宏零求值和核心/测试编译。
- macOS / AppleClang 21、CMake 4.2.3 实测：`cmake-build-logging-release`（Release、LOG=ON）重新配置及 `libxcp_tests`、`libxcp_logging_probe` 增量编译通过；新 `LibxcpLogControlRecovery` 黑盒测试通过，扩展 `LibxcpLogFileOutput` 黑盒测试通过。`ctest --test-dir cmake-build-logging-release --output-on-failure -R 'LibxcpLog|LoggingMacrosTest|Session|Recovery|EventCmdPending|EventDispatch|Daq|Memory|SeedKey'`：**245/245 通过**。
- `cmake-build-control-log-off-release`（Release、LOG=OFF）独立配置成功，`libxcp_tests` 目标构建成功；`ctest --test-dir cmake-build-control-log-off-release --output-on-failure -R 'LoggingMacrosTest|Session|Recovery|EventCmdPending|EventDispatch|XcpDaq|Memory|SeedKey'`：**184/184 通过**。最初配置缺 `Boost_USE_STATIC_LIBS=ON` 时因本机仅有静态 Boost filesystem/process 被拒绝，补充此选项后成功；与新增日志无关。
- 上述均为目标增量编译和相关用例回归，并非全量 clean build、全部 CTest 或 Windows/Linux 验证；既有全量构建阻塞见 `code-plan/LOG_日志功能设计与实施计划.md` 的 FakeA2l 记录。

## 边界及未解决事项

- XCP 没有响应事务 ID，日志只能陈述当前 Pending 快照，不能证明某个迟到 RES 属于该命令；RES/ERR 正文保持全量脱敏。
- 构建树中的文件日志属于测试产物，不保证磁盘写失败时 glog 本身完全不输出内部诊断；不能把测试环境结果泛化为所有宿主。
