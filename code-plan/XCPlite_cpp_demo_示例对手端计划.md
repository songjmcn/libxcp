# XCPlite cpp_demo 原样对手端·示例程序计划（examples/xcp_master_udp）

状态：**批次22 已交付（实测收口）**——示例 `examples/xcp_master_udp` 建成；零参数实跑 EXITCODE=0（15 断言全绿，含 kParameters.counter_max/delay_us  INSTANCE+offset 直址写回读往返+恢复原值+静态钉值 1000）；ctest ExampleXcpMasterUdp PASS；全量 ON 395/395（旧 394 零回归+新 1，Ag1_Cto8 已知 not-run）、Xcplite 29/29、OFF 基线 358/358（清单含后续批次新增，与计划初稿"352"差=清单刷新，非回归）。实施记录：`XCPlite_cpp_demo_实施记录_批次22_示例程序交付.md`；分析：`XCPlite_cpp_demo_示例对手端_批次22_分析结论.md`。（原状态：待批准（v2.1——三个形态问题已定：目录 examples/xcp_master_udp · 单命令自动两轮 · 写回读纳入；剩余 C2/C4/C5 见 §7，已按推荐默认执行））
形态变更记录：
- User said (m00040): "根据 thirdparty\XCPlite\examples\cpp_demo 做 XCP master 端对手测试程序，功能：UDP connection、加载 A2L、读取 A2L 中的变量；先出计划 MD 放 code-plan 下。"
- 澄清（m00056）：保留 xcp_test_slave 不动，新增 cpp_demo 原样对手；A2L 通路——User said: "我建议运行2次 XCPlite demo程序，第一次生成A2L文件，这时候可以加载生成的A2L文件，第二次就开始做正常的XCP 测量。"
- User said (m00100): "我认为此次开发应该不是test，而是一个example，应该新建一个example文件夹，作为单独的示例程序。"
- 形态问答（ask_user_question 本轮答复）：目录与目标名 = `examples/xcp_master_udp`；运行形态 = **单命令自动两轮**（示例自管理 demo 进程）；写回读 = **纳入**。
→ 本次交付形态 = **独立示例程序**（不是 gtest 套件）。批次17–21 既有 29 例测试与夹具一律不动、不迁移。

## 1. 对手端事实基线（cpp_demo，源码实证，禁改）

| 项 | 值 | 出处 |
|---|---|---|
| 项目/EPK | PROJECT="cpp_demo" / EPK="V201" | examples/cpp_demo/src/main.cpp |
| 传输 | UDP（TCP=false）、端口 **5555 constexpr 硬编码**、绑 {0,0,0,0}、main 无 argv（不可注入） | main.cpp:24/:25/:28/:91 |
| 注册路径 | OPTION_USE_VARIADIC_MACROS → DaqEventVar 变参 | main.cpp:17,:217-224 |
| XcpInit | XCP_MODE_PERSISTENCE \| XCP_MODE_LOCAL | main.cpp:102 |
| A2lInit | WRITE_ONCE \| FINALIZE_ON_CONNECT \| AUTO_GROUPS | main.cpp:112 |
| 可钉静态值 | CalSeg ParametersT{counter_max=1000, delay_us=1000}——**counter_max/delay_us 是 STRUCTURE 成员，非顶层符号**：A2lTypedefParameterComponent(main.cpp:125-126) 只注册 typedef 组件，顶层 CHARACTERISTIC 只有结构体实例 "ParametersT"(:128)，无 "counter_max"/"delay_us" 名；且 ParametersT.read_write 实然=**false**（xcplite_a2l_bridge.cpp:914 仅 VAL_BLK 置可写，STRUCTURE 落 :910 默认；与运行期 calseg 实际可写矛盾）→ 结构体成员整体**排除写回读**，写回读改用顶层 VAL_BLK `loop_histogram`（见 V4/V6/§3-4） | main.cpp:33-39/:121-128；lookup.hpp:17-30；第三批工具调用核实 |
| 动态值 | temperature(UBYTE)=50 起，每绕圈+1、>150→0；speed(DOUBLE) 渐近 250、>245→0；SigGen1 ±12.5 TRIANGLE / SigGen2 ±80 SINE（实例相对寻址）；mainloop 栈测量 counter/sum/loop_cycletime/histogram | main.cpp:44-45/:204-211/:54-83；sig_gen.cpp:37/:49/:74/:80-83/:119 |
| 文件机制 | WRITE_ONCE 主文件名带 EPK → `cpp_demo_V201.a2l`；首个 CONNECT 即 A2lFinalize，一次性完成：主 A2L 写出 + XcpSetA2lName（可 UPLOAD）+ `XcpBinWrite` 写 `cpp_demo_V201.bin`（a2l.c:1660-1677）→ **CONNECT 之后硬杀进程产物已完整**；Run B 同目录重启检测 .a2l 存在→跳过生成（a2l.c:1540-1547），.bin EPK 匹配复用（persistence.c:103-111/:401） | a2l.c:316/:341/:1464-1478/:1518-1521 |
| F4 | 绑 {0,0.0.0} → A2L **无 TRANSPORT_LAYER**，端点自备 127.0.0.1:5555 | a2l_writer.c:346-361 |

