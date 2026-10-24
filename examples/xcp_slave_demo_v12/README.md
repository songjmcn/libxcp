# XCPlite V1.2 XCP 从站测试程序

本示例基于 `thirdparty/XCPliteV1.2`（协议层 **1.2 / 传输层 1.4**）提供可独立运行的 XCP 从站，
用于验证 **TCP/UDP 传输**、**DAQ 模型 + POLL 命令模式**、**A2L 生成**与**标定（含 CAL page 切换）**。

与 `examples/xcp_slave_demo`（基于 V2.2）的分工：本目录只链接 V1.2 源码，用于验证 libxcp 主站
对旧版协议层/传输层的兼容性；两份 XCPlite 副本的 `xcplib` 目标同名，因此**不能**
`add_subdirectory(thirdparty/XCPliteV1.2)`，本目录的 `CMakeLists.txt` 自带一份显式源列表构建
`xcplite_v12` 静态库。

## 构建

```bash
cmake -S . -B cmake-build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DLIBXCP_BUILD_EXAMPLES=ON
cmake --build cmake-build-release --target xcp_slave_demo_v12
```

可执行文件位于构建树的 `examples/xcp_slave_demo_v12/` 目录。

配置注入说明（`CMakeLists.txt`）：V1.2 的 `src/main_cfg.h` 默认**关闭**标定段
（`OPTION_CAL_SEGMENTS` 被注释），且该头文件以引号形式包含、无法用同名文件覆盖，因此通过
`target_compile_definitions` 注入 `OPTION_CAL_SEGMENTS` 与 `OPTION_CAL_SEGMENT_COUNT=8`，
注入后 `xcp_cfg.h` 才会启用 `XCP_ENABLE_CALSEG_LIST` / `XCP_ENABLE_SEG_ADDRESSING` /
`XCP_ENABLE_CAL_PAGE`（双页）与 `XCP_ENABLE_COPY_CAL_PAGE`。

## 生成 A2L（生成后退出）

```bash
./cmake-build-release/examples/xcp_slave_demo_v12/xcp_slave_demo_v12 \
  --generate-a2l --output-dir ./a2l --port 5555 --transport udp --verify-a2l
```

输出 `xcp_slave_demo_v12.a2l` 与外链所需的 `XCP_104.aml` 空壳，二者必须一起交付；默认不覆盖已存在文件，
需显式 `--overwrite`。`--verify-a2l` 会用 `libxcp::a2lbridge` 重新解析生成结果并打印事件/符号摘要。

**A2L 必须在每次启动时重新生成**（`--a2l-mode write-always`，默认）：测量量使用**事件相对地址**，
A2L 中记录 `(事件号 << 16) | 相对基址偏移`，其中基址是该进程内的测量块对象地址。

## 启动从站

```bash
# UDP
./cmake-build-release/examples/xcp_slave_demo_v12/xcp_slave_demo_v12 \
  --transport udp --bind 127.0.0.1 --port 5555 --output-dir ./a2l

# TCP
./cmake-build-release/examples/xcp_slave_demo_v12/xcp_slave_demo_v12 \
  --transport tcp --bind 127.0.0.1 --port 5555 --output-dir ./a2l
```

`--transport` 只能二选一：V1.2 的以太网服务是进程级单例
（`thirdparty/XCPliteV1.2/src/xcpEthServer.c` 检测到已初始化会直接返回失败），
同一进程无法同时监听 TCP 与 UDP；需要两种传输同时在线时启动两个进程并使用不同端口。

主要参数（完整列表见 `--help`）：

| 参数 | 说明 |
| --- | --- |
| `--transport udp\|tcp` | 传输方式，默认 `udp` |
| `--bind <IPv4>` | 监听与 A2L 端点地址，默认 `127.0.0.1`，禁止 `0.0.0.0`（V1.2 不写 TRANSPORT_LAYER 段） |
| `--port <端口>` | 监听端口，默认 `5555` |
| `--queue-size <字节>` | 测量队列容量，默认 65536 |
| `--a2l-mode write-always\|write-once` | A2L 生成策略，默认 `write-always` |
| `--output-dir <目录>` | A2L 输出目录，默认当前目录 |
| `--generate-a2l` | 仅生成 A2L 与 AML 空壳后退出 |
| `--overwrite` | 允许覆盖已存在的 A2L（仅生成模式） |
| `--verify-a2l` | 生成后用 a2lbridge 解析并打印摘要 |
| `--log-level <0..5>` | XCPlite 日志级别，默认 3 |
| `--max-runtime-ms <n>` | 运行 n 毫秒后自动退出（0=不限制） |
| `--ready-token <标记>` | 就绪后向 stdout 打印该标记，供脚本探测 |

