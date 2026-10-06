# P4 本地 UDP 超时硬化实施记录

## 范围与决定

用户批准仅实施 UDP 接收超时配置硬化与测试，不修改 `thirdparty/`。P4 整体仍包含 Linux/macOS 和非自有对手端的实测门禁；本记录不将本地 Windows 测试等同于这些验收。

静态审查发现：`receive_poll_interval_ms` 可为 0；`Open()` 忽略 `setsockopt(SO_RCVTIMEO)` 的返回值；`Close()` 在关闭 Socket 前先等待接收线程 join。若接收超时被禁用或配置失败且没有入站数据，接收线程可能一直阻塞。

## 修改

- `include/libxcp/udp_transport_config.hpp`：配置注释明确 `receive_poll_interval_ms` 必须大于 0。
- `src/udp_transport.cpp`：`Open()` 在 Socket 创建前拒绝零轮询间隔，抛 `InvalidArgument`；检查 Windows/POSIX `SO_RCVTIMEO` 设置结果。设置失败时先读取平台错误文本，再关闭 Socket 并抛 `TransportError`，避免清理操作覆盖底层错误。
- `tests/udp_transport_test.cpp`：新增 `UdpTransportConfigValidation.ZeroReceivePollIntervalIsRejected`；修正长轮询关闭用例注释，使之准确表达“一个轮询周期内退出”。
- 未增加公共 API、依赖或 thirdparty 修改。

## 验证

配置：`build-v09`，Visual Studio 17 2022，Windows Release；A2L、测试、XCPlite 集成门均开启。

- `cmake --build build-v09 --config Release --target clean -j 1`：成功。
- `cmake --build build-v09 --config Release -j 1`：全目标干净 Release 构建成功。输出含若干既有无关警告（A2L 测试中的窄化转换、未使用参数及 thirdparty demo 未使用参数）；本次改动未导致构建失败。
- `ctest --test-dir build-v09 -C Release -R ZeroReceivePollIntervalIsRejected --output-on-failure`：1/1 通过。
- `ctest --test-dir build-v09 -C Release --output-on-failure -j 1`：502 项，501 通过、1 项既有 Skip、0 失败；Skip 为 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`。
- `git diff --check`：无空白错误；仅提示计划文档 LF 将转为 CRLF。

`SO_RCVTIMEO` 失败分支做了代码级错误保留与清理，但没有跨平台故障注入测试；本次新增自动测试覆盖零间隔拒绝，常规 Loopback 覆盖正常设置及关闭。

## 未完成门禁

- Linux/macOS 运行时构建和测试未验证。本机检查 WSL 时得到 `Wsl/E_ACCESSDENIED`；Windows `clang++`/`g++` 命令不能代替 Linux/macOS 运行证据。
- 非自有 XCP Slave/ECU 的抓包与互操作仍未完成。
- 因此本子项只关闭 Windows UDP 超时配置风险，P4 整体不标记完成。
