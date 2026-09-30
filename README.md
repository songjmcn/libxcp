# libxcp —— XCP 1.3.0 Master 协议栈（C++20）

`libxcp` 是一个跨平台（Windows / Linux / macOS）的 **XCP Master 静态库**，
命名空间 `calmcar::xcp`，CMake 目标 `libxcp::libxcp`（产物名 `libxcp.lib` /
`libxcp.a`）。面向汽车 ECU 标定与测量场景，提供从协议编解码、UDP/IP 传输、
DAQ 数据接收到 A2L 描述文件集成的完整 Master 侧能力。

## 1. 工程用途

| 能力 | 说明 | 状态 |
|------|------|------|
| XCP 1.3.0 协议核心 | CONNECT/DISCONNECT 会话状态机、CTO 命令编码、RES/ERR/EV/SERV 响应解析、Seed&Key 安全解锁 | ✅ |
| XCP on UDP/IP Transport | 标准 4 字节 Transport Header（LEN/CTR），跨平台 IPv4 Socket，传输层抽象接口 `IXcpTransport` | ✅ |
| 内存读写 | `MemoryAccess`：SHORT_UPLOAD 优先、必要时降级 SET_MTA+UPLOAD 分块；写路径 | ✅ |
| Dynamic DAQ 测量 | 变量名 → `MeasurementPlanner` 规划 → 动态 DAQ 落表 → DTO 信封/净荷解码 → 时间戳回卷外推 → `MeasurementFrame` 物理值帧回调 | ✅ v1.0 封板 |
| A2L 集成 | 经 A2L 桥接层（`libxcp::a2lbridge`，基于上游 a2llib）加载 A2L，变量名解析为地址 / CompuMethod / 事件通道 | ✅ 可选组件 |
| Calibration / Programming | 标定写入、刷写 | ⛔ 不在 v1.0 承诺范围 |
| CAN / CAN FD Transport | Vector / SocketCAN / Peak 等 | ⛔ v1.0 之后的下一步（见 `code-plan/libxcp_measurement_architecture.md` §25/§26） |

核心数据流：

```text
A2L 变量名 → MeasurementPlanner → Dynamic DAQ 配置 → ECU DTO
           → DtoEnvelopeDecoder → Timestamp/换算 → MeasurementFrame 回调
```

## 2. 代码结构

