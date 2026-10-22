# libxcp —— XCP Master 协议栈（C++20）

`libxcp` 是一个跨平台（Windows / Linux / macOS）的 **XCP Master 静态库**，
命名空间 `calmcar::xcp`，CMake 目标 `libxcp::libxcp`（产物名 `libxcp.lib` /
`libxcp.a`）。面向汽车 ECU 标定与测量场景，提供从协议编解码、UDP/IP 传输、
DAQ 数据接收到 A2L 描述文件集成的完整 Master 侧能力。

> **版本声明（批次 G 收口）**：库版本 **2.0.0**。协议侧工程支持范围为
> **XCP PL V1.3 / V1.4 / V1.5**，传输侧为 **XCP on Ethernet TL V1.5**
> （IPv4 UDP 单播 Profile）。版本通过 `GET_VERSION` 运行时发现；
> 探测不可用时进入 `1.x-conservative` 保守能力集，**不伪造具体 Minor**。
> 详细能力与证据边界见 §5「能力矩阵与验收证据」。

## 1. 工程用途

| 能力 | 说明 | 状态 |
|------|------|------|
| XCP 协议核心（PL V1.3～V1.5） | CONNECT/DISCONNECT 会话状态机、CTO 命令编码、RES/ERR/EV/SERV 响应解析、Seed&Key 安全解锁 | ✅ |
| 版本发现（GET_VERSION） | Level-1 命令族 `0xC0,0x00`；PL/TL **独立**快照（reported/effective/source/confidence/capped）、冲突可见、重连重探 | ✅ 批次 B1 |
| XCP on UDP/IP Transport | 标准 4 字节 Transport Header（LEN/CTR），跨平台 IPv4 Socket，传输层抽象接口 `IXcpTransport`；TL V1.5 `PACKET_ALIGNMENT` 8/16/32 的 Tail 编解码 | ✅ 批次 B2 |
| 内存读写 | `MemoryAccess`：SHORT_UPLOAD 优先、必要时降级 SET_MTA+UPLOAD 分块；写路径 | ✅ |
| Dynamic DAQ 测量 | 变量名 → `MeasurementPlanner` 规划 → 动态 DAQ 落表 → DTO 信封/净荷解码 → 时间戳回卷外推 → `MeasurementFrame` 物理值帧回调 | ✅ v1.0 封板 |
| A2L 集成 | 经 A2L 桥接层（`libxcp::a2lbridge`，基于上游 a2llib）加载 A2L，变量名解析为地址 / CompuMethod / 事件通道 | ✅ 必编组件 |
| 标定（Calibration） | CAL/PAG 页切换、段管理、`MODIFY_BITS` 位级修改、写保护与访问权限错误分类 | ✅ 已实现 |
| Programming（刷写） | Flash 编程流程 | ⛔ 未实现 |
| CAN / CAN FD Transport | Vector / SocketCAN / Peak 等 | ⛔ 未实现（见 `code-plan/libxcp_measurement_architecture.md` §25/§26） |

**明确不支持（能力边界，可经 `XcpMaster::IsCapabilityUnsupported()` 查询）**：

| 能力 | 说明 |
|------|------|
| `DaqPackedMode` | `SET/GET_DAQ_PACKED_MODE`（`0xC0,0x01`/`0xC0,0x02`）为 V1.5 新特性，本实现未提供编码路径 |
| `Level1OtherSubcommand` | `0xC0` 族除 `GET_VERSION` 外的子命令均未实现 |
| `Ipv6Transport` | 本版本基础 Profile 只承诺 IPv4 UDP 单播 |
| `OptionalTlSubcommand` | 四个 Optional TL Subcommand 未实现 |

核心数据流：

```text
A2L 变量名 → MeasurementPlanner → Dynamic DAQ 配置 → ECU DTO
           → DtoEnvelopeDecoder → Timestamp/换算 → MeasurementFrame 回调
```

## 2. 代码结构

