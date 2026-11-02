# libxcp C++ API 说明

本文面向将 libxcp 集成到 C++ 应用的开发者，依据当前公开头文件整理。库使用 C++20，命名空间为 `calmcar::xcp`；A2L 桥接类型位于 `calmcar::xcp::a2l`。不要将本文未列出的 `src/`、`adapter/` 实现细节视为稳定公共接口。

## 1. 构建与依赖

本仓库提供以下 CMake targets：

| Target | 用途 |
|---|---|
| `libxcp::libxcp` | XCP 主站核心、TCP/UDP transport、内存访问和 measurement 核心 |
| `libxcp::a2lbridge` | A2L 加载、符号查询及 raw/物理值换算 |
| `libxcp_measurement_adapter` | 将 A2L 数据库适配为 measurement 子系统所用接口 |

在同一 CMake 工程中集成时，可按实际用到的功能链接对应 targets；例如只用 XCP 主站和 A2L 查询时链接核心与 A2L bridge：

```cmake
target_link_libraries(my_app PRIVATE
    libxcp::libxcp
    libxcp::a2lbridge
)
```

以当前工程实际安装导出的 targets 和依赖配置为准。A2L bridge 是项目集成组件，不应直接依赖 `thirdparty/a2l-sdk` 的内部实现头文件。

## 2. 组件和典型数据流

- **主站门面 `XcpMaster`**：拥有 transport；管理 CONNECT/DISCONNECT、协商会话参数、内存读写等命令。
- **Transport**：`UdpTransport` 或 `TcpTransport`，负责网络帧封装与收发，不负责 XCP 命令恢复策略。
- **A2L bridge**：`A2lBridge` 加载本地 A2L 并发布只读 `IA2lDatabase`；数据库用于查符号地址、大小、类型和物理换算。
- **Measurement**：可选的 `A2lMeasurementDatabase` 与 `MeasurementSession` 将 A2L 符号接到 DAQ 规划/测量流；这不是简单内存轮询 API，需对端支持并正确配置 DAQ 能力。

一次普通标量读取的数据流为：加载 A2L → `IA2lDatabase::Find` → 读取 `SymbolInfo` 中的 XCP 地址和扩展 → `XcpMaster::ReadMemoryBytes` 取原始字节 → `IA2lDatabase::ToPhysical` 换算。A2L 元数据用于解释变量，不会自动连接 ECU，也不能证明 ECU 当前授权/允许访问该地址。

## 3. XCP 主站核心

头文件：`<libxcp/xcp_master.hpp>`。

### 构造与连接

```cpp
XcpMasterOptions options;  // 默认使用库提供的命令超时
// transport 为按 UDP 或 TCP 配置创建的 std::unique_ptr<IXcpTransport>
XcpMaster master(std::move(transport), options);
master.Connect();
// ... 调用 XCP API ...
master.Disconnect();
```

公开签名：

```cpp
explicit XcpMaster(std::unique_ptr<IXcpTransport> transport,
                   XcpMasterOptions options = {});
void Connect();
void Disconnect();
[[nodiscard]] bool IsConnected() const;
[[nodiscard]] SessionParameters GetSessionParameters() const;
```

`XcpMaster` 接管传入 transport 的所有权。`XcpMasterOptions::event_listener` 是非拥有指针，监听器必须比 master 活得更久。CONNECT 会打开 transport 并完成 XCP 会话连接；连接或命令失败会抛出 `XcpException`。不要假定 TCP 断线后 transport 会自动重连。

`XcpMasterOptions` 还含超时、可选字节序/地址粒度覆盖与版本策略。字节序和地址粒度通常由对端 CONNECT 协商；覆盖项只应在确认对端声明错误等情况下使用。

### XCP 协议版本自动探测（GET_VERSION）

`autodetect_version` 默认是 `true`：`Connect()` 成功后自动发送 XCP `GET_VERSION`（命令 `0xC0, 0x00`），获取协议层 PL 和传输层 TL 的运行时版本。CONNECT 本身只提供主版本；GET_VERSION 不可用或超时默认不会使连接失败，而是保留 CONNECT 主版本证据并将 minor 标为未确认，按保守能力处理，不能把超时当成“旧版本”。需要时可通过 `GetProtocolVersionSnapshot()`、`GetTransportVersionSnapshot()` 检查来源、置信度和有效值；快照查询本身不发命令。`require_get_version = true` 才会要求探测成功，否则连接期版本裁决失败。若构造时关闭自动探测，可在已连接状态调用 `ProbeVersion()` 主动补测。

**GET_VERSION 不是 TCP wire profile 探测。** 它只识别 XCP PL/TL 协议版本，不会决定 TCP 的 `AsamV14`/`AsamV15` 帧布局或 PACKET_ALIGNMENT；版本快照也不能替代 transport profile 配置。

### 内存读写

