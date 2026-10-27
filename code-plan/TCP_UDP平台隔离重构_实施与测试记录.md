# TCP/UDP 平台隔离重构：实施与测试记录

> 状态：Windows/MSVC Release 全量构建与 CTest 1055/1055 已通过，Linux/macOS 与三平台 CI **尚未验收**。最新验证数据以 [TCP_UDP平台隔离重构_阶段实施记录_2026-10-27.md](TCP_UDP平台隔离重构_阶段实施记录_2026-10-27.md) 为准，不得据此文档宣称方案完成。

## 依据与范围

- 实施依据：`code-plan/TCP_UDP平台隔离重构_实施计划_POD接口与平台分文件.md` v3；用户同意在当前 `update_boost_asio` 分支实施平台分离（不接入 Boost.Asio），保留 POD 形式的内部 DNS 解析钩子但 TCP/UDP 只接受 IPv4 字面量。EventPoller 也按平台拆分；不修改 `include/libxcp/**`、`thirdparty/**`。
- 平台无关声明 `src/detail/socket/socket_ops.hpp`：`Ipv4Address` 为网络序四字节，`Ipv4Endpoint::port` 为主机序；以 `SocketKind` 和 `SocketOption` 代替原生协议/选项常量；`SocketOps` 处理 TCP/UDP 句柄、端点、I/O 与三态单句柄等待。真实 DNS 函数在平台源中取得 IPv4 候选并释放系统地址链，注入函数返回 POD 列表。
- `src/socket_util.hpp/.cpp` 只作内部转发、测试注入及无平台头的 IPv4 文本转换；`src/event_poller.hpp/.cpp` 只作等待接口与公共注入；Windows/POSIX 原生操作分别位于 `src/detail/socket/socket_ops_{win,posix}.cpp`、`src/detail/socket/event_poller_{win,posix}.cpp`，`CMakeLists.txt` 依平台只选本平台的两个实现。
- `src/tcp_transport.cpp` 改用 POD 绑定、连接、accept、套接字选项；保留 TCP 100 ms 等待片、现有业务错误分类；accept 后已关闭的监听句柄标记为无效，避免 `Close()` 时重复关闭。`src/udp_transport.cpp` 的数据 Socket 与唤醒对共用 `SocketOps`，等待继续使用 `EventPoller`，保持先 join 再 close 与接收超时兜底。
- `tests/tcp_fake_socket.hpp` 迁移假操作签名；`tests/event_poller_test.cpp` 更新原有测试并补等待失败、真实空等待、注入复位等测试；`tests/socket_ops_pod_test.cpp` 增加 IPv4 解析/格式化、真实数值地址解析、失败清空候选及注入候选测试；`tests/tcp_transport_test.cpp` 用关闭抽象行为测试替换已内聚到平台源的原生关闭常量断言。
- `tests/check_socket_platform_boundary.py` 扫描全部 `src/` 非平台 `.cpp/.hpp`，仅显式豁免四个指定平台源，检查原生类型、常量、系统调用、平台分支与 include；`tests/CMakeLists.txt` 将该门禁加入 CTest，各平台现有 workflow 的完整 CTest 因而覆盖它。

## 验证和待办

| 项目 | 状态 | 证据/限制 |
| --- | --- | --- |
| 源码读回 | 已做静态人工检查 | 已读回 SocketOps 双平台源、EventPoller 接口及 POSIX 平台源、TCP/UDP 关键路径及测试替身；待完整编译验证。 |
| 静态隔离脚本 | **未运行** | 首次执行 `python tests/check_socket_platform_boundary.py .` 时命令执行器在运行前报 `EXECUTOR_UNAVAILABLE`，伴随 `CreateFileMapping ... Win32 error 5`；不能视为扫描通过，也不绕过隔离重试。 |
| Release 完整构建 | **未运行** | 需先 `cmake --build cmake-build-release --target clean`，再配置/构建；命令执行器暂不可用。 |
| 定向测试/全量 CTest | **未运行** | 至少覆盖 `TcpFakeSocketTest`、`SocketOpsPodTest`、`SocketPlatformTest`、`EventPollerTest`、`UdpTransportTest`、`XcpliteIntegration` 和静态门禁，再运行 Release 功能回归；不可从源码推测通过。 |
| Windows/Linux/macOS CI | **未运行** | 未执行 Git 写操作，也没有本次改动的远端 CI 结果；仍属硬验收门。 |

## 风险与恢复路径

1. 当前 Windows 主机不能替代 Linux/macOS 编译；待命令执行环境恢复后先运行静态门禁、完整构建前 clean 和定向回归，再依三个 workflow 验收平台结果。
2. 若新脚本误报日志字符串，脚本现先去注释，再去字符串，仅保留独立 include 检查；仍须实际执行并检查产出与 CI。所有测试结果只记录真实运行证据。
3. 本文为本次代码修改的实施/测试记录，保持“未验收”直到构建和测试实际完成。