> 各模块的详细文档见 [docs/modules/ 模块文档索引](#22-模块文档索引)。

```text
libxcp/
├── CMakeLists.txt              # 根构建脚本：核心库 + A2L 栈（必编）+ 可选构建开关
├── include/libxcp/             # 公开头文件（API 2.0.0）
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
│   ├── a2l-sdk/                #   A2L 栈构建工程：主树经 add_subdirectory 直接编译（必编）
│   │   ├── a2lbridge/          #     语义适配层 libxcp_a2lbridge（C++20）
│   │   └── build-sdk.ps1       #     独立 SDK 开发入口：产出 liba2l.dll + 桥接库 + 头树
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
| A2L 组件（必编） | `adapter/a2l` + `thirdparty/a2l-sdk` | A2L 解析、语义适配、`IMeasurementDatabase` 实现 |

**依赖隔离**：核心库与测量路径不依赖 A2L——`IMeasurementDatabase` 是核心窄接口，
A2L 接入是独立 target（`adapter/a2l/A2lMeasurementDatabase` →
`libxcp_measurement_adapter`），虽随主工程必编，但隔离由 ctest `A2lIsolation`
门禁持续校验（扫描主树禁引用 A2L 栈 / 桥接层禁含上游头）。A2L 栈源码经
`add_subdirectory(thirdparty/a2l-sdk)` 随主工程直接编译，不再消费预构建准备根。

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
| [构建系统](docs/modules/构建系统.md) | CMake 目标、安装规则、构建开关与 Boost 硬依赖、已知构建注意事项 |
| [第三方依赖](docs/modules/第三方依赖.md) | a2llib / a2l-sdk / XCPlite 子模块消费方式与边界纪律 |
| [文档与工程规范](docs/modules/文档与工程规范.md) | `docs/`、`code-plan/`、`scripts/` 与 AGENTS.md 约定 |

## 3. 编译方法

### 3.1 环境要求

- CMake ≥ 3.24，C++20 编译器（Windows 默认 VS 2022 生成器，根脚本自动指定）
- 跑测试需 GoogleTest：在线 FetchContent 拉取；离线环境预置
  `.deps-cache/googletest/`（自动改用本地源）
- **Boost 为硬依赖**（A2L 栈必编所致）：CONFIG 包，组件 locale / filesystem /
  process；configure 期自动发现（显式 `-DBoost_DIR` > `C:\boost\lib\cmake\Boost-*`
  > `BOOST_ROOT` 环境变量），找不到直接报错并给出指引。本机装有 vcpkg 全局集成时
  其 Boost 缺 locale/process 组件不会被误选（查找已屏蔽环境注入路径），必要时设
  `VCPkgLocalAppDataDisabled=true`
- A2L 专项测试（`A2lSmoke` / `A2lGolden` / `A2lE2E` / `MeasurementAdapterTest` 等）
  另需 Python3

### 3.2 Release 编译（推荐）

A2L 栈（`liba2l` / `libxcp::a2lbridge` / `libxcp_measurement_adapter`）随主工程
必编，常规配置无需任何额外开关：

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j 4
```

> 注意：本机（CMake 4.0 + VS 生成器）下 `--build -j N` 偶发在 MSBuild 并行
> 解析项目引用阶段失败于 gtest → ZERO_CHECK（报"0 个错误"但退出码非 0）。
> 遇到时去掉 `-j` 串行构建即可，产物不受影响。
>
> 全新 build 目录的首次构建需编译上游 a2llib 全部翻译单元，耗时数分钟；
> 增量构建不受影响。子模块未初始化（`thirdparty/a2llib/CMakeLists.txt` 缺失）
> 时 configure 会给出明确报错，先执行 `git submodule update --init`。

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
| `LIBXCP_BUILD_XCPLITE_SLAVE` | OFF | 构建 XCPlite 回环 Slave（L3 集成测试载体） |
| `LIBXCP_BUILD_EXAMPLES` | OFF | 构建 `examples/`；开启时自动跟随打开 `LIBXCP_BUILD_XCPLITE_SLAVE`（无需手工同开两门） |
| `LIBXCP_ENABLE_LOG` | **OFF** | 启用 `LIBXCP_LOG_INFO/WARNING/ERROR`；ON 时要求 C++20 `<format>` 与 glog 0.7.1+ |

A2L 桥接层（`thirdparty/a2l-sdk` → `liba2l` / `libxcp::a2lbridge`）与测量适配器
`libxcp_measurement_adapter` 为**必编组件**，随主工程无条件构建，不再提供任何开关。
Boost 自动发现规则：显式 `-DBoost_DIR=<目录>` > Windows 下 `C:\boost\lib\cmake\Boost-*`
取版本最新 > `BOOST_ROOT` 环境变量探测；找不到直接报错并给出指引。如需手工指定
Boost 位置：

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release `
      -DBoost_DIR=C:/boost/lib/cmake/Boost-1.86.0
```

`thirdparty/a2l-sdk/build-sdk.ps1` 保留为独立 SDK 开发入口（单独产出预构建
SDK 根的场景），不再是主树构建的前置步骤。

### 3.6 日志功能

日志默认关闭；关闭时不查找或下载 glog，三个日志宏也不会求值参数。启用方式：

```bash
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release -DLIBXCP_ENABLE_LOG=ON
cmake --build cmake-build-release -j 4
```

启用后要求编译器/标准库支持 C++20 `<format>`。构建优先复用 glog 0.7.1+ CONFIG
包；找不到时按固定版本和校验值通过 FetchContent 按需获取，也可用
`FETCHCONTENT_SOURCE_DIR_GLOG` 指定本地源码目录。`libxcp::libxcp` 会向同一 CMake
构建中的消费者传递启用宏和 glog 链接依赖。

使用日志宏时包含 `libxcp/log.hpp`，格式串使用 `{}`：

```cpp
#include "libxcp/log.hpp"

LIBXCP_LOG_INFO("XCP packet bytes={}", packet_size);
LIBXCP_LOG_WARNING("retry={}", retry_count);
LIBXCP_LOG_ERROR("operation failed: {}", reason);
```

libxcp 核心库不初始化或关闭进程级 glog。启用日志的外部宿主需在工作线程启动前
初始化 glog，并在相关对象/线程销毁后关闭；若要求文件输出而不在终端显示，应在
初始化前设置 `FLAGS_log_dir`、`FLAGS_logtostderr=false`、
`FLAGS_alsologtostderr=false`，并把 `FLAGS_stderrthreshold` 设为高于 ERROR。
本项目自有 tools/examples/tests 已由入口辅助代码配置为文件输出，日志目录相对
进程当前工作目录为 `logs/<模块>/<可执行程序名>/`；文件名按级别生成
`<程序名>.<级别>.<时间>.<PID>.glog`，不以点开头，也不创建无 `.glog` 后缀的默认软链接。
普通 CLI、示例结果和 GoogleTest 报告仍使用原有终端输出。

单独链接已安装的静态库时，本项目当前没有导出的 CMake package target；日志开启时
消费方需定义 `LIBXCP_ENABLE_LOG=1` 并自行链接 glog 0.7.1+。运行期间若底层文件系统
写入失败，日志后端自身诊断行为不由 libxcp 保证；核心日志宏会隔离格式化/写日志异常，
不会将日志消息改为另一条业务输出通道。

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
  `xcplite_measurement_test`）随本层默认可用（A2L 栈必编）；
- **A2L 专项**（随 `LIBXCP_BUILD_TESTS=ON` 默认可用）：`A2lSmoke`、`A2lGolden`
  （黄金回归）、`A2lE2E`（端到端）、`MeasurementAdapterTest`（适配器单测）、
  `ParseLargeFileUnderBudget`（性能基线）、`A2lIsolation`（依赖隔离静态门禁）；
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
（默认启动 examples 内的 cpp_demo → UPLOAD 取 A2L → 适配器 → 会话全链路，含 RAII 守卫范式；无需开启 tests）。

## 6. 公开头文件

> **API/ABI 说明**：库版本已升至 **2.0.0**，含破坏性变更（`XcpMaster` 构造
> 接口由 `XcpMasterOptions` 替换）。迁移清单见
> [`code-plan/XCP_V1.5_批次B1_实施记录.md`](code-plan/XCP_V1.5_批次B1_实施记录.md)
> 与 §5.2。下列 v1.0 子面保持不变。

- `include/libxcp/daq/`：`dto_envelope_types.hpp`、`dto_envelope_decoder.hpp`、
  `dto_payload_decoder.hpp`、`daq_timestamp.hpp`
- `include/libxcp/measurement/`：`measurement_types.hpp`、`measurement_result.hpp`、
  `measurement_database.hpp`、`measurement_planner.hpp`、`measurement_sample.hpp`、
  `measurement_session.hpp`
- 既有协议核心（`xcp_master.hpp` 等）仅追加式扩展，签名未变。

封板与已知限制记录：`code-plan/libxcp_测量子系统_v1.0_封板记录.md`；
各里程碑实施记录见 `code-plan/libxcp_测量子系统_v0.*_记录.md`。

## 7. 文档资源

- `docs/`：XCP 规范与中文速查——PL 基线
  `docs/ASAM_XCP_Part2_Protocol_Layer_1.3.md`、PL V1.5
  `docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md`（**以同目录原始 PDF 为
  权威准据**，Markdown 转换版存在图文错位）、TL V1.5
  `docs/ASAM_XCP_Part3_XCP_on_Ethernet_TCP_UDP_1.5.md`（Dev 版，仅作过渡）、
  `XCPlite_对手端协议调试指南.md`；**模块文档索引见
  [§2.2](#22-模块文档索引)（`docs/modules/`）**
- `code-plan/`：详细设计（`XCP_1.3.0_详细设计_类接口与头文件.md`、
  `libxcp_measurement_architecture.md`、A2L 集成 R4 系列）、各批次实施记录，
  以及 V1.5 工程三份入口文档——规范符合性裁决与修复方案
  （`XCP_V1.5_规范符合性裁决与修复方案.md`）、总进度与下一步修改计划
  （`XCP_V1.5_总进度与下一步修改计划.md`）、批次 G 实施记录
  （`XCP_V1.5_批次G_实施记录.md`）
- 开发规范：`AGENTS.md`（C++20、Doxygen 中文注释、测试与文档要求）；格式：`.clang-format`