> 各模块的详细文档见 [docs/modules/ 模块文档索引](#22-模块文档索引)。

```text
libxcp/
├── CMakeLists.txt              # 根构建脚本：核心库 + 4 个可选开关
├── include/libxcp/             # 公开头文件（v1.0 冻结面）
│   ├── xcp_master.hpp          #   Master 顶层门面，用户唯一入口
│   ├── session.hpp             #   会话状态机与协商参数管理
│   ├── command_codec.hpp       #   高层参数 → CTO 字节序列编码
│   ├── response_parser.hpp     #   Packet 分类与字段提取
│   ├── command_executor.hpp    #   编码→发送→等响应→超时恢复
│   ├── memory_access.hpp       #   高层内存读（SHORT_UPLOAD / SET_MTA+UPLOAD）
│   ├── protocol_types.hpp      #   协议常量、强类型别名、基础枚举
│   ├── xcp_error.hpp           #   统一错误分类与异常类型
│   ├── ixcp_transport.hpp      #   传输层抽象接口与包监听器
│   ├── udp_transport.hpp       #   标准 XCP on UDP/IP 实现
│   ├── udp_header_codec.hpp    #   4 字节 LEN/CTR Transport Header 编解码
│   ├── udp_transport_config.hpp#   UDP Transport 配置参数
│   ├── daq/                    #   DTO 解码子层（A2L-free）
│   │   ├── dto_envelope_types.hpp     # 识别字段模式 / 运行时信封布局
│   │   ├── dto_envelope_decoder.hpp   # 按布局切出 DTO 信封
│   │   ├── dto_payload_decoder.hpp    # ODT 净荷字节级切片解码
│   │   └── daq_timestamp.hpp          # 原始计数 → 单调纳秒，含回卷延展
│   └── measurement/            #   测量子系统（A2L-free 核心窄接口）
│       ├── measurement_database.hpp   # IMeasurementDatabase 窄接口
│       ├── measurement_planner.hpp    # 测量规划器
│       ├── measurement_session.hpp    # 高级用户 API：DTO 流 → 物理值帧
│       ├── measurement_types.hpp      # 核心值类型
│       ├── measurement_sample.hpp     # 单样本与帧模型
│       └── measurement_result.hpp     # 结果封装类型
├── src/                        # 核心库实现（与 include 一一对应）
│   ├── daq/                    #   dto_envelope_decoder / dto_payload_decoder / daq_timestamp
│   └── measurement/            #   measurement_planner / measurement_session
├── adapter/a2l/                # A2L 测量适配器（核心↔桥接层唯一接触点）
│   └── a2l_measurement_database.*   # 包装桥接层 IA2lDatabase → 核心窄接口
├── thirdparty/                 # 第三方依赖（未经明确指示不得修改）
│   ├── a2llib/                 #   git 子模块：上游 A2L 解析库（MIT）
│   ├── a2l-sdk/                #   A2L 栈构建工程：build-sdk.ps1 一键产出准备根
│   │   ├── a2lbridge/          #     语义适配层 libxcp_a2lbridge（C++20）
│   │   └── build-sdk.ps1       #     产出 liba2l.dll + 桥接库 + 头树
│   └── XCPlite/                #   git 子模块：Vector 的开源 XCP Slave（测试对手端）
├── tests/                      # 测试用例（GoogleTest）与测试设施
│   ├── a2l_gen/gen_a2l.py      #   Python 黄金 A2L 生成器（ctest fixture）
│   ├── data/a2l/               #   A2L 测试数据
│   ├── xcp_test_slave/         #   基于 XCPlite 的独立 Slave 进程
│   └── *_test.cpp              #   L1/L2 单元与 mock 传输测试
├── examples/                   # 交付用示例程序（不进静态库）
│   ├── xcp_master_udp/         #   Master 连接 XCPlite cpp_demo 对手端（两轮自动）
│   └── measurement_demo/       #   变量名 → 物理值帧端到端演示（含 RAII 守卫范式）
├── docs/                       # 协议规范、中文速查、模块文档（见 docs/modules/）
├── code-plan/                  # 设计文档与各批次实施记录（markdown）
├── scripts/                    # 代码质量审计脚本（doxygen 覆盖率、命名、注释等）
└── .deps-cache/                # 离线预取的 googletest v1.15.2 源码（可选）
```

### 2.1 分层架构

| 层 | 组成 | 职责 |
|----|------|------|
| 门面层 | `XcpMaster` | 用户唯一入口，持有 Transport 与 Session |
| 会话层 | `Session` / `CommandExecutor` | 状态机、单 Pending Command、超时与恢复 |
| 编解码层 | `CommandCodec` / `ResponseParser` / `ProtocolTypes` / `XcpError` | CTO 编码、RES/ERR/EV/SERV/DTO 解析、错误分类 |
| DAQ 解码层 | `daq/` 四个头文件 | DTO 信封切分、ODT 净荷解码、时间戳换算（A2L-free） |
| 测量层 | `measurement/` | 规划 → DAQ 配置 → 物理值帧回调（A2L-free） |
| 传输层 | `IXcpTransport` → `UdpTransport` | 抽象接口 + 标准 UDP/IP 实现（预留 CAN 系实现位） |
| A2L 组件（可选） | `adapter/a2l` + `thirdparty/a2l-sdk` | A2L 解析、语义适配、`IMeasurementDatabase` 实现 |

**依赖隔离**：核心库与测量路径不依赖 A2L——`IMeasurementDatabase` 是核心窄接口，
A2L 接入是独立可选组件（`adapter/a2l/A2lMeasurementDatabase`），隔离由 ctest
`A2lIsolation` 门禁持续校验（扫描主树禁引用 A2L 栈 / 桥接层禁含上游头）。
主工程不编译任何 thirdparty 源码，只以 IMPORTED target 消费准备根产物。

### 2.2 模块文档索引

每个模块的独立文档位于 `docs/modules/`：

| 模块文档 | 覆盖内容 |
|----------|----------|
| [代码结构总览](docs/modules/代码结构总览.md) | 顶层目录、分层架构、公开头文件清单、核心数据流 |
| [协议核心](docs/modules/协议核心.md) | 会话状态机、命令编解码、执行器、内存访问、错误模型、顶层门面 |
| [传输层](docs/modules/传输层.md) | `IXcpTransport` 抽象、UDP/IP 实现、LEN/CTR Header、CTR 接收策略 |
| [DAQ 解码](docs/modules/daq解码.md) | DTO 信封/净荷解码、识别字段模式、时间戳回卷延展（A2L-free） |
| [测量子系统](docs/modules/测量子系统.md) | `IMeasurementDatabase` 窄接口、规划器、`MeasurementSession` 帧回调 |
| [A2L 集成](docs/modules/A2L集成.md) | 适配器、桥接层 `a2lbridge`、隔离门禁、构建与测试 |
| [测试](docs/modules/测试.md) | L1-L4 分层、测试文件清单、测试设施、ctest fixture |
| [示例程序](docs/modules/示例程序.md) | `xcp_master_udp`、`measurement_demo` 两轮流程与断言口径 |
| [构建系统](docs/modules/构建系统.md) | CMake 目标、安装规则、4 个构建开关、已知构建注意事项 |
| [第三方依赖](docs/modules/第三方依赖.md) | a2llib / a2l-sdk / XCPlite 子模块消费方式与边界纪律 |
| [文档与工程规范](docs/modules/文档与工程规范.md) | `docs/`、`code-plan/`、`scripts/` 与 AGENTS.md 约定 |

## 3. 编译方法

### 3.1 环境要求

- CMake ≥ 3.24，C++20 编译器（Windows 默认 VS 2022 生成器，根脚本自动指定）
- 跑测试需 GoogleTest：在线 FetchContent 拉取；离线环境预置
  `.deps-cache/googletest/`（自动改用本地源）
- A2L 相关目标另需 Python3；构建准备根需 Boost（经 `-BoostRoot` 传入）

### 3.2 Release 编译（推荐）

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j 4
```

> 注意：本机（CMake 4.0 + VS 生成器）下 `--build -j N` 偶发在 MSBuild 并行
> 解析项目引用阶段失败于 gtest → ZERO_CHECK（报"0 个错误"但退出码非 0）。
> 遇到时去掉 `-j` 串行构建即可，产物不受影响。

### 3.3 Debug 编译（仅代码调试用）

```bash
cmake -B cmake-build-debug -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug -j 4
```

### 3.4 安装与清理

```bash
# 安装（release 推荐）
cmake --install cmake-build-release
cmake --install cmake-build-debug

# 清理（完整编译前建议先清理一次）
cmake --build cmake-build-release --target clean
cmake --build cmake-build-debug --target clean
```

### 3.5 构建开关

| 开关 | 默认 | 作用 |
|------|------|------|
| `LIBXCP_BUILD_TESTS` | **ON** | GoogleTest 单测/集成测试（ctest 套件） |
| `LIBXCP_BUILD_A2L` | OFF | 消费 A2L 桥接构件并构建 `libxcp_measurement_adapter`；必须同时指定 `LIBXCP_LIBA2L_ROOT` 指向准备根（无环境变量回退猜测） |
| `LIBXCP_BUILD_XCPLITE_SLAVE` | OFF | 构建 XCPlite 回环 Slave（L3 集成测试载体） |
| `LIBXCP_BUILD_EXAMPLES` | OFF | 构建 `examples/`（要求上面两门同开，缺位在 CONFIGURE 期直接 FATAL_ERROR） |

A2L 准备根一键构建（相对路径即可，脚本自行换算）：

```powershell
pwsh -File thirdparty/a2l-sdk/build-sdk.ps1 -BoostRoot <boost前缀> `
     -Config Release -OutRoot build/liba2l-prepared
```

随后配置主树：

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release \
      -DLIBXCP_BUILD_A2L=ON \
      -DLIBXCP_LIBA2L_ROOT=build/liba2l-prepared/msvc-x64-Release
```

## 4. 测试

```bash
ctest --test-dir cmake-build-release -C Release --output-on-failure
```

> 建议用 release 版本跑测试；**不要运行 `install/Debug/bin` 下的所有测试**，
> 会导致测试时间大大增加。

测试分层：

- **L1 单元**（`libxcp_tests`）：编解码器、解码器、规划器、时间戳、错误分类
  （`command_codec_test`、`dto_envelope_decoder_test`、`measurement_planner_test`、
  `daq_timestamp_test` 等）；
- **L2 mock transport**：`mock_transport.hpp` 模拟链路的会话状态机与恢复测试
  （`session_test`、`recovery_test`、`xcp_master_integration_test`）；
- **L3 集成**（需 `LIBXCP_BUILD_XCPLITE_SLAVE=ON`）：真实 UDP + XCPlite Slave
  子进程回环（`XcpliteIntegration`）；A2L 用例（`xcplite_a2l_read_test`、
  `xcplite_measurement_test`）另需 `LIBXCP_BUILD_A2L=ON`；
- **A2L 专项**（需 `LIBXCP_BUILD_A2L=ON`）：`A2lSmoke`、`A2lGolden`（黄金回归）、
  `A2lE2E`（端到端）、`ParseLargeFileUnderBudget`（性能基线）、
  `A2lIsolation`（依赖隔离静态门禁）；
- **L4 真 ECU 冒烟清单**见 `code-plan/libxcp_测量子系统_v0.9_记录.md` §6。

## 5. 测量快速上手（变量名 → 帧回调）

```cpp
using namespace calmcar::xcp;
MeasurementSession session(database);          // IMeasurementDatabase（可用 A2L 适配器）
session.Bind(master);                          // master 以 &session 作 listener 构造
session.SetEnvelopeMode(IdentificationFieldType::RelativeWord, 2U);
session.Add("EngineSpeed");                    // 全库唯一名或规范键
session.Prepare();                             // 规划 + 动态 DAQ 落表
session.Start([](const MeasurementFrame& f) {  // 物理值帧回调（worker 线程）
    for (const auto& s : f.samples) { /* s.name / s.value / s.valid */ }
});
// ... 采集期间可查 session.Statistics()（received/dropped/decode_errors/wraps）
session.Stop();
```

时间戳语义：Slave 若只对事件首 ODT 帧盖时间戳（XCPlite 实然），
在 `Prepare()` 前调用 `session.SetTimestampFirstOdtOnly(true)`，
并经 `SetTimestampUnit(ns_per_tick)` 注入分辨率；回卷由内部
`DaqTimestampConverter` 外推并计入 `Statistics().timestamp_wraps`。

端到端可运行样例：`examples/measurement_demo/measurement_demo.cpp`
（启动 XCPlite Slave → UPLOAD 取 A2L → 适配器 → 会话全链路，含 RAII 守卫范式）。

## 6. 公开头文件（v1.0 冻结面）

- `include/libxcp/daq/`：`dto_envelope_types.hpp`、`dto_envelope_decoder.hpp`、
  `dto_payload_decoder.hpp`、`daq_timestamp.hpp`
- `include/libxcp/measurement/`：`measurement_types.hpp`、`measurement_result.hpp`、
  `measurement_database.hpp`、`measurement_planner.hpp`、`measurement_sample.hpp`、
  `measurement_session.hpp`
- 既有协议核心（`xcp_master.hpp` 等）仅追加式扩展，签名未变。

封板与已知限制记录：`code-plan/libxcp_测量子系统_v1.0_封板记录.md`；
各里程碑实施记录见 `code-plan/libxcp_测量子系统_v0.*_记录.md`。

## 7. 文档资源

- `docs/`：XCP 1.3.0 规范与中文速查、ASAM Part 3（XCP on Ethernet TCP/UDP）、
  `XCPlite_对手端协议调试指南.md`；**模块文档索引见
  [§2.2](#22-模块文档索引)（`docs/modules/`）**
- `code-plan/`：详细设计（`XCP_1.3.0_详细设计_类接口与头文件.md`、
  `libxcp_measurement_architecture.md`、A2L 集成 R4 系列）与各批次实施记录
- 开发规范：`AGENTS.md`（C++20、Doxygen 中文注释、测试与文档要求）；格式：`.clang-format`
