# xcp_master_udp — XCP master(UDP) 示例程序（cpp_demo 原样对手端）

演示如何用 libxcp（`calmcar::xcp::XcpMaster`）+ libxcp a2lbridge 连接
`thirdparty/XCPlite/examples/cpp_demo` 官方 slave，经 UDP 完成 CONNECT、A2L
获取/加载、符号读取与 CalSeg 结构成员写回读。本目录是**示例程序**（用户形态
指令 m00100），不是 gtest 套件；随注册一个 ctest 用例 `ExampleXcpMasterUdp`。

计划：`code-plan/XCPlite_cpp_demo_示例对手端计划.md`（v2.1）
实施记录：`code-plan/XCPlite_cpp_demo_实施记录_批次22.md`

## 1. 构建

依赖两个既有开关（根 `CMakeLists.txt` 的 `LIBXCP_BUILD_EXAMPLES` 门会校验并
给出 FATAL_ERROR 指引）：

```bash
cmake -B cmake-build-xcplite -S . -DCMAKE_BUILD_TYPE=Release \
      -DLIBXCP_BUILD_XCPLITE_SLAVE=ON -DLIBXCP_BUILD_A2L=ON -DLIBXCP_BUILD_EXAMPLES=ON
cmake --build cmake-build-xcplite --config Release   # 产出 cpp_demo.exe + xcp_master_udp.exe
```

## 2. 运行

零参数即完整演示（自动两轮，见 §3），全部断言通过退出码 0，否则 1；
5555 端口被占时预检退出码 2：

```bash
./xcp_master_udp [--host 127.0.0.1] [--port 5555] [--run-dir DIR] [--watch N]
                 [--no-writeback] [--timeout-ms 2000] [--demo EXE]
```

产物保留在 `--run-dir`（默认 `./xcp_master_udp_run`）：`cpp_demo_V201.a2l`
(15269B)、`cpp_demo_V201.bin`（XCPlite PERSISTENCE 落盘）、预置的
`XCP_104.aml`。

## 3. 两轮原理（对手端事实，源码实证见计划 §1）

cpp_demo 的 A2L 模式为 `FINALIZE_ON_CONNECT`：**首次 CONNECT 即触发 A2L
生成落盘**。同一目录重启进程时 XCPlite 判定版本匹配不再重生成，且
`PERSISTENCE` 会加载上一轮 `.bin`。因此：

- **Run A（生成轮）**：示例自管理启动 demo → CONNECT 触发 finalize → 等待
  a2l 落盘 → `UPLOAD` 全量读回与磁盘逐字节一致 → 终止进程。
- **Run B（读取轮）**：桥接加载 Run A 产物（`require_if_data_xcp=false`）→
  重启 demo → CONNECT → 标量读取/动态性/写回读 → DISCONNECT。

另：生成的 a2l 含 `/include "XCP_104.aml"`（a2l_writer.c:474，路径按 a2l.h:66
相对 a2l 所在目录解析），aml 不在 cpp_demo 的 CWD 搜索路径内，故示例在 Run A
前把 `thirdparty/XCPlite/XCP_104.aml`（构建定义 `XCP_104_AML_FILE`）复制到
run-dir——不改上游、不改 demo 启动方式。

## 4. 断言集与关键实证

| # | 断言 | 实证要点 |
|---|------|----------|
| V1 | Run A 后 a2l 落盘 + UPLOAD==磁盘 | 15269 字节逐字节一致 |
| V2 | Run A CONNECT 成功 | FINALIZE_ON_CONNECT 触发 |
| V3 | A2L 加载成功 | 符号总数 30（含 INSTANCE 与结构成员叶子） |
| V4 | Run B 复用同一 a2l + CONNECT | 重启不重生成、`.bin` 加载 |
| V5 | CONNECT 回参 | MAX_CTO=248 MAX_DTO=1024 res=0x05，小端 |
| 动态 | counter 两次读取变化 | 采集通路可用 |
| 写回读 | `kParameters.counter_max`(0x0,U16)/`delay_us`(0x4,U32) | 静态钉值 1000 → 写 1234/2000 读回一致 → 恢复 1000 读回一致 |

写回读采用 **INSTANCE 基址+offset 直址**（桥接 Find("kParameters")
@0x80010000 ext=0；结构成员叶子键因成员引用 TYPEDEF_CHARACTERISTIC 不被桥接
登记，路线论证见分析结论 §4）；读回使用**窗口右对齐 over-read**：XCPlite 动态
ext(3..6) 异步队列把响应填充到 4 字节对齐（上游 queue32.c:207-240 TODO，不改
第三方），实测观察长度 = `4·⌈(1+n)/4⌉−1`；成员起点直读会越出段尾被 Slave 拒
（`out of bound calseg read access` → ERR_ACCESS_DENIED，round11 实证），故读
窗右移到成员末端再切片，写命令仍按精确宽度。

已知口径差（如实记录）：`temperature` 桥接物理值显示 -2450（桥接
`p=f+i*C+i*O` 与 ASAM 语义差异，libxcp 既有行为，非本示例引入）。

## 5. ctest 集成

`ExampleXcpMasterUdp`（RUN_SERIAL，TIMEOUT 90s）直接运行示例零参数全流程。
与既有基线并存：Xcplite 29 例、全量 395 项、OFF 基线计数均不受影响
（`LIBXCP_BUILD_EXAMPLES=OFF` 时整个 examples 子目录跳过）。
端口 5555 是 cpp_demo 硬编码值：并行测试或本机占用会触发预检失败（exit 2），
属环境前提，非示例缺陷。
