# XCP 调用 Quick Start：A2L、变量查询、TCP 与 UDP

本文展示如何用 libxcp 加载本地 A2L、枚举/查询变量，并通过 XCP on UDP/IP 或 XCP on TCP/IP 读取一个标量变量。示例遵循当前公开 C++20 API；ECU 地址、端口和 TCP wire profile 必须依据实际设备配置填写，示例值不能直接视为所有从站的通用值。

## 1. 构建与链接

在已将本项目接入 CMake 的工程中，按功能链接：

```cmake
target_link_libraries(my_app PRIVATE
    libxcp::libxcp
    libxcp::a2lbridge
)
```

项目的必编 A2L 栈依赖 Boost CONFIG 包的 `locale`、`filesystem`、`process` 组件；完整构建依赖和安装方式见仓库根目录 `CMakeLists.txt`。

## 2. 加载 A2L 并查询变量

```cpp
#include <libxcp/a2l/a2l_bridge.hpp>

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace xcp = calmcar::xcp;
namespace a2l = calmcar::xcp::a2l;

a2l::LoadOptions load_options;
// 仅当确认 A2L 不含 IF_DATA XCP 时，才启用下面这项：
// load_options.require_if_data_xcp = false;
auto loaded = a2l::A2lBridge::Load("path/to/ecu.a2l", load_options);
if (!loaded.HasValue()) {
    const auto& error = loaded.ErrorInfo();
    std::cerr << "A2L 加载失败: " << error.message << '\n';
    // 应用可按 error.code / error.phase 做机器可判定的错误处理。
    throw std::runtime_error("A2L load failed");
}

// bridge 必须保持存活：database 指针由 bridge 持有。
auto bridge = std::move(loaded).Value();
const auto* database = bridge->Database();
if (database == nullptr) {
    throw std::runtime_error("A2L database is not ready");
}

auto count = database->Count();
if (!count.HasValue()) {
    throw std::runtime_error(count.ErrorInfo().message);
}
std::cout << "A2L 符号数: " << count.Value() << '\n';

auto symbols = database->Search("*", 1000);
if (!symbols.HasValue()) {
    throw std::runtime_error(symbols.ErrorInfo().message);
}
for (const auto& symbol : symbols.Value()) {
    std::cout << symbol.module_name << "::" << symbol.name
              << " @ 0x" << std::hex << symbol.xcp_address << std::dec
              << "，宽度 " << static_cast<unsigned>(symbol.element_size_bytes)
              << " 字节\n";
}

// 推荐使用 module::name，避免不同 MODULE 中的同名符号产生歧义。
const std::string variable_name = "Powertrain::EngineSpeed";
auto found = database->Find(variable_name);
if (!found.HasValue()) {
    throw std::runtime_error(found.ErrorInfo().message);
}
const auto symbol = found.Value();
```

`Find` 的裸变量名仅在全库唯一时成功；`Search` 的模式针对全限定名，支持 `*` 和 `?`。如果 A2L 不包含 IF_DATA XCP，默认 `LoadOptions::require_if_data_xcp == true` 会要求其存在；只有已确认该文件可不带 IF_DATA XCP 使用时，才设置 `load_options.require_if_data_xcp = false`。加载成功只代表 A2L 可解析，不代表 ECU 可访问该地址。

## 3. 创建 XcpMaster 并读取物理值

下列函数可与 UDP 或 TCP 小节中任一 transport 创建代码组合。`ReadMemoryBytes` 返回原始字节，A2L 数据库负责把这些字节换算为带类型的物理值。示例限定标量，数组/曲线需按相应元素或 DAQ 布局接口读取。

```cpp
#include <libxcp/xcp_master.hpp>
#include <libxcp/a2l/ia2l_database.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace xcp = calmcar::xcp;
namespace a2l = calmcar::xcp::a2l;

// transport 是下文 UDP/TCP 小节创建的 std::unique_ptr<IXcpTransport>。
xcp::XcpMaster master(std::move(transport));
try {
    master.Connect();

    if (!symbol.dimensions.empty()) {
        throw std::runtime_error("此示例只演示标量读取");
    }
    if (symbol.element_size_bytes == 0) {
        throw std::runtime_error("A2L 未提供可用的固定元素宽度");
    }
    if (symbol.xcp_address > std::numeric_limits<xcp::Address>::max()) {
        throw std::runtime_error("符号地址超出 XCP Address 范围");
    }

    const auto raw = master.ReadMemoryBytes(
        static_cast<xcp::Address>(symbol.xcp_address),
        static_cast<xcp::AddressExtension>(symbol.address_extension),
        static_cast<xcp::ByteCount>(symbol.element_size_bytes));

    const auto physical = database->ToPhysical(variable_name, raw);
    if (!physical.HasValue()) {
        throw std::runtime_error(physical.ErrorInfo().message);
    }
    std::visit([](const auto& value) { std::cout << "物理值: " << value << '\n'; },
               physical.Value());

    master.Disconnect();
} catch (const xcp::XcpException& error) {
    std::cerr << "XCP 错误类别="
              << xcp::ErrorCategoryName(error.Category())
              << "，详情=" << error.what() << '\n';
    // 按 error.Category() 做错误处理；如需重连，先按应用策略重新建立会话。
    throw;
}
```

