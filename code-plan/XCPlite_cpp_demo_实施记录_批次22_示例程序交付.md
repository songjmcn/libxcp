# XCPlite cpp_demo 原样对手端 · 批次22 实施记录（示例程序交付）

日期：2026-07-22　计划：`code-plan/XCPlite_cpp_demo_示例对手端计划.md`（v2.1）
分析：`code-plan/XCPlite_cpp_demo_示例对手端_批次22_分析结论.md`
交付形态：独立示例程序（用户指令 m00100：不是 test，是 example）。

## 1. 交付文件

| 文件 | 内容 |
|------|------|
| `examples/CMakeLists.txt` | examples 聚合层（转发子目录） |
| `examples/xcp_master_udp/CMakeLists.txt` | 双目标 cpp_demo（引上游 XCPlite 源，不改第三方）+ xcp_master_udp；编译定义 `CPP_DEMO_EXECUTABLE`/`XCP_104_AML_FILE`；ctest 注册 `ExampleXcpMasterUdp`（RUN_SERIAL、TIMEOUT 90） |
| `examples/xcp_master_udp/xcp_master_udp.cpp` | 主示例：单命令自动两轮（Run A 生成 A2L / Run B 连接读取），CLI 全项（--host/--port/--run-dir/--demo/--watch/--no-writeback/--timeout-ms/--help） |
| `examples/xcp_master_udp/demo_process.hpp/.cpp` | demo 进程管理（Win CreateProcessA / POSIX posix_spawn+addchdir_np；Stop=优雅 wait→kill 兜底） |
| `examples/xcp_master_udp/README.md` | 两轮原理、构建/运行、断言集与实证、ctest 集成 |
| 根 `CMakeLists.txt` | `LIBXCP_BUILD_EXAMPLES` 门（+27 行，OFF 默认全隔离） |

## 2. 关键决策与技术点

1. **两轮工作流**：cpp_demo A2L 模式 `WRITE_ONCE|FINALIZE_ON_CONNECT|AUTO_GROUPS` → Run A CONNECT 触发生成 `cpp_demo_V201.a2l`（15269B），UPLOAD 全量与磁盘逐字节一致后终止；Run B 重启复用（"already exists with matching version, disabling A2L generation"）并以 `PERSISTENCE` 加载 `.bin`（2264B）。
2. **aml 预置**：生成 a2l 含 `/include "XCP_104.aml"`（a2l_writer.c:474，路径相对 a2l 解析，a2l.h:66）；示例经编译定义定位上游件并 `fs::copy_file` 进 run-dir，不改 thirdparty。
3. **端口占用预检（V6）**：5555 为 cpp_demo 硬编码；`PortLooksOccupied` 能 CONNECT=占用 → exit 2 干净失败。
4. **SHORT_UPLOAD over-read 适配**：XCPlite 动态 ext(3..6) 异步队列把响应填充至 4 字节对齐（queue32.c:207-240 上游 TODO 未动；CRM 无 TIME 字段 xcplite.c:2341-2363、xcp.h:558/:567）→ 实测观察长度 = `4·⌈(1+n)/4⌉−1`；示例读 n′≡3 mod 4 再截断。libxcp 侧 `CheckResLength`（src/command_executor.cpp:297-305）精确匹配口径保持不变——适配在示例端。
5. **写回读路线（分析文档 §4 定稿=B）**：结构成员叶子键不登记（桥接 typedef 源仅 `ListTypedefMeasurements`，a2l_bridge.cpp:85-102；cpp_demo 成员引用 TYPEDEF_CHARACTERISTIC）→ 弃叶子路径 Find；弃 loop_histogram（512B 分片+时序覆盖风险）；采 **INSTANCE 基址+offset 直址**（先例 tests/xcplite_write_test.cpp:191-217）。`Find("kParameters")`=0x80010000 ext=0；counter_max@0x0/U16、delay_us@0x4/U32（生成件 :71-72/:106-107）。解析失败计 FAIL（消除旧静默跳过空转）。
6. **窗口右对齐读**（round11 实证修复）：成员起点直读 over-read 越出段尾被 Slave 边界校验拒（`ERROR: out of bound calseg read access (addr=80010004, size=7)` → 0xF4 ERR_ACCESS_DENIED）；改为读窗右移到成员末端（windowStart=max(0,offset+size−n′)）再切片，写命令仍按成员精确宽度。
7. **静态钉值断言**：counter_max/delay_us 当前值=1000 与计划 §3 钉值口径相符，纳入 Check。

## 3. 测试与验收结果（全部实测）

| 门禁 | 结果 |
|------|------|
| 编译 | Release 零警告零错误（修复 C4100/C4267 后） |
| round11 首验 | 12 断言 1 失败（delay_us 越界）→ 定位并修复 |
| **round12 零参数实跑** | **EXITCODE=0，15 断言 0 失败**（V1–V6、四标量、counter 动态、双成员 钉值/写入/读回/恢复 全绿） |
| ctest -R ExampleXcpMasterUdp | PASS 6.97s（#395） |
| 全量 ON | **395/395 通过**（旧 394 零回归 + 新 1；Ag1_Cto8 已知 not-run） |
| Xcplite 子集 | 29/29（批次17–21 全不动） |
| OFF 基线 | 重建后 358/358 通过（清单含后续批次新增；陈旧清单曾 345——计数与计划"352"差异=清单刷新时点，非回归；OFF 缓存无 EXAMPLES 条目=门隔离生效） |

实测值快照：CONNECT 回参 MAX_CTO=248 / MAX_DTO=1024 / resources=0x05；temperature raw=32、speed≈2.735、counter 127→126 变化、sum≈78.83；kParameters.counter_max 1000→1234→1000、delay_us 1000→2000→1000 均读回一致。

## 4. 已知差异与遗留（不阻塞）

- `temperature` 桥接物理值显示 -2450：桥接 LINEAR `p=f+i·C+i·O` 口径与 ASAM 语义（应=0 偏移）差异，属 libxcp 既有行为，未改 src，README §4 已声明。
- `--watch N` 长采集为人工验收项（断言集不含）。
- 5555 端口为对手端硬编码：CI 与本机其它 XCP 服务互斥，靠预检+RUN_SERIAL 缓解。

## 5. 约束符合性

未修改 `thirdparty/`（cpp_demo 以源引用方式构建）；未新增 libxcp 公开接口；C++20；全部函数/成员 doxygen 中文注释；路径经编译定义/相对路径，无写死绝对路径；tests/ 既有 29 例未动。