```cpp
[[nodiscard]] Bytes ReadMemoryBytes(Address address,
                                    AddressExtension extension,
                                    ByteCount byte_count);
[[nodiscard]] Bytes ReadMemory(Address address,
                               AddressExtension extension,
                               ElementCount element_count);
void WriteMemoryBytes(Address address, AddressExtension extension,
                      BytesView data);
```

- `ReadMemoryBytes` 的长度单位是字节；长度必须能被当前协商的 Address Granularity（AG）整除。
- `ReadMemory` 的数量单位是 AG 元素数；不可与字节数混用。
- 返回值是 ECU 的**原始字节**；要获得物理量，应使用 A2L 数据库的 `ToPhysical`。
- 写入可能分多条命令执行，失败时不保证跨块原子性；只有在确认目标内存和写权限后才调用。
- 地址、扩展和字节数必须依据 ECU/A2L 事实提供，禁止将地址扩展误当成地址高位。

其他常用入口包括 `FetchA2lViaUpload()`（对端支持相应上传路径时获取 A2L 字节流）、`QueryStatus()`、`GetProtocolVersionSnapshot()` 与 `GetTransportVersionSnapshot()`。详见各自声明头文件的 doxygen 注释。

## 4. 网络 Transport

### XCP on UDP/IP

头文件：`<libxcp/udp_transport.hpp>`、`<libxcp/udp_transport_config.hpp>`。

```cpp
UdpTransportConfig cfg;
cfg.remote_host = "192.168.1.10";
cfg.remote_port = 5555;
cfg.local_host = "0.0.0.0";
cfg.local_port = 0;  // 让操作系统选择本地临时端口

auto transport = std::make_unique<UdpTransport>(cfg);
```

远端地址和非零端口必填；默认要求入站报文同时匹配远端地址和端口（`strict_remote_port = true`）。UDP 是无连接数据报传输；transport 负责帧检查与报文投递，不在此层重排或重传 XCP 命令。

### XCP on TCP/IP

头文件：`<libxcp/tcp_transport.hpp>`、`<libxcp/tcp_transport_config.hpp>`、`<libxcp/tcp_wire_profile.hpp>`。

```cpp
TcpTransportConfig cfg;
cfg.role = TcpRole::Master;
cfg.remote_host = "192.168.1.10";
cfg.remote_port = 5555;
cfg.wire_profile = TcpWireProfile::AsamV14;  // 必须根据对端线路配置选择

auto transport = std::make_unique<TcpTransport>(cfg);
```

`AsamV14` 与 `AsamV15` 的 TCP 帧布局不同。虽然配置类型具有默认值，应用仍应依据从站实现/文档明确指定 `wire_profile`：`Unknown` 会在 Open 时拒绝；禁止仅凭 A2L、GET_VERSION 结果或首帧猜测线路格式。TCP 是字节流，transport 负责跨 `recv` 分片解帧；断链不会自动重连。

### TCP wire profile 自动试连（连接级协商）

库不在同一条 TCP 连接中做字节流“嗅探”或中途切换 profile：错配解帧可能吞掉下一帧头，造成流失步。若设备配置未知，可显式使用连接级候选试连，而不是在单连接内窥探：

- 构造 `TcpFramingCandidateRequest`，用 A2L 的 TL 版本和 `PACKET_ALIGNMENT` 调用 `BuildTcpFramingCandidates()`。这些证据**只排序候选**，A2L 声明可能过时，仍会验证其它候选。
- 通过 `NegotiateTcpFraming()` 逐候选调用 `VerifyTcpFramingCandidate()`。每次验证都创建全新 TCP 连接，执行 CONNECT、GET_COMM_MODE_INFO、GET_STATUS，再额外执行一次 `QueryStatus()`；验证阶段关闭 GET_VERSION 自动探测，结束后关闭连接。胜出候选不能复用为正式会话，必须另建连接。
- 结果是三态：`Accepted`（连续协议往返可用）、`Rejected`（有协议层违规/连接后主动断开等拒绝证据）、`Inconclusive`（超时、地址/端口不可达等没有足够证据）。超时不应误报为“该布局不支持”。首个 `Accepted` 胜出；失败时检查每项 `reason`，区分布局不匹配和网络/从站问题。
- 默认候选顺序为 `AsamV14/8bit`、`AsamV15/8bit`、`AsamV15/16bit`、`AsamV15/32bit`。8 bit 下 V1.4/V1.5 的字节布局相同，差别主要在 CTR 严格性，因此试连只能选出兼容候选，不能证明真实 profile 唯一；A2L TL 版本可改变两者的优先次序。若 A2L 对齐声明为 16/32 bit，则对应的 V1.5 候选先试，其余默认候选仍会保留为回退项。

对应公开头文件为 `<libxcp/tcp_framing_candidates.hpp>`、`<libxcp/tcp_framing_negotiation.hpp>`、`<libxcp/tcp_framing_verification.hpp>`。独立工具 `xcp_master` 也提供 `--tcp-profile auto`；其自动流程同样使用全新连接验证并在胜出后重新连接。`auto-legacy` 是旧式首成功试连模式，不提供唯一识别保证，不建议新集成采用。

