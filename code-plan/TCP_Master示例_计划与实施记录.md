# TCP Master 示例计划与实施记录

## 1. 目标与范围

新增 `examples/xcp_master_tcp/`，提供可运行的 TCP XCP Master 示例，完整演示 A2L 加载、测量物理值读取和用户显式授权后的标定写入/回读；支持自动启动本仓库示例 Slave 和连接外部 Slave 两种模式。标定写入后保留目标值，不自动恢复原值。

本任务不修改第三方依赖，不改变现有 TCP wire decoder/transport 行为，也不实现断线自动重连。

## 2. 规范与线路兼容设计

规范参考：`docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`。TCP 上的 framing profile 是连接级显式配置，V1.4 与 V1.5 的 LEN/FILL 语义不同；不能通过 XCP 版本、A2L 对齐声明或首帧内容静默推断，也不能在同一连接内切换 profile。

内置 `xcp_slave_demo` 使用仓库确认的 `TcpWireProfile::AsamV14` 与 `PacketAlignment::Bit8`。外部模式要求用户显式传入 profile 和 alignment，支持 V1.4/V1.5 与 8/16/32-bit alignment。实现契约以 `include/libxcp/tcp_wire_profile.hpp`、`include/libxcp/tcp_transport_config.hpp` 为准。本次执行环境无法提取指定 PDF 的压缩正文，因此本文件不伪造条款号或声称逐条引用 PDF；后续如需规范条款逐项追溯，应使用可读取 PDF 正文的工具补充核验。

## 3. 组件及数据流

### 自动模式

1. 在运行目录生成 `xcp_slave_demo.a2l` 与 `XCP_104.aml`。
2. 启动 `xcp_slave_demo --transport tcp --bind ... --port ...`；从站由示例进程 RAII 清理。
3. 通过 A2L Bridge 加载 A2L，按全限定 A2L 测量符号获取地址、扩展、类型和元素宽度。
4. 建立显式 AsamV14/8-bit TCP 连接，读取 raw bytes 并调用 `ToPhysical` 展示测量值。
5. 默认只读；只有同时提供 `--write-calibration` 与目标值时，才对单个 A2L 直接标量标定执行写入和回读。写入不恢复原值。

### 外部模式

调用者提供 host/port、A2L、profile、alignment 和测量全限定符号。程序只连接该端点，不管理外部从站生命周期。标定另需写入开关、符号和值；只接受 A2L `READ_WRITE` 的直接数值 `CHARACTERISTIC/VALUE` 标量，并由 `FromPhysical` 做物理到 raw 编码和限值检查。

## 4. 错误处理与安全边界

- 参数缺失、模式混用、非法 profile/alignment、非有限标定值均 fail-closed。
- A2L 查找失败、符号类别/宽度不匹配、读写长度错误、转换失败均报错，不猜地址、不自行构造偏移。
- TCP 初连有界重试以等待刚启动的内置 Slave；成功后断链不自动重连。
- 写命令成功但回读失败时提示目标值可能已改变；不会自动回滚。
- A2L `READ_WRITE` / limit 只能证明 A2L 描述，不替代用户对目标 ECU、台架/车辆状态与授权的确认。

## 5. 测试计划

- Release 构建 `xcp_master_tcp` 和集成测试目标，跨平台代码按 C++20 编译。
- 自动模式黑盒测试：启动示例 Slave，加载生成 A2L，读取测量并正常退出。
- 外部模式黑盒测试：测试夹具独立生成 A2L/启动 Slave；运行 Master 外部模式执行标定；在 Master 退出但 Slave 仍运行时另建 XCP 会话，验证目标物理值仍为写入值。
- 执行对应 CTest；构建前按仓库约定清理 Release 构建目录。

## 6. 实施记录

### 已实施

- 新增 `examples/xcp_master_tcp/xcp_master_tcp.cpp` 与 README，配置自动/外部运行、显式 TCP framing、A2L 物理测量及 opt-in 标定。
- 为 `examples/xcp_master_udp/demo_process.*` 增加可传 argv 的进程启动和有界等待/退出码接口，供 TCP Master 启动生成器、Slave，并由测试启动子进程；原无参数调用保留。
- 新增 `tests/xcp_master_tcp_integration_test.cpp`，覆盖外部 framing 参数缺失时拒绝、自动模式及外部标定后值保留。
- 在 `examples/CMakeLists.txt` 注册 TCP Master 子目录，新增 CMake 目标/CTest 构建配置。

### 验证状态

- 完成 Release 清理、配置及全量构建：`cmake --build cmake-build-release --target clean`；`cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_BUILD_TESTS=ON -DLIBXCP_BUILD_EXAMPLES=ON`；`cmake --build cmake-build-release -j 4`。全量构建退出码 0（415/415 targets）。
- 最终变更后定向重建 `xcp_master_tcp` 与 `XcpMasterTcpIntegration`：退出码 0。
- 最终运行 `ctest --test-dir cmake-build-release -R XcpMasterTcpExample --output-on-failure`：3/3 通过，0.85 s。覆盖外部模式未显式选择 profile/alignment 时 fail-closed、自动启动 Slave 并读 A2L 测量、外部标定写入后独立会话确认值仍保留。
- 回归验证既有 UDP 示例 `ctest --test-dir cmake-build-release -R '^ExampleXcpMasterUdp$' --output-on-failure`：1/1 通过，5.95 s。
- macOS 26 SDK 对 `posix_spawn_file_actions_addchdir_np` 发出弃用警告；保留该兼容 API 以支持较旧 macOS。GoogleTest 的 char8_t 转换及重复链接库另有既有工具链警告；本次目标构建无错误。
- 实测环境为 macOS；Windows/Linux 尚未在本轮执行构建或测试。指定规范 PDF 正文提取工具在此环境不可用，未声称逐条核验规范条文。