`ByteCount` 必须能被 CONNECT 协商出的地址粒度（AG）整除。如果 XCP 异常发生在 `Disconnect()`，本次操作仍应报告失败；析构时也会尽力关闭会话与 transport。生产代码建议用应用自己的 RAII/错误策略管理连接，且避免忽略断开错误。

## 4. XCP on UDP/IP

```cpp
#include <libxcp/udp_transport.hpp>

xcp::UdpTransportConfig udp_config;
udp_config.remote_host = "192.168.1.10";  // 改为 ECU 的 IPv4 地址
udp_config.remote_port = 5555;             // 改为 ECU 的 XCP UDP 端口
udp_config.local_host = "0.0.0.0";
udp_config.local_port = 0;                 // 由操作系统分配本地端口

std::unique_ptr<xcp::IXcpTransport> transport =
    std::make_unique<xcp::UdpTransport>(udp_config);
```

将此处的 `transport` 移交给第 3 节的 `XcpMaster`。默认严格检查入站报文远端地址及端口；请确认 ECU 返回报文使用配置的端点。UDP 不需要 TCP wire profile。

## 5. XCP on TCP/IP

```cpp
#include <libxcp/tcp_transport.hpp>

xcp::TcpTransportConfig tcp_config;
tcp_config.role = xcp::TcpRole::Master;
tcp_config.remote_host = "192.168.1.10";  // 改为 ECU 的 IPv4 地址
tcp_config.remote_port = 5555;             // 改为 ECU 的 XCP TCP 端口
// 必须按 ECU 的 Ethernet transport 帧格式明确选择：
// AsamV14（例如 XCPlite 兼容路径）或 AsamV15。
tcp_config.wire_profile = xcp::TcpWireProfile::AsamV14;

std::unique_ptr<xcp::IXcpTransport> transport =
    std::make_unique<xcp::TcpTransport>(tcp_config);
```

将 `transport` 移交给第 3 节的 `XcpMaster`。如果设备资料已明确 profile，直接显式配置；若未知，可按下一节进行候选连接试连。不要在同一连接中依据首帧内容动态切换 profile。TCP 断连后库不会自动重连；新连接需要重新 CONNECT。

## 6. TCP wire profile 自动试连说明

“TCP 协议版本探测”容易指两件不同的事，必须区分：

1. **XCP PL/TL 版本探测（GET_VERSION）**：`XcpMasterOptions::autodetect_version` 默认 `true`，正常 `Connect()` 后会请求 `GET_VERSION (0xC0, 0x00)`。对端不支持或超时默认不导致连接失败，而是保留 CONNECT 的主版本并将 minor 视为未确认；可用 `GetProtocolVersionSnapshot()` / `GetTransportVersionSnapshot()` 查看证据来源和置信度。此命令识别 XCP 协议层/传输层版本，**不决定** TCP `AsamV14` / `AsamV15` wire profile。
2. **TCP wire profile 自动试连**：未知 `AsamV14` / `AsamV15` 线路布局时，不能在同一 TCP 流中按首帧猜测、切换。库提供的候选验证方式是每个候选都用**全新 TCP 连接**进行协议往返测试；验证通过后关闭试连，再以选中的 profile 和 alignment 建立正式连接。

### 使用 xcp_master 工具

若使用仓库的 `xcp_master` 工具，可显式启用严格候选验证：

```sh
xcp_master --a2l path/to/ecu.a2l --transport tcp \
  --host 192.168.1.10 --port 5555 --tcp-profile auto
```

工具用 A2L 声明的 TL 版本和 `PACKET_ALIGNMENT` **只排序**候选，声明不可靠时仍会继续试其它候选。每项结果显示为“接受 / 被拒 / 未决”：超时或无法连接属于“未决”，不是对端拒绝该 profile。命令返回码 `0` 表示找到可用候选，`3` 表示所有候选均被明确拒绝，`4` 表示至少一个候选未决，应先排查地址、端口、网络或从站状态。工具完成验证后会重新建立正式连接。

