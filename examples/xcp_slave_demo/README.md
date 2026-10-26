# XCPlite 独立 XCP Slave 示例

本示例基于 `thirdparty/XCPlite` 提供可独立运行的 XCP Slave（支持 **UDP 与 TCP**），用于验证测量、标定、A2L 与 DAQ；不修改 XCPlite 第三方源码。

> **一次只能跑一种传输**：XCPlite 的 `XcpEthServerInit` 是全局单例
> （`thirdparty/XCPlite/src/xcpEthServer.c` 的 `"XCP server already running!"`
> 会拒绝第二次初始化），传输方式在初始化时定死。需要同时提供 UDP 与 TCP 时，
> 请启动两个进程并使用不同端口。

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
# UDP（默认）
./cmake-build-release/examples/xcp_slave_demo/xcp_slave_demo \
  --generate-a2l --output-dir ./a2l --bind 127.0.0.1 --port 5555

# TCP：A2L 会改为声明 XCP_ON_TCP_IP
./cmake-build-release/examples/xcp_slave_demo/xcp_slave_demo \
  --generate-a2l --transport tcp --output-dir ./a2l --bind 127.0.0.1 --port 5555
```

生成模式只初始化 XCP/A2L 元数据并写文件，不启动 server、模拟算法或 DAQ。成功后退出码为 0。输出目录包含 `xcp_slave_demo.a2l` 和 A2L 外链所需的 `XCP_104.aml`；两者需一起交付。默认不覆盖已存在的任一目标文件；只有明确传入 `--overwrite` 才替换这两个确切文件，不删除目录中的其他内容。为保护第三方源码，`thirdparty/` 目录及其子目录不能作为输出目录。A2L 自描述的端点（地址、端口、传输类别）会被回读校验：必须与 CLI 的 `--bind`/`--port`/`--transport` 完全一致，不一致时生成失败并以退出码 1 结束——避免把 UDP 的 A2L 交给 TCP 主站使用。地址不接受 `0.0.0.0`。

参数：

- `--generate-a2l`：显式进入生成模式；生成完成退出，不启动服务。
- `--output-dir <目录>`：A2L/AML 输出目录，默认当前工作目录下的 `a2l`。
- `--bind <IPv4>`：服务监听及 A2L 端点地址，默认 `127.0.0.1`。
- `--port <端口>`：监听端口，默认 `5555`（TCP 与 UDP 端口空间独立，可用同一端口号）。
- `--transport <udp|tcp>`：传输方式，默认 `udp`；只接受这两个字面量。
- `--overwrite`：仅允许覆盖 `xcp_slave_demo.a2l` 和 `XCP_104.aml`。
- `--help`：显示用法。

## 启动 Slave

```bash
# UDP（默认，与既有脚本兼容）
./cmake-build-release/examples/xcp_slave_demo/xcp_slave_demo \
  --bind 127.0.0.1 --port 5555

# TCP
./cmake-build-release/examples/xcp_slave_demo/xcp_slave_demo \
  --transport tcp --bind 127.0.0.1 --port 5555
```

服务模式不生成或修改 A2L 文件。使用生成模式获得的 A2L 通过 Master 建立会话（UDP 或 TCP），并可读取测量/DAQ 或下载标定值。使用 Ctrl+C（SIGINT）或 SIGTERM 停止服务。

主站侧连接 TCP 时必须显式给出帧布局，无法从版本号或 A2L 推断：

```bash
xcp_master --a2l ./a2l/xcp_slave_demo.a2l --host 127.0.0.1 --port 5555 \
  --transport tcp --tcp-profile asam-v1.4
# 或用 --tcp-profile auto 让主站按 {V14/8bit, V15/8bit, V15/16bit, V15/32bit}
# 的顺序逐个候选试连
```

## 数据与 DAQ

- `meas_fast`：快速测量对象，包含基础值、嵌套状态结构和 8 元素 `samples` 数组，按 10 ms 事件更新并触发 DAQ。
- `meas_slow`：慢速测量对象，包含 8 种基础类型、统计结构和 8 元素 `history` 数组，按 100 ms 事件更新并触发 DAQ。
- `cal`：独立标定段，包含 8 种基础类型、`model` 结构和 8 元素 `corrections` 数组。示例算法应用 `gain`、`offset`、输出限幅及数组修正。

事件周期是桌面测试用的逻辑调度周期，不提供硬实时保证。测量对象通过每个进程自己的相对基址注册；生成 A2L 与服务进程需使用相同程序版本及对象/事件注册顺序。XCP 内存协议本身不承诺把测量区设为只读；测量/标定的区分体现在对象、A2L 分组和 CalSeg 注册。

## 自动化测试

启用 `LIBXCP_BUILD_TESTS=ON` 与 `LIBXCP_BUILD_EXAMPLES=ON` 后，`XcpliteIntegration` 增加模型测试和独立进程集成测试。集成测试会验证 A2L/AML 输出及覆盖策略、生成模式不绑定网络端口、跨进程相对寻址、标定写入、快速/慢速 DAQ DTO，以及 `--transport tcp` 下的 TCP 连接、POLL 读取与 DAQ 解码（`GeneratedTcpA2lFeedsLiveSessionOverTcp`）。可运行：

```bash
ctest --test-dir cmake-build-release -R 'XcpSlaveDemo|XcpliteIntegration' --output-on-failure
```
