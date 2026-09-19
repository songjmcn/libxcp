# XCP 1.3.0 实施记录 — 批次 1（阶段 0~2：工程基线 + 协议类型 + Codec/Parser/Session）

> 依据文档：`code-plan/XCP_1.3.0_详细设计_类接口与头文件.md`（第 2~5、9~11、15.1 节）、
> `code-plan/XCP_1.3.0_最小协议核心实现计划.md`（阶段 0、1、2）。
>
> 本批次范围经用户确认：**只做阶段 0~2**，Transport（UdpTransport/UdpHeaderCodec）、
> CommandExecutor、MemoryAccess、XcpMaster、UdpTestSlave 及 UDP Loopback 测试留待下一批。

---

## 1. 本次新增/修改的文件

### 1.1 工程基线（阶段 0）

| 文件 | 说明 |
|---|---|
| `CMakeLists.txt` | 顶层构建脚本。C++20、`libxcp` 静态库目标、别名 `libxcp::libxcp`、安装规则、`enable_testing()` |
| `tests/CMakeLists.txt` | GoogleTest（FetchContent）+ `libxcp_tests` 可执行目标 + `gtest_discover_tests` |
| `.gitignore` | 忽略 `cmake-build-*/`、`.deps-cache/`、IDE 产物 |
| `.clang-format` | **用户提供**（Google 基础 / IndentWidth 4 / PointerAlignment Left / SortIncludes false），本批次未修改，仅据其格式化代码 |

### 1.2 公共头文件（`include/libxcp/`）

| 文件 | 设计章节 | 内容 |
|---|---|---|
| `protocol_types.hpp` | §3 | 强类型别名（`Bytes`/`BytesView`/`Address`/`ElementCount`/`ByteCount`/`DatagramCtr`/`DatagramLen`）、`CommandCode`、`PacketType`、`ErrorCode`、`EventCode`、`ByteOrder`、`AddressGranularity`、`Resource`+`ResourceMask`、`SessionState`、`ConnectResponse`、`GetStatusResponse`、`GetCommModeInfoResponse`、`XcpAddress40`、`SessionParameters` |
| `xcp_error.hpp` | §4 | `ErrorCategory`、`XcpException`（继承 `std::runtime_error`）、`detail::make*` 便捷构造 |
| `ixcp_transport.hpp` | §5 | `IPacketListener`、`IXcpTransport` 抽象接口 |
| `command_codec.hpp` | §9 | `CommandCodec`：8 条命令的 CTO 编码 |
| `response_parser.hpp` | §10 | `PositiveResponse`/`NegativeResponse`/`EventPacket`/`ServicePacket`/`DtoPacket`、`ParsedPacket` 变体、`ResponseParser` |
| `session.hpp` | §11 | `Session` 状态机 + 协商参数 + 单 Outstanding Command 约束 |

所有头文件使用设计文档 §2.2 规定的传统宏守卫（如 `CALMCAR_XCP_SESSION_HPP_`），
全部公共类型/函数/成员带中文 Doxygen 注释。

### 1.3 实现文件（`src/`）

`protocol_types.cpp`、`xcp_error.cpp`、`command_codec.cpp`、`response_parser.cpp`、`session.cpp`

### 1.4 测试（`tests/`）

| 文件 | 覆盖 |
|---|---|
| `protocol_types_test.cpp` | PID 分类、AG 换算与 COMM_MODE_BASIC 位域往返、错误码/事件码名称与未知值防护、资源掩码、`XcpAddress40::advance` 溢出、状态名 |
| `xcp_error_test.cpp` | 异常字段保留、默认可选项、可作为 `std::runtime_error` 捕获、8 个分类工厂 |
| `command_codec_test.cpp` | 8 条命令黄金报文（Intel/Motorola）、reserved 填 0、NumberOfElements 边界 |
| `response_parser_test.cpp` | RES/ERR/EV/SERV/DTO 分类、附加信息保留、CONNECT/GET_STATUS/GET_COMM_MODE_INFO 字段解析、截断与保留位拒绝 |
| `session_test.cpp` | 全生命周期迁移、非法迁移拒绝、CONNECT 参数校验矩阵、单 Pending Command、能力降级持久性、快照不可变、并发冒烟 |
| `mock_transport.hpp` | 设计 §15.1 测试设施（本批次无消费者，供阶段 4~6 使用；已单独编译并做行为自检） |

