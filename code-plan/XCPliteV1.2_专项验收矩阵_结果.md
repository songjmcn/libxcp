# XCPlite V1.2 专项验收矩阵——执行结果

- 执行日期：2026-10-28
- 执行环境：Windows，当前 checkout `D:\project\libxcp`
- 分支：`refactor/tcp-udp-platform-isolation`
- 验收对象：`examples/xcp_slave_demo_v12`（基于 `thirdparty/XCPliteV1.2`）
- 本轮目标：延续 A2L 覆盖修复与 TCP/UDP 基础闭环，新增 libxcp Master 驱动的 V1.2 测量读取和标定写回/回读验收。
- 本轮未修改 `thirdparty/XCPliteV1.2`；新增 C++ 测试程序负责启动真实 V1.2 从站子进程。

> **本轮结论：Windows V1.2 A2L/TCP/UDP/测量读取/标定写回基础链路均通过；V1.2 全功能验收仍未完成。**
>
> CTest 17/17 通过：模型 12 项，A2L 生成覆盖与解析 1 项，Master TCP/UDP 测量及标定 4 项。POLL 不属于 XCPlite V1.2：`0xFC` 是 `SYNCH`，本矩阵不把 POLL 当作 V1.2 通过条件。DAQ、多 ODT/错误边界、双进程并行、Linux/macOS 仍未完成。

## 1. 本轮实现变更

### 1.1 Windows A2L 覆盖替换

`examples/xcp_slave_demo_v12/main.cpp:524-570` 新增 `ReplaceA2lFile(...)`：

- Windows 优先调用 `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`，支持覆盖已存在的 XCPlite 生成文件；
- 若 Windows 受限工作区返回 `ERROR_ACCESS_DENIED`，回退到 `std::filesystem::copy_file(..., overwrite_existing)`，再删除临时文件；此回退不保证原子性；
- POSIX 继续使用同文件系统内的 `std::filesystem::rename`；
- 仍保持“临时文件写入 → 回读校验 → 覆盖替换”的顺序；替换失败返回退出码 4。

这解决了上一轮确认的 Windows 语义问题：目标 A2L 已存在时，`std::filesystem::rename(temporary, target, error)` 不会覆盖目标，原错误为：

```text
替换 A2L 失败：Access is denied.
```

### 1.2 V1.2 C++ Master 跨进程门禁

新增 `tests/xcp_v12_interop_test.cpp` 与 `tests/xcp_v12_slave_process.cpp/.hpp`，由
`examples/xcp_slave_demo_v12/CMakeLists.txt` 注册 `XcpliteV12InteropTest`。测试程序与从站分进程运行，
从站 target 仍独立链接 `thirdparty/XCPliteV1.2`；测试 target 只链接 libxcp Master/a2lbridge，不会把 V1.2/V2.2 两套同名 C 符号放进同一进程。

覆盖包括：

1. TCP 与 UDP 各在同一目录连续生成 A2L 两次，使用 `--verify-a2l` 验证覆盖；检查协议端点、AML 外链并再由 a2lbridge 加载、查询 `fast`/`cal`；
2. TCP 与 UDP 各通过 libxcp XcpMaster 建连、DISCONNECT；
3. 通过 A2L `fast` 的地址与 extension 读取测量结构，确认计数递增且浮点值有限；
4. COPY_CAL_PAGE 参考页1→工作页0、SET_CAL_PAGE 切换 XCP 页；通过写入/回读 `cal.corrections[0]` 并观察 `fast.samples[0]-fast.sine` 验证标定值影响运行测量；
5. 子进程用 RAII 清理、CTest 60 秒超时，失败不作为 SKIP。

> 注意：XCPlite V1.2 的 `0xFC` 是 `SYNCH`，不是后续协议版本的 `POLL`。新测试使用 Master 支持的内存读写和 CAL page 命令，不把 POLL 误用于验收。

## 2. 执行证据

### 2.1 Release 构建

按仓库规则先清理，再使用 VS Developer Command Prompt 初始化 MSVC 环境后构建：

```powershell
cmake --build cmake-build-release --target clean
cmake --build cmake-build-release -j 4
```

结果：

```text
365/365 targets built successfully
```

构建中只有既有第三方/工具警告，没有新增编译错误。

### 2.2 V1.2 模型单测

```powershell
& '.\cmake-build-release\tests\XcpSlaveV12ModelTest.exe' --gtest_color=no
```

结果：

```text
[==========] 12 tests from 1 test suite ran.
[  PASSED  ] 12 tests.
```

判定：**通过**。覆盖默认标定、中性波形、快速/慢速事件、相位回绕、`run=0` 冻结、增益/偏置/限幅、数组修正、统计、确定性和标定快照回读。