## 2. 示例程序目录与目标

```
examples/
  CMakeLists.txt                  # 新目录（根当前无 examples/，顶层已核实）
  xcp_master_udp/
    CMakeLists.txt
    xcp_master_udp.cpp            # master 示例主体（链接 libxcp::libxcp + libxcp::a2lbridge）
    demo_process.hpp/.cpp         # 自包含小进程封装（CreateProcess/TerminateProcess；POSIX fork/exec），
                                  # 参考 tests/xcplite_slave_fixture.cpp 的做法但不引用、不改动 tests/
    README.md                     # 构建 + 单命令两轮用法 + 工作流背景（两次运行原理说明）
```

构建目标（两个）：
1. `cpp_demo`（对手 slave 可执行）：`add_executable` 引用 `thirdparty/XCPlite/examples/cpp_demo/src/{main,sig_gen,lookup}.cpp` 链接 xcplite 库——只引用第三方文件、零修改，符合禁改约束。自备目标的原因：根 CMakeLists.txt:188 固定 `XCPLITE_BUILD_EXAMPLES OFF`，打开会构建全部上游示例（Windows 不一定全可编）。编译条件（C++ 标准/defines）对齐上游目标定义 thirdparty/XCPlite/CMakeLists.txt:334-335，保持"原样"。
2. `xcp_master_udp`（本示例交付物）：链接 `libxcp::libxcp`（公开头 include/libxcp/：xcp_master.hpp、udp_transport.hpp、session.hpp、memory_access.hpp、command_executor.hpp…）+ `libxcp::a2lbridge`（LIBXCP_BUILD_A2L 分支产物；桥接头 libxcp/a2l/a2l_bridge.hpp 来自 LIBXCP_LIBA2L_ROOT，根 CMakeLists.txt:157-170）。liba2l.dll 运行期复制沿用 tests/CMakeLists.txt:146-148 POST_BUILD 模式。demo exe 定位：compile-def `CPP_DEMO_EXECUTABLE="$<TARGET_FILE:cpp_demo>"` + add_dependencies（沿用 tests/CMakeLists.txt:84-96 XCPLITE_SLAVE_EXECUTABLE 先例）。加载 A2L 是本示例核心功能，故 a2lbridge 不可用时不静默降级。

依赖门：新门 `option(LIBXCP_BUILD_EXAMPLES "构建 cpp_demo 原样对手示例" OFF)` + `add_subdirectory(examples)`；前置 = `LIBXCP_BUILD_XCPLITE_SLAVE=ON`（xcplite 库目标来源，根 CMakeLists.txt:185-195）且 `LIBXCP_BUILD_A2L=ON`（桥接来源）——任一未满足 → CONFIGURE 期 FATAL_ERROR 明确提示（不静默、不二次 add_subdirectory）。

## 3. 示例程序行为设计（已定：单命令自动两轮）

一条命令端到端：示例自行拉起/终止 cpp_demo，内部完成"第一次运行生成 A2L → 第二次运行正常测量"全流程。

CLI（全部有默认值，零参数即主路径）：

```
xcp_master_udp [--host 127.0.0.1] [--port 5555]
               [--run-dir <dir>]     # 缺省 xcplite_runs/xcp_master_udp_<时间戳>/（沿用现有 run 治理体系）
               [--cpp-demo <path>]   # 缺省经 compile-def 定位同输出目录 cpp_demo
               [--symbol <name>]...  # 追加读取符号；缺省=默认符号集（V4 快照后定）
               [--watch [N]]         # 两轮完成后轮询打印动态符号 N 轮（缺省 N=∞，Ctrl+C 退出）
               [--no-writeback]      # 跳过写回读环节（默认执行）
```

主流程（顺序即示例代码主线，每步打印进度，失败输出差异明细、exit≠0）：
1. **预检**：探测 5555 占用（demo 端口硬编码不可注入；被占 → 明确报错 exit 2，与断言失败区分口径，V6）；准备空 run-dir（保证走"首次生成"路径）。
2. **Run A（生成轮）**：拉起 `cpp_demo`（cwd=run-dir）→ UDP 建链 → CONNECT（触发 FINALIZE_ON_CONNECT）→ 轮询等待 `cpp_demo_V201.a2l` + `cpp_demo_V201.bin` 落盘 → 终止进程（硬杀无损：finalize 路径一次性写齐，见 §1 文件机制）。
3. **加载 A2L**：桥接层解析 `cpp_demo_V201.a2l`，零错误；符号表（名→地址/扩展/类型/换算）抽取。demo 绑 {0,0,0,0} 无 TRANSPORT_LAYER（F4）→ 端点由示例自备 127.0.0.1:5555。
4. **Run B（测量轮）**：同 run-dir 重启 `cpp_demo`（检测已有 .a2l → 跳过生成，.bin 复用）→ CONNECT → 读取（SHORT_UPLOAD 为主，按 A2L 符号地址/扩展；DAQ 事件路径视 V2 结论可选）→ 断言集 → **写回读**：SHORT_MODIFY 写 counter_max=1024 → 读回 → 恢复 1000 → 读回（CalSeg 寻址按 V3 实然；写回读口径对齐批次18-19 既有写用例路径；"second lock after write" 可见性延迟如需轮询则给上限）。
5. `--watch`（可选体验段）：Run B 断言通过后持续轮询打印 temperature/speed/SigGen 等动态值，展示实时测量。