---

## 2. 与设计文档的偏差（全部为显式记录，非静默改动）

| # | 设计文档 | 实际实现 | 理由 |
|---|---|---|---|
| 1 | §2.2.1 成员变量 `m_<snake>_` | 成员用 `<snake>_`（尾下划线） | 仓库现有 `.clang-format`（Google 风格）在列对齐时把 `m_xxx_` 误判为两个标识符，导致每次格式化都改写注释对齐（结果不稳定）。Google 官方约定即尾下划线。**这是本批次唯一需要用户裁决的风格项**，若坚持 `m_` 前缀可一次性改回。 |
| 2 | §4.2 `XcpException` 构造非 explicit | 加 `explicit` | 防止 `std::string` 隐式转成异常的意外转换；不影响任何调用点。 |
| 3 | §10 `NegativeResponse.errorCode : ErrorCode` | 拆为 `rawErrorCode : uint8_t` + `errorCode : optional<ErrorCode>` | 计划文档 §6.4「未知错误码须保留原值并上报」要求不丢原始字节；纯枚举无法表达未知值。`EventPacket` 同理。 |
| 4 | §10 `readU16/readU32` 返回值 | 改为 `optional<uint16_t>` 并新增 `readU8` | 设计原文返回裸值，但越界时无合法返回值可用；返回 nullopt 才能真正保证「畸形包不越界读」。 |
| 5 | §12 `CommandResult{ParsedPacket response;}` | 本批次未实现 | 属阶段 4（CommandExecutor），按分批决定推迟。 |
| 6 | §3.6 `bytesToAg(uint8_t)` | 改为 `commModeBasicToAg(field)` / `agToCommModeBasicField(ag)` | 协议里 AG 有两个不同表示：COMM_MODE_BASIC 的 bit1-2 编码（00/01/10）与字节数（1/2/4）。原签名混淆二者；拆分后单位明确，且能表达「11 为保留值 → 非法」。 |
| 7 | §3.7 只有 `hasResource` | 额外提供 `constexpr operator\|(Resource, Resource)` | 测试与后续 Session 资源判定需要组合位；未新增类或公共流程接口。 |
| 8 | §11 未列出的常量 | 新增 `kMaxCtoMinimum=0x08`、`kMaxDtoMinimum=0x0008` | 来自 ASAM XCP on Ethernet 1.1 Part 3 §1.4（本地 `docs/ASAM_XCP_Part3_..._1.1.md` 第 305-308 行），用于 CONNECT 校验。 |
| 9 | §11 `markCommandSent` 仅注明「已有 Pending 抛错」 | 额外拒绝 `Disconnected`/`Failed` 状态下发送 | 计划文档 §6.2「Transport 已断开时不执行 SYNCH」的前置保障；否则 SYNCH 恢复路径可在无效会话上发起。 |

> 除上述 9 项外，函数签名、枚举取值、结构体字段顺序与设计文档一致。

---

## 3. 规范校核结论（设计文档 §18.1 开放问题）