## 5. A2L 加载和符号查询

头文件：`<libxcp/a2l/a2l_bridge.hpp>`、`<libxcp/a2l/ia2l_database.hpp>`、`<libxcp/a2l/a2l_types.hpp>`。

```cpp
a2l::LoadOptions load_options;
auto loaded = a2l::A2lBridge::Load("calibration.a2l", load_options);
if (!loaded.HasValue()) {
    const auto& error = loaded.ErrorInfo();
    // 根据 error.code / error.phase 分支；message 用于展示
}
const auto& bridge = *loaded.Value();
const auto* database = bridge.Database();
```

`Load` 返回 `a2l::Result<std::unique_ptr<a2l::A2lBridge>>`，不是抛 `XcpException` 的主站接口。先检查 `HasValue()`，成功后再访问 `Value()`；失败使用 `ErrorInfo()` 的结构化 `code`、`phase`、定位字段等处理，不能解析 `message` 作为程序逻辑。同步加载成功时 `Database()` 可用；异步加载需等待完成回调后再读数据库。数据库视图由 bridge 持有，使用期间必须保证 bridge 仍存活。

```cpp
const auto* database = bridge.Database();
if (database == nullptr) {
    // 数据库未就绪；不要继续查询
}

auto found = database->Find("Powertrain::EngineSpeed");
auto count = database->Count();
auto matches = database->Search("Powertrain::Engine*", 100);
```

- `Find` 支持 `module::symbol` 精确键；裸符号名仅在全库唯一时成功，否则返回 `AmbiguousName`。
- `Search` 的 `*` 匹配任意串、`?` 匹配单字符；匹配对象是全限定名，大小写无关，结果稳定排序。
- `SymbolInfo` 提供 `module_name`、`name`、`kind`、`xcp_address`、`address_extension`、`element_size_bytes`、`dimensions`、`data_type`、`byte_order` 等字段。`xcp_address` 是 A2L 地址原值，不乘 AG；地址扩展是独立字段。
- 标量可用 `ToPhysical(name, raw)`；`ToPhysical` 要求 `raw` 长度恰好等于元素宽度。数组/曲线等应按对应布局和元素 API 处理，不能将其误当标量。
- `PhysicalValue` 是 `std::variant<std::int64_t, std::uint64_t, double, std::string, bool>`，消费时应按实际 variant 分支处理。
- A2L 符号存在不代表其类型、地址访问模式或 ECU 运行时权限一定可用；如遇不支持的数据类型/布局，应处理结构化错误而非猜宽度。

## 6. Measurement 高层 API

头文件：`<libxcp/measurement/measurement_session.hpp>` 和 `<libxcp/measurement/measurement_database.hpp>`。A2L 适配器当前通过 `libxcp_measurement_adapter` target 的构建期 include 路径暴露，仓库内可用 `#include <a2l/a2l_measurement_database.hpp>`；该适配器 target 当前并未定义独立安装规则。链接 `libxcp_measurement_adapter`。

典型顺序为：先创建数据库适配器，再创建 `MeasurementSession`，创建 master 时把 `&session` 设置到 `XcpMasterOptions::event_listener`，CONNECT 后 `session.Bind(master)`，随后 `Add`、`Prepare`、`Start`。回调在 measurement worker 线程执行，非 transport 接收线程；停止时调用 `Stop()`。适配器和 A2L bridge/database 必须活得比 measurement session 久。

DAQ 事件、识别字段布局、时间戳单位和目标支持的 Entry 能力必须来自 A2L/运行时或 ECU 文档等可验证证据；不要在缺少证据时猜测后直接启动 DAQ。

## 7. 错误处理与生命周期

- XCP 主站/Transport API 以 `calmcar::xcp::XcpException` 报错。通过 `Category()` 做程序分支，必要时读取 `GetCommandCode()`、`GetErrorCode()`、`RetryCount()`、`TransportError()`；不要匹配错误文本。
- A2L bridge/database 通过 `a2l::Result<T>` 返回结构化错误码；这套错误模型不同于 `XcpException`。
- Transport 接收回调在其接收线程中执行；回调参数中的 Packet 视图只在回调期间有效。避免在该线程上执行阻塞或耗时工作。
- `XcpMaster` 拥有 transport，但不拥有事件 listener；`A2lMeasurementDatabase` 不拥有数据库；`MeasurementSession` 对数据库和 master 均有生命周期要求。按“被引用对象活得更久”的原则安排析构顺序。

## 8. 相关示例

- [XCP 调用 Quick Start](XCP_调用_QuickStart.md)：A2L 符号查询和 UDP/TCP 内存读取。
- [UDP 主站示例说明](../examples/xcp_master_udp/README.md)：可运行的 UDP/XCPlite 示例。
- [Measurement 子系统说明](modules/测量子系统.md)：A2L bridge、适配器和测量会话的集成设计与用法。