退出码：`0` 正常结束；`2` 参数错误；`3` 初始化失败（`XcpInit`/`A2lInit`/监听端口失败）；
`4` A2L 输出准备、生成或自校验失败。

## 变量模型

| 事件 | 周期 | 内容 |
| --- | --- | --- |
| `meas_fast_10ms` | 10 ms | `counter`、`sine`/`square`/`saw`（经 `gain`/`offset`/`limit` 变换）、`samples[8]`（正弦 + 逐通道修正量） |
| `meas_slow_100ms` | 100 ms | 八种基础类型（`u8`…`f64`）、`stats` 结构体、`history[8]` 环形缓冲、`event_tick` |
| 标定段 `cal` | — | `gain`/`offset`/`limit`、`run`（0=冻结波形推进）、`corrections[8]` |

标定参数**被算法真实消费**：`gain`/`offset` 参与波形变换，`limit` 参与限幅，`run=0` 冻结推进，
`corrections[i]` 加到 `samples[i]`。因此标定联调验证的是"起作用"，不只是"写成功"。
`stats.mean_output` 为快速输出的滑动均值，`stats.last_diagnostic` 在参数异常（`limit<=0` 或 `gain<0`）时为 `-1`。

标定段写入的 **A2L 声明限值**（主站会据此在本地拒绝越界目标值，错误码 B-15）：

| 参数 | 限值 |
| --- | --- |
| `cal.gain` | `0 … 10` |
| `cal.offset` | `-100 … 100` |
| `cal.limit` | `0 … 1000` |
| `cal.run` | `0 … 1` |
| `cal.corrections[8]` | `-5 … 5` |

例如把 `cal.gain` 写成 `20` 会被 `xcp_master` 直接拒收（不会下发任何 XCP 命令），
这是限值约束而不是链路故障；需要更大增益时应先改本示例的注册参数再重新生成 A2L。

## 标定页

V1.2 的页号约定与常见实现**相反**：

- `DEFAULT`（参考页 / FLASH）= **1**，`WORKING`（工作页 / RAM）= **0**（`src/xcpLite.h:468-469`）。
- 主站经 `SET_CAL_PAGE` 切 `XCP` 侧页；SEG 相对寻址的读写走 `XCP` 侧当前页，
  写参考页会被拒绝（`ERR_ACCESS_DENIED`）。
- `COPY_CAL_PAGE` 只支持同段 **参考页(1) → 工作页(0)**。
- 从站算法读的是 `ECU` 侧当前页，默认停在工作页。

## A2L 后处理

生成后本程序会给标定段 `INSTANCE` 补上 `READ_WRITE`：V1.2 在段相对模式下不输出该标记，
而 `tools/xcp_master/symbol_policy.hpp` 要求可写符号带 `A2L READ_WRITE`，缺失会导致主站拒绝标定写入。
补丁只改命中唯一的一条 `INSTANCE` 声明行，先写 `<file>.patch.tmp` 回读比对，再用
`std::filesystem::rename` 原子替换，失败返回退出码 4。

## 已知限制

1. **DQ 条目必须按事件分开配置**：A2L 中快速/慢速测量分别绑定事件 0/1，
   DAQ list 装载跨事件条目会被从站按 `ERR_DAQ_CONFIG` 拒绝（V1.2 的 `XCP_ENABLE_DAQ_ADDREXT` 语义）。
2. **不能用绝对地址模式**：V1.2 的 `src/xcp_cfg.h:202` 无条件把 `XCP_ADDR_EXT_APP` 定义为 `0x01`，
   与 CASDD 模式下的 `XCP_ADDR_EXT_ABS=0x01` 撞车，`src/xcpLite.c` 的 DAQ 采样循环会把
   `ext==XCP_ADDR_EXT_APP` 的条目整段填零（表现为 DAQ 净荷恒为 0，而 POLL 正常）。
   本示例因此改用 `addr_ext=3`（事件相对寻址），未修改第三方源码。
3. **A2L 中的 MEMORY_SEGMENT 段号**由 V1.2 写出为 `0`；标定页命令的段号需用 `0`（V1.2 的
   段号与索引一致，EPK 段占用保留编号），与 A2L 声明保持一致。
4. **不支持多实例**：V1.2 以太网服务是进程级单例，同进程只能一种传输、一个端点。
5. 端到端联调（DAQ/POLL/标定/切页）**没有自动化用例**：按计划 §8 只提供模型层单测
   （`tests/xcp_slave_demo_v12_model_test.cpp`），端到端由 `libxcp` 主站手工/探针联调。

## 单元测试

```bash
ctest --test-dir cmake-build-release -R XcpSlaveV12ModelTest --output-on-failure
```

测试目标只编译 `slave_model.cpp`，不链接 `xcplite_v12` 与 libxcp，避免与 `XcpliteIntegration`
已链接的 V2.2 `xcplite` 产生同名符号冲突。