# XCPlite 独立 Slave 实施记录

## 范围与约束

按已批准的 [独立 Slave 实施计划](XCPlite_独立Slave_测量标定与DAQ_实施计划.md) 新增独立 XCPlite UDP Slave 示例；未修改 `thirdparty/`、核心库或现有示例。新增 C++ 使用 C++20，并按仓库 `.clang-format` 格式化；新增接口及成员采用中文 Doxygen 注释。

## 实施内容

- 新增 `examples/xcp_slave_demo/`：独立构建目标 `xcp_slave_demo`、测量/标定模型、XCPlite 事件与 A2L 注册、命令行入口和使用说明。
- 普通服务模式默认监听 `127.0.0.1:5555`，不初始化 A2L writer、不生成 A2L，启动独立 UDP XCP Server。SIGINT/SIGTERM 后关闭服务。
- 显式 `--generate-a2l` 进入生成后退出模式：注册与服务相同的事件和对象布局，写出 `xcp_slave_demo.a2l` 及其外链 `XCP_104.aml`，检查文件和 A2L Bridge 解析结果，不启动 UDP Server、算法循环或 DAQ。因 XCPlite 当前版本未实现内嵌 AML，生成模式不设置 `A2L_MODE_EMBED_AML_FILE`。
- 生成模式默认禁止覆盖；`--overwrite` 仅处理两个确切输出文件。输出路径会先规范化并拒绝 `thirdparty/` 目录及其子目录，避免生成器覆盖第三方源码。
- 测量对象 `meas_fast`、`meas_slow` 与标定段 `cal` 分离。测量/标定均覆盖 `int8_t`、`uint8_t`、`int16_t`、`uint16_t`、`int32_t`、`uint32_t`、`float`、`double`，并包含嵌套结构和固定 8 元素数组。
- 固定事件按 fast=`meas_fast_10ms`、slow=`meas_slow_100ms` 顺序创建，周期分别为 10,000,000 ns 和 100,000,000 ns；测量对象以各进程自身地址作为 DAQ 相对基址。运行不启用 XCP persistence。
- 模拟算法根据 tick、标定增益/偏置/限幅及 correction 数组生成测量值；标定参数无效时输出安全值并设置诊断状态。CalSeg 读取仅在短 RAII 锁作用域内复制快照。
- CMake 将测试目标与 Slave 可执行文件按 examples/tests 选项挂接；AML 源路径由 CMake 注入，没有写死开发机绝对路径。

## 自动化测试

新增 `tests/xcp_slave_demo_model_test.cpp` 和 `tests/xcp_slave_demo_integration_test.cpp`，覆盖参数校验、安全降级、数组/结构算法、样本计数饱和、CLI 错误、A2L/AML 生成与覆盖保护、生成时 UDP 端口可保持占用、普通服务不生成 A2L，以及独立进程间相对寻址和标定访问。

跨进程集成测试在 Master 同一会话中对 `cal.model.offset`、`cal.basic.signed_8`、`cal.corrections[7]` 执行写入/读回；分别订阅快速与慢速事件并检查 DTO，验证标定偏置影响快速测量输出及慢速事件实际触发。事件通道编号从解析后的 A2L 获取，不硬编码。

## 验证记录

Release 配置：`cmake-build-release`。

```text
cmake --build cmake-build-release --target xcp_slave_demo XcpliteIntegration -j4
```

结果：Slave 和集成测试目标构建成功。GoogleTest 依赖产生 `implicit conversion from 'char8_t' to 'char32_t'` 编译警告，链接器产生 duplicate libraries 警告；本次未观察到新增代码编译错误。

```text
ctest --test-dir cmake-build-release -R '^XcpSlaveDemo' --output-on-failure
```

结果：9/9 通过。覆盖 6 项模型测试和 3 项进程/A2L/DAQ 集成测试；跨进程测试同时通过 fast 与 slow DAQ 验证。另运行 `git diff --check`，无空白错误。

源码核验与行为测试确认：A2L 外链 AML 与主文件可由 Bridge 解析；generation 即使 loopback UDP 端口已占用仍成功；普通服务目录不产生 A2L；按相同程序版本和注册顺序运行的独立服务进程可正确接收标定下载，并通过 10 ms、100 ms 两个 DAQ 事件发送测量 DTO。

原有 XCPlite 回归命令 `ctest --test-dir cmake-build-release -R '^Xcplite' --output-on-failure` 在首个用例停滞超过 20 分钟后终止。随后用 30 秒超时单独复现现有用例 `XcpliteSlaveProcessTest.OccupiedPortDoesNotSatisfyReadinessAndRetriesNextCandidate`：用例在 830 ms 时报告 `XcpliteSlaveProcess::Start 失败: 端口 65449 上的 Slave 未就绪或已退出`，同时输出 `xcp_test_slave: server init failed on port 65421`；测试进程之后未正常退出，最终被 CTest 超时终止。此用例源文件 `tests/xcplite_slave_fixture_test.cpp` 未由本次修改，原因尚未查明；该旧回归套件不能记为通过。新 Slave 专属的 9 项测试随后再次全数通过。测试仅在当前开发环境的 Release 配置下实测；Windows/Linux 的运行测试未在本轮执行。

## 已知约束

- 10 ms/100 ms 是桌面逻辑调度周期，不提供硬实时保证。
- XCP 内存协议不承诺把测量区设为只读；分离通过对象、A2L 分组和 CalSeg 表达。
- generation 与服务进程须使用相同版本及测量对象、事件、标定段注册顺序；A2L 与同目录 `XCP_104.aml` 必须一起交付。
