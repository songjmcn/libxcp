# EventPoller 平台分文件实施与测试记录

- 范围：仅拆分 `src/event_poller.hpp/.cpp` 的平台实现，迁移 `tests/event_poller_test.cpp`，不修改公共接口和 TCP/UDP 协议逻辑。
- 公共文件 `src/event_poller.cpp` 保留进程内原子测试注入、`CurrentEventPoller()` 和 `WaitAnySocket()`，不包含平台原生头或 `_WIN32`。
- `src/event_poller.hpp` 改为依赖无平台头 `detail/socket/socket_ops.hpp`，沿用 `WaitItem`、`WaitStatus` 和所有 EventPoller 签名；空集合契约注释与已有实现保持一致。
- Windows 实现 `src/detail/socket/event_poller_win.cpp` 保留独立的 Winsock 启动/清理计数、WSAPoll 事件和错误格式；Linux/macOS 实现 `src/detail/socket/event_poller_posix.cpp` 保留 poll、errno 文本和事件位。
- 测试更新为 IPv4 POD 接口（`Ipv4Endpoint`，端口主机序），新增真实平台实现空等待、failed 位就绪、注入恢复及失败文本转发验证；原回环/多句柄/超时/可写/委托用例保留。
- 静态验证：`clang-format --dry-run --Werror` 对本任务五个 C++ 文件通过；公共实现/头的原生头、`_WIN32`、`sockaddr` 检索无命中。`CMakeLists.txt` 已由父任务将公共源文件与 WIN32/POSIX 平台源文件分别接入。
- 构建和测试结果：待父任务完成 TCP/UDP/SocketOps 和其他测试迁移后执行 Release 构建及定向测试；尚未运行 Windows/Linux/macOS CI，不将静态检查误写为运行通过。
