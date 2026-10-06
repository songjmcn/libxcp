# P4 XCPlite 子进程就绪身份绑定实施记录

## 范围与决定

用户批准加固 XCPlite 集成夹具：就绪必须证明是本次创建的子进程，而非仅证明监听端口上存在可应答 CONNECT 的 XCP 服务；并增加确定性占用端口回归。只改 tests/ 和 code-plan/，不改 thirdparty，不扩展 libxcp 公共 API。

原逻辑先检查子进程存活，再对选定端口执行 CONNECT/DISCONNECT。端口冲突时，这个连接探测无法证明应答者身份。XCPlite socket 设置 SO_REUSEADDR；最初使用第二个 XCPlite 进程作端口占用者的测试中，两个进程都能在同一 UDP 端口报告 ready，故不能作为独占占用的确定性测试。回归改用不设置 SO_REUSEADDR 的 `UdpTestSlave`，它可正常应答 XCP CONNECT/DISCONNECT，并让候选子进程首轮延迟 750 ms 后再绑定，确定性覆盖“端口已有另一 XCP 服务”的竞争窗口。

## 修改

- `tests/xcp_test_slave/xcplite_test_types.hpp`：与 Slave 和夹具共享就绪标记文件名。
- `tests/xcp_test_slave/main.cpp`：完成 A2L 定稿后写入包含本次启动 token 的标记；标记写失败返回 4。可选首轮启动延迟仅用于冲突回归。
- `tests/xcplite_slave_fixture.hpp/.cpp`：增加可选首选端口和首轮延迟的夹具测试参数；生成本次启动 token；只有在子进程存活、工作目录标记 token 匹配且 CONNECT/DISCONNECT 成功时才置为 ready；检查端口重试范围不越过 UDP 端口上限。
- `tests/xcplite_slave_fixture_test.cpp`：用正在工作的 UdpTestSlave 占用候选端口并延迟子进程首次绑定，断言夹具不会把外部服务误认为其子进程、会换端口并最终完成 A2L/标记验证。
- `tests/CMakeLists.txt`：将夹具回归和 UdpTestSlave 测试实现编入 XCPliteIntegration。
- 未修改 thirdparty、生产协议实现或新增依赖。

## 验证

- `cmake --build build-v09 --config Release --target XcpliteIntegration -j 1`：成功。
- `ctest --test-dir build-v09 -C Release -R OccupiedPortDoesNotSatisfyReadiness --output-on-failure -j 1`：1/1 通过。
- `ctest --test-dir build-v09 -C Release -R Xcplite --output-on-failure -j 1`：34/34 通过。
- `cmake --build build-v09 --config Release -j 1`：全 Release 目标增量构建成功。
- `ctest --test-dir build-v09 -C Release --output-on-failure -j 1`：503 项，502 通过、1 项既有 Skip、0 失败；Skip 为 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`。
- `git diff --check`：无空白错误；仅报告工作区已有 LF→CRLF 转换提示。

## 限制

本次是 Windows 本地证据；Linux/macOS 子进程/端口行为仍需各平台运行验证。此子项不代表 P4 整体完成，非自有对手端互操作和抓包门禁仍未完成。
