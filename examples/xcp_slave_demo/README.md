# XCPlite 独立 XCP Slave 示例

本示例基于 `thirdparty/XCPlite` 提供可独立运行的 XCP UDP Slave，用于验证测量、标定、A2L 与 DAQ；不修改 XCPlite 第三方源码。

## 构建

在仓库根目录启用示例和 XCPlite Slave：

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBXCP_BUILD_EXAMPLES=ON \
  -DLIBXCP_BUILD_XCPLITE_SLAVE=ON
cmake --build cmake-build-release --target xcp_slave_demo
```

可执行文件位于构建树的 `examples/xcp_slave_demo/` 目录。

## 生成 A2L（生成后退出）

```bash
./cmake-build-release/examples/xcp_slave_demo/xcp_slave_demo \
  --generate-a2l --output-dir ./a2l --bind 127.0.0.1 --port 5555
```

生成模式只初始化 XCP/A2L 元数据并写文件，不启动 UDP server、模拟算法或 DAQ。成功后退出码为 0。输出目录包含 `xcp_slave_demo.a2l` 和 A2L 外链所需的 `XCP_104.aml`；两者需一起交付。默认不覆盖已存在的任一目标文件；只有明确传入 `--overwrite` 才替换这两个确切文件，不删除目录中的其他内容。为保护第三方源码，`thirdparty/` 目录及其子目录不能作为输出目录。A2L 自描述的 UDP 端点应是 Master 可连接的 IPv4 地址，不接受 `0.0.0.0`。

参数：

- `--generate-a2l`：显式进入生成模式；生成完成退出，不启动服务。
- `--output-dir <目录>`：A2L/AML 输出目录，默认当前工作目录下的 `a2l`。
- `--bind <IPv4>`：服务监听及 A2L 端点地址，默认 `127.0.0.1`。
- `--port <端口>`：UDP 端口，默认 `5555`。
- `--overwrite`：仅允许覆盖 `xcp_slave_demo.a2l` 和 `XCP_104.aml`。
- `--help`：显示用法。

## 启动 Slave

```bash
./cmake-build-release/examples/xcp_slave_demo/xcp_slave_demo \
  --bind 127.0.0.1 --port 5555
```

服务模式不生成或修改 A2L 文件。使用生成模式获得的 A2L 通过 Master 建立 UDP 会话，并可读取测量/DAQ 或下载标定值。使用 Ctrl+C（SIGINT）或 SIGTERM 停止服务。

## 数据与 DAQ

- `meas_fast`：快速测量对象，包含基础值、嵌套状态结构和 8 元素 `samples` 数组，按 10 ms 事件更新并触发 DAQ。
- `meas_slow`：慢速测量对象，包含 8 种基础类型、统计结构和 8 元素 `history` 数组，按 100 ms 事件更新并触发 DAQ。
- `cal`：独立标定段，包含 8 种基础类型、`model` 结构和 8 元素 `corrections` 数组。示例算法应用 `gain`、`offset`、输出限幅及数组修正。

事件周期是桌面测试用的逻辑调度周期，不提供硬实时保证。测量对象通过每个进程自己的相对基址注册；生成 A2L 与服务进程需使用相同程序版本及对象/事件注册顺序。XCP 内存协议本身不承诺把测量区设为只读；测量/标定的区分体现在对象、A2L 分组和 CalSeg 注册。

## 自动化测试

启用 `LIBXCP_BUILD_TESTS=ON` 与 `LIBXCP_BUILD_EXAMPLES=ON` 后，`XcpliteIntegration` 增加模型测试和独立进程集成测试。集成测试会验证 A2L/AML 输出及覆盖策略、生成模式无 UDP 绑定、跨进程相对寻址、标定写入和快速/慢速 DAQ DTO。可运行：

```bash
ctest --test-dir cmake-build-release -R 'XcpSlaveDemo|XcpliteIntegration' --output-on-failure
```