### 2.3 V1.2 CTest 门禁

```powershell
ctest --test-dir cmake-build-release --output-on-failure -R "XcpliteV12|XcpSlaveV12ModelTest"
```

结果：

```text
17/17 tests passed, 0 tests failed
Total Test time = 3.13 sec
```

其中：模型 12/12；`XcpliteV12A2lGenerationTest.RewritesAndParsesA2lForTcpAndUdp` 1/1；Master 真实从站测量读取和标定写回 TCP/UDP 各 2 项，共 4/4。首次检查中误假设测量实例 address extension 为 0，实际 A2L 为 extension 3；按读到的 A2L 元数据修正测试后重新运行，17 项全通过。

判定：**Windows 上 A2L 双次覆盖、TCP/UDP Master 测量读取和标定写回/效果验证通过**。

### 2.4 相关回归结果

执行了 52 项 XCPlite/示例相关筛选回归：

```powershell
ctest --test-dir cmake-build-release -R "XcpliteV12|XcpliteProtocolTest|XcpliteWriteTest|XcpliteDaqTest|XcpliteA2lReadTest|XcpliteMeasurementTest|XcpliteTcpInteropTest|XcpSlaveDemoIntegrationTest" --output-on-failure
```

结果：**43 通过，9 失败**。9 项失败全部发生在既有 `XcpSlaveDemoIntegrationTest` 临时目录创建阶段，错误完全一致：

```text
create_directories: Access is denied.: "C:\Users\songj\AppData\Local\Temp\libxcp_xcp_slave_demo_..."
```

这 9 项未进入被测断言；失败点是当前执行环境对系统临时目录的 ACL，不是本轮 V1.2 门禁失败。V1.2 两项 CTest 已单独复跑并得到 2/2 通过。

## 3. 专项验收矩阵

状态含义：

- **通过**：本轮有直接、可复现的自动化或命令行证据；
- **部分通过**：关键前置/部分路径有证据，但目标仍有未覆盖条件；
- **未验证**：本轮没有足够的 V1.2 对端或自动化证据；
- **不适用/不纳入**：协议版本或当前范围不包含该项。

| # | 矩阵维度 | 验收内容 | 本轮结果 | 证据/边界 |
|---:|---|---|---|---|
| 1 | V1.2 模型 | 模型确定性、周期、波形、标定参数消费 | **通过** | `XcpSlaveV12ModelTest` 直接运行 12/12。 |
| 2 | A2L 结构与后处理 | A2L/AML、PL/TL、DAQ、事件、标定段、端点、`READ_WRITE` | **通过（Windows）** | 每种传输连续生成两次，示例 `--verify-a2l`、a2lbridge 加载/符号查询、端点/AML/READ_WRITE 检查均通过。 |
| 3 | UDP 真实进程 | V1.2 Slave 与 libxcp Master 真实 CONNECT、测量读取、标定、断开 | **通过（本轮范围）** | `XcpliteV12InteropTest` UDP 测量与标定各通过，使用独立从站子进程。 |
| 4 | TCP 真实进程 | V1.2 TL1.4 字节流与 libxcp Master 真实测量/标定闭环 | **通过（本轮范围）** | `XcpliteV12InteropTest` TCP 测量与标定各通过，使用独立从站子进程。 |
| 5 | POLL | 使用 `POLL` 读取测量 | **不适用/不纳入 V1.2** | V1.2 的 `0xFC` 是 `SYNCH`；本轮不把后续版本 POLL 误算成 V1.2 能力。 |
| 6 | V1.2 测量读取 | Master 按 A2L `fast` 地址/extension 读取结构体并核验计数与浮点值 | **通过（Windows）** | `XcpliteV12InteropTest.ReadsMeasurementThroughA2lAddress`：TCP/UDP 均通过真实 V1.2 从站。此用例证明当前测量读链，不代表所有数据类型/读取尺寸。 |
| 7 | DAQ | 动态 DAQ、双事件/多 ODT、首 ODT 32-bit timestamp、计数递增 | **未验证** | 现有 DAQ 证据主要来自 V2.2 对端和模型层，不能外推为 V1.2 真实进程通过。 |
| 8 | 标定写入 | XCP 侧工作页写入/UPLOAD 回读，观察 corrections 影响运行测量 | **通过（范围有限，Windows）** | TCP/UDP 均成功 COPY/SET 页，写回并读回 `corrections[0]`，观测 `samples[0]-sine` 达到写入修正量；其他标定字段/边界未覆盖。 |
| 9 | CAL page | 参考页1→工作页0 的 COPY_CAL_PAGE、SET_CAL_PAGE 和 GET_CAL_PAGE 核验 | **部分通过（Windows）** | 真实 V1.2 TCP/UDP Master 调用通过；只核验示例 Segment 0 与 XCP 侧页，未覆盖 ECU 侧切换/其他页拒绝条件。 |
| 10 | 双传输进程 | UDP 与 TCP 分别启动、独立端口、互不冲突 | **部分通过** | 两种单进程独立模式均通过；同一测试未并行启动两个 V1.2 进程。 |
| 11 | UDP 边界 | DLC/CTR、截断、对齐/尾部、多帧错误边界 | **部分通过** | 基本 V1.2 DLC/CTR 单帧真实互操作通过；完整畸形/边界矩阵仍由合成 framing 测试补齐。 |
| 12 | TCP 边界 | 字节流分片、合帧、V1.4 无尾帧、CTR/错误边界 | **部分通过** | 基础真实 TCP 会话通过；本轮没有让 XCPlite V1.2 对端确定性产生分片、错误 CTR 或异常断链。已有 libxcp loopback/decoder 测试不等同于 V1.2 独立对端。 |
| 13 | 可选 TL 子命令 | GET_SLAVE_ID、多播等可选 V1.5 TL 能力 | **不纳入 V1.2 基线** | V1.2 默认 PL=1.2/TL=1.4 单播；不把 V1.5 optional TL 当作 V1.2 通过条件。 |
| 14 | 跨平台 | Windows/Linux/macOS 分别运行模型和真实互操作 | **Windows 部分通过，其余未验证** | Windows 模型与 TCP/UDP 门禁通过；Linux/macOS 本轮未运行同一 CTest。历史手工证据仍只能作为参考。 |