| 编号 | 问题 | 结论 | 证据 |
|---|---|---|---|
| Q1 | COMM_MODE_BASIC 精确位定义 | **确认与设计推断一致**：bit0=BYTE_ORDER(0=Intel/1=Motorola)、bit1-2=AG(00=BYTE/01=WORD/10=DWORD/11=保留)、bit6=SLAVE_BLOCK_MODE、bit7=OPTIONAL | ① 本地 `docs/XCP_1.3.0_document.md` §12.4 示例 `FF 15 C0 08 08 00 10 10` → Intel/BYTE/BlockMode/Optional，与 `0xC0` 逐位吻合；② 第三方实现交叉验证 [robotjatek/XCP ConnectPositivePacket.h](https://github.com/robotjatek/XCP/blob/master/XCPLib/ConnectPositivePacket.h) 中 `BYTE_ORDER=0x1, ADDRESS_GRANULARITY_0=0x2, _1=0x4, SLAVE_BLOCK_MODE=0x40, OPTIONAL=0x80` |
| Q2 | MAX_DTO 是 1 还是 2 字节 | **2 字节**，位于 RES Byte 4-5，按 Session Byte Order | 同上第三方实现 `MAX_DTO = 3, //2 bytes long!`，PROTOCOL_LAYER_VERSION=5、TRANSPORT_LAYER_VERSION=6；并与 `docs/ASAM_XCP_Part3_..._1.1.md` §1.4 `MAX_DTO: Parameter WORD 0x0008-0xFFFF` 一致 |
| Q3 | SET_MTA 地址字节序 | **按 Session Byte Order**（SET_MTA 只在 CONNECT 之后使用，届时字节序已确定） | `docs/XCP_1.3.0_document.md` §7.5.1.10；已用 Intel/Motorola 双黄金报文锁定 |
| Q4 | SYNCH 编码 | `[FC][00]`，2 字节 | `docs/XCP_1.3.0_document.md` §7.5.1.4 命令码表；恢复语义（始终以 ERR_CMD_SYNCH 应答）留给阶段 6 的 CommandExecutor |
| Q5 | 构建系统与测试框架 | 用户选定：CMake + GoogleTest（FetchContent） | 见 §1.1 |
| Q6 | `.clang-format` | 用户已提供，本批次据此格式化 | 见 §1.1 |

UDP Header（§6、阶段 3 实施）也已核对：`LEN(u16le)+CTR(u16le)`、无 Tail、LEN/CTR 恒为 Intel 格式、
一个 UDP Datagram 可含多个完整 Frame 且 Frame 不得跨界 —— 依据
`docs/ASAM_XCP_Part3_XCP_on_Ethernet_TCP_UDP_1.1.md` §1.3（第 244-296 行）。

---

## 4. 构建环境与命令

```
CMake   4.0.2
编译器   MSVC 19.44.35228（VS 2022 Community, x64）
Windows SDK 10.0.26100
生成器   Visual Studio 17 2022（见下方「环境限制」）
GoogleTest v1.15.2（离线源 .deps-cache/googletest，已在 .gitignore 中排除）
clang-format 22.1.8（LLVM）
```

Release（主要验证配置）：
```bash
cmake -B cmake-build-release -S .
cmake --build cmake-build-release --config Release
ctest --test-dir cmake-build-release -C Release --output-on-failure
```

Debug：
```bash
cmake -B cmake-build-debug -S .
cmake --build cmake-build-debug --config Debug
ctest --test-dir cmake-build-debug -C Debug --output-on-failure
```

---

## 5. 环境限制（重要，影响后续批次）

排查过程中确认两条本机限制，均已写入 `CMakeLists.txt` 注释：

1. **Ninja 生成器不可用**：沙箱阻断 ninja 派生子进程执行编译命令——连一条
   `command = cmake -E echo ok` 的规则都会永久挂起（直接调用 cl.exe / cmake / cmd 均正常）。
   因此改用 VS 生成器。**后果**：MSVC 由 CMake 自行定位，不再需要手工加载 `vcvars64.bat`。
2. **`cmake --build -j N` 并行构建失败**：MSBuild 在 `/m:4` 下并行解析项目引用时，
   `gtest.vcxproj → ZERO_CHECK` 报「0 个警告 0 个错误」但退出码非 0。
   去掉 `-j` 串行构建稳定通过，产物完好。**注意这与 AGENTS.md 里的 `-j 4` 示例不同**，
   在本沙箱内请使用串行命令。
3. FetchContent 在线克隆受本机 git `schannel` 后端影响（`SEC_E_NO_CREDENTIALS`）而挂起。
   解决办法：把依赖预取到 `.deps-cache/<name>/`，`tests/CMakeLists.txt` 检测到该目录即自动改用
   `FETCHCONTENT_SOURCE_DIR_GOOGLETEST`；目录不存在则回退在线拉取。未修改任何用户级 git 配置。

---

## 6. 测试结果

### 6.1 自动化测试

```
[==========] 81 tests from 26 test suites ran. (1 ms total)
[  PASSED  ] 81 tests.
```

| 配置 | 构建 | ctest |
|---|---|---|
| Release | 成功，项目源码 **0 警告**（`/W4`） | **81/81 Passed**（Total Test time 1.09 s） |
| Debug | 成功 | **81/81 Passed**（Total Test time 1.35 s） |

### 6.2 格式检查

```
clang-format --dry-run --Werror -style=file <全部 .hpp/.cpp>   → exit 0（无差异）
```

### 6.3 MockTransport 行为自检

`tests/mock_transport.hpp` 本批次尚无测试消费者，故单独编译并运行临时自检程序（已删除），
验证：初始 `isOpen()==false` → `open()` 后可发送 → 脚本响应同步回调 1 次 → `sentPackets()`
记录 1 条 → `injectWarning` 回调 1 次 → `close()` 触发 1 次 `onTransportClosed` 且二次 `close()`
幂等不再回调 → 关闭后 `send()` 抛 `XcpException(TransportError)`。全部符合预期（exit 0）。

### 6.4 过程中发现并修正的缺陷

| 现象 | 根因 | 处置 |
|---|---|---|
| `EXPECT_FALSE(XcpAddress40{a,b} == ...)` 编译期语法错误 | GTest 宏把初始化列表的逗号当参数分隔 | 改为先落命名变量再比较 |
| `Resource::A \| Resource::B \| Resource::C` 编译失败 | `operator\|` 返回底层整数类型，第三次 `\|` 无匹配重载 | 测试改为两步合并 + 显式 `static_cast<ResourceMask>` |
| `C4834 放弃 [[nodiscard]] 返回值` ×5 | `EXPECT_THROW` 内需显式丢弃 | `(void)codec.encode...(...)` |
| `C4244 const unsigned int → uint8_t` 窄化 | 初始化列表推导为 `unsigned int` | 元素显式写 `std::uint8_t{...}` |
| `SessionPendingCommand.ClearedByFailAndReset` 失败 | 测试自身前置状态漏了 `reset()`：`fail()` 后不允许直接重连（这是设计行为，已由 `FailedCannotSendCommands` 用例锁定） | 补 `session.reset()` |

---

## 7. 验收对照（计划文档 §10 中与本批次相关的条目）

| 条目 | 状态 |
|---|---|
| 1. 8 条命令均有 Codec/Parser 覆盖 | ✅ Codec 8 条全实现并有黄金报文；Parser 覆盖 CONNECT/GET_STATUS/GET_COMM_MODE_INFO 三类需解析的响应，其余命令响应为通用 `PositiveResponse.data` |
| 2. 能建立/关闭 Session 并保存全部必需参数 | ⏳ Session 层已完成并测试；端到端 `XcpMaster::connect()` 属阶段 4 |
| 8. 输入和解析具备长度、范围、溢出保护 | ✅ 编解码越界返回 nullopt、AG 保留位拒绝、地址推进溢出检测、NumberOfElements 范围校验 |
| 13. 核心库不依赖 Qt/A2L/CAN 厂商库 | ✅ 仅依赖标准库 + Threads |
| 14. 未实现功能返回明确错误 | ✅ `ErrorCategory::UnsupportedFeature` 与 `detail::makeUnsupportedFeature` 已就位，供阶段 6 使用 |

---

## 8. 下一批次（阶段 3~7）待办

1. `udp_header_codec.hpp/.cpp`：多 Frame Datagram 解析（§6 + §15.3 第 1 条），LEN 越界/尾部残留整体丢弃。
2. `udp_transport_config.hpp` + `udp_transport.hpp/.cpp`：跨平台 IPv4 UDP Socket、接收线程、双向独立 CTR 与缺口/重复/歧义策略（§8.1 七条）、来源过滤。
3. `tests/udp_test_slave.hpp/.cpp`：Loopback Slave，含 XCP 1.1 端点绑定规则（连接后仅校验 CONNECT 来源 IP，响应固定回原 IP:port）与故障注入。
4. `command_executor.hpp/.cpp`：发送-等待-解析-SYNCH 恢复、EV_CMD_PENDING 重启计时、错误分派表（计划 §6.4）。
5. `memory_access.hpp/.cpp` + `xcp_master.hpp/.cpp`：SHORT_UPLOAD 优先、SET_MTA+UPLOAD 分块降级、AG 换算与溢出。
6. 相应单元/Mock 集成/UDP Loopback 测试，覆盖计划 §9.3~§9.6 与设计 §15.3 五条要求。