### 在 C++ 集成中复用候选验证

可使用公开头文件 `<libxcp/tcp_framing_candidates.hpp>`、`<libxcp/tcp_framing_negotiation.hpp>`、`<libxcp/tcp_framing_verification.hpp>`。核心调用形态如下（`evidence` 可从已加载 A2L 的 TL 版本与 alignment 声明填充；只用于候选排序）：

```cpp
#include <libxcp/tcp_framing_candidates.hpp>
#include <libxcp/tcp_framing_negotiation.hpp>
#include <libxcp/tcp_framing_verification.hpp>
#include <libxcp/tcp_transport.hpp>
#include <libxcp/xcp_master.hpp>

#include <memory>
#include <stdexcept>
#include <utility>

namespace xcp = calmcar::xcp;

xcp::TcpFramingCandidateRequest evidence;
// evidence.transport_layer_version = ...; // A2L TL 版本（可选）
// evidence.alignment = ...;               // A2L PACKET_ALIGNMENT（可选）
const auto candidates = xcp::BuildTcpFramingCandidates(evidence);

xcp::TcpFramingProbeConfig probe;
probe.host = "192.168.1.10";
probe.port = 5555;
// 默认连接超时 3000 ms、命令超时 1000 ms；按目标设备酌情调整。
xcp::FramingAttemptFn attempt = [probe](const xcp::TcpFramingCandidate& candidate) {
    return xcp::VerifyTcpFramingCandidate(probe, candidate);
};
const auto result = xcp::NegotiateTcpFraming(candidates, attempt);
if (!result.selected) {
    // 查看 result.attempts / result.Summary()；区分“被拒”与“未决”，不要盲目重试。
    throw std::runtime_error(result.Summary());
}

// VerifyTcpFramingCandidate 已关闭试连；正式会话必须创建新连接。
xcp::TcpTransportConfig final_config;
final_config.role = xcp::TcpRole::Master;
final_config.remote_host = probe.host;
final_config.remote_port = probe.port;
final_config.wire_profile = result.selected->profile;
final_config.packet_alignment = result.selected->alignment;

auto formal_transport = std::make_unique<xcp::TcpTransport>(final_config);
xcp::XcpMaster master(std::move(formal_transport));
master.Connect();  // 正式连接上默认仍执行独立的 GET_VERSION 版本探测
```

验证阶段特意关闭 GET_VERSION 自动探测以避免把版本探测失败混进 framing 结论；验证序列执行 CONNECT 所需握手及额外一次 `QueryStatus()`，并保证每次试连结束关闭 socket。8 bit 下 V1.4 与 V1.5 字节布局等价，验证只选出可用兼容候选，不保证 profile 唯一；A2L TL 版本仅影响优先次序。`auto-legacy` 是旧式首成功试连模式，可能影响 ECU 会话且不证明唯一 profile，不建议新集成使用。

## 7. 常见问题

| 现象 | 检查项 |
|---|---|
| A2L 加载失败 | 文件路径、A2L 语法、`/include` 文件位置、是否要求 IF_DATA XCP；查看结构化 `ErrorCode`、`Phase` 和定位信息。 |
| `Find` 找不到或歧义 | 核对 `MODULE` 名与符号名，优先用 `module::name` 全限定键。 |
| 连接超时/transport 错误 | 核对 ECU 地址、传输类型和远端端口、防火墙、从站是否已启动及其地址配置。 |
| TCP 收到畸形包或解帧失败 | 核对对端 wire profile。不要尝试靠 A2L 版本推导 TCP profile。 |
| 内存读取报参数/协议错误 | 核对符号地址、地址扩展、元素宽度、协商 AG、ECU 当前内存映射与访问权限。 |
| raw 数据无法换算 | 确认传入长度等于符号元素宽度、数据类型与换算方法受支持；数组不能当标量调用 `ToPhysical`。 |

## 8. 相关文档与可运行示例

- [C++ API 说明](API_说明.md)：主要公开 API、所有权、错误模型及线程/生命周期约束。
- [UDP 主站示例](../examples/xcp_master_udp/README.md)：实际 UDP 对手端演示，含 A2L 上传/加载和标量读写。
- [TCP transport 配置声明](../include/libxcp/tcp_transport_config.hpp) 与 [TCP wire profile 声明](../include/libxcp/tcp_wire_profile.hpp)：完整配置字段和 profile 语义。