## 4. 结论与发布标签

当前可以确认：

1. Windows 下 XCPlite V1.2 从站模型测试通过；
2. Windows 下 A2L 双次生成/覆盖、AML 外链、`READ_WRITE` 后处理、a2lbridge 解析与符号查询通过；
3. Windows 下真实 XCPlite V1.2 UDP/TCP Master 测量读取通过；
4. Windows 下真实 XCPlite V1.2 UDP/TCP 工作页标定写入/回读及测量影响通过，示例 CAL page 基本流程部分通过；
5. 之前的 Windows `Access is denied` A2L 覆盖阻断已修复，不是权限放宽后“偶然通过”；
6. 仍不能宣称“XCPlite V1.2 全功能兼容通过”，DAQ、协议/错误边界、并行双进程及 Linux/macOS 证据未齐。

发布标签：

- **V1.2 模型层：PASS**
- **V1.2 A2L 生成与桥接：PASS（Windows）**
- **V1.2 UDP 最小真实互操作：PASS（Windows）**
- **V1.2 TCP 最小真实互操作：PASS（Windows）**
- **V1.2 测量读取 + 示例标定写回：PASS（Windows；有限范围）**
- **V1.2 DAQ：UNVERIFIED**
- **V1.2 CAL page：PARTIAL（Windows；示例页操作通过，覆盖不完整）**
- **V1.5 optional TL/multicast：不属于 V1.2 基线**
- **Linux/macOS 同矩阵：UNVERIFIED**

## 5. 后续建议

1. 优先新增真实 V1.2 DAQ Master 闭环，覆盖动态 DAQ、双事件/多 ODT、DTO 与时间戳；
2. 用可控合成对端补齐 UDP/TCP 截断、CTR 回绕/重复/跳号、TCP 分片和 peer close 边界；
3. 在 Linux、macOS CI 运行相同 `XcpliteV12InteropTest` 和模型门禁；
4. 继续扩充标定数据类型、ECU/XCP 双侧页状态与拒绝条件；
5. 处理或隔离 `XcpSlaveDemoIntegrationTest` 使用系统临时目录时的 Windows ACL，避免环境错误污染相关回归报告。

相关文件：

- [本轮 TCP/UDP 能力补全实施记录](XCPliteV1.2_TCP-UDP能力补全_实施记录.md)
- [XCPlite V1.2 从站 README](../examples/xcp_slave_demo_v12/README.md)
- [V1.2 协议实现](../thirdparty/XCPliteV1.2/)
- [XCP Ethernet V1.5 非 IPv6 缺口可行性评估](XCP_Ethernet_V1.5_非IPv6缺口可行性评估_XCPliteV1.2.md)

---

**文档性质：** 本文件是本轮专项验收结果记录；它区分了“真实 V1.2 最小传输闭环通过”和“V1.2 全功能兼容尚未完成”，不把 V2.2 对端测试或历史手工探针证据升级为本轮 V1.2 全量通过。