断言集（示例内置"流程正确性"检查，非 gtest）：CONNECT/GET_STATUS 实然 OK；Run A 盘上产物出现且 Run B 事件/参数编号与 Run A 一致（V1）；A2L 加载零错误、默认符号集全部可解析；UPLOAD 字节=磁盘（口径沿用批次21 FetchA2lViaUploadMatchesDiskFile：GET_ID CRO 实然 2 字节 [0xFA][IDT]）；静态符号钉值（counter_max=1000、delay_us=1000、lookup 按 V4 实然）；动态符号范围/趋势（temperature∈[0,150]、speed∈(0,250)、波形 |v|≤ampl）；写回读两轮往返一致且现场恢复。

README 主线 = 单命令用法；同时解释其内部即你提出的两次运行工作流（第一次生成、第二次测量），进程管理由示例自动完成。不做手动两步模式（避免双路径）。

## 4. 核证任务（实施期先实证，V 系列）

| # | 待核证 | 影响 |
|---|---|---|
| V1 | Run B 复用 .bin 启动、事件/参数编号冻结（EPK V201） | 两次运行成立性 |
| V2 | 栈寻址/实例相对寻址符号**跨运行**读取实然 | 默认符号集与 --watch 内容；不可达则 README 如实标注排除，不硬造断言 |
| V3 | CalSeg 地址扩展方案（CASDD/ACSDD，查 OPTION_CAL_SEGMENTS_ABS）+ SHORT_MODIFY 写 calseg 后读回可见性实然 | 钉值寻址与写回读环节实现细节 |
| V4 | A2L 实际符号/类型/换算快照 | 默认符号集、UPLOAD 断言范围 |
| V5 | XCP_MODE_LOCAL 对命令集实然限制 | 会话步骤期望 |
| V6 | 5555 被占时行为 | 示例预检：探测失败 → 明确报错 exit 2（区别于断言失败 exit 1） |
| V7 | Run B 首个 CONNECT 再触发 finalize：已有 .a2l 时是否重写/字节一致（a2l.c:1540-1547 语义） | Run A 终止与产物时序断言、--watch 段安全 |

## 5. 交付物与验收（DoD）

1. `examples/` 目录及 §2 所列文件；根 CMakeLists 仅追加门+add_subdirectory（不重构既有段）。
2. 中文 doxygen 注释；README 含单命令用法与两次轮次原理说明。
3. 验收：
   - `cmake-build-xcplite` 追加 `-DLIBXCP_BUILD_EXAMPLES=ON`（前置门见 §2）：`xcp_master_udp` 零参数端到端 exit 0；`--watch` 人工可见实时变化值；写回读环节往返一致且恢复原值；
   - ctest 注册一条 `ExampleXcpMasterUdp`（非 gtest 裸可执行 add_test + RUN_SERIAL；口径按 C4 定夺后执行）；`-R Xcplite` 旧 29 例不回归；全量门禁 ≥394（+1），无新增 FAIL；
   - OFF 基线 352/352 不变（新门默认 OFF）。
4. 实施记录：`code-plan/XCPlite_cpp_demo_示例对手端_实施记录_批次22.md`（V1–V7 结论、实然排除项如实记录）。

## 6. 非目标

- 不修改 thirdparty/XCPlite 任何文件；不启用上游 XCPLITE_BUILD_EXAMPLES。
- 不迁移/改动批次17–21 测试与夹具；本次不以 gtest 形态交付对手测试。
- 不覆盖 TCP；不做 Seed&Key（上游不支持，xcplite.c:2195-2224 #if 0，批次17-21 既有结论）；不做 A2L 写侧（写回读只是 XCP 写命令+读回验证，非 A2L 生成侧）。
- 示例不进 libxcp 静态库（examples 独立可执行，保持"单独示例程序"）。

## 7. 确认项

已定（本轮形态问答答复）：**C1** 目录/目标 = `examples/xcp_master_udp`；**C3** 运行形态 = 单命令自动两轮；**C6** 写回读 = 纳入（默认执行，`--no-writeback` 可跳过）。

仍待定夺（给推荐默认，可整体批准）：
- **C2** 依赖门：按 §2——`LIBXCP_BUILD_EXAMPLES=ON` 要求 `LIBXCP_BUILD_XCPLITE_SLAVE=ON` 且 `LIBXCP_BUILD_A2L=ON`，否则 configure 报错提示（推荐，复用既有挂接、不二次 add_subdirectory）。
- **C4** ctest：注册一条 `ExampleXcpMasterUdp` 冒烟（推荐；RUN_SERIAL 下端口冲突罕见，被占按 V6 exit 2 → 计 FAIL 暴露环境问题；若嫌扰可改为端口占用 SKIP）。
- **C5** 批次号 = 22？
