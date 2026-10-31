# XCPlite V1.2 互操作用例端口分配偶发失败修复记录

## 问题现象

GitHub workflow `MSBuild`（job `build-windows`，run `41094724693`，分支 `feat/tcp-a2l-wire-evidence`，sha `2979c903`）报：

```
1148 - TcpAndUdp/XcpliteV12InteropTest.ReadsMeasurementThroughA2lAddress/Udp
       # GetParam() = "udp" (Failed)
```

同 run 的 Linux、macOS 全部通过，同一分支前一次 run 也通过，因此是平台相关的偶发失败，而非协议逻辑回归。

## 根因

用户提供的 CI 日志（`logs_111399218452.zip` 中 `0_build-windows.txt`）给出了完整证据链：

```
[XCP  ] ERROR: 10013 - cannot bind on 127.0.0.1 port 51556!
XcpEthServerInit 失败，无法在 127.0.0.1:51556 上监听 udp
...（51557 … 51571 共 16 个候选，全部 10013）
tests/xcp_v12_interop_test.cpp(109): error:
  Expected: slave_->Start() doesn't throw an exception.
  Actual: it throws std::runtime_error with description
  "XCPlite V1.2 udp 从站就绪失败: XCPlite V1.2 从站提前退出（端口可能已占用）".
```

- `10013` 即 `WSAEACCES`。Windows 上 Hyper-V / WinNAT / Docker 会动态保留整段端口，落在保留段内的显式 `bind` 一律失败。
- 父进程 pid = 7556，夹具的旧基址公式为 `44000 + pid % 10000 = 51556`，`kPortSpan = 16` 个候选正好是 `51556…51571`，整段落在 runner 当次的保留段内，于是 16 次尝试全部失败。
- 失败发生在 SetUp 的 `ASSERT_NO_THROW(slave_->Start())`，与 UDP 收发、CTR 校验、UPLOAD 读回均无关；紧随其后的 `/Tcp`、`CalibrationWrite…/{Udp,Tcp}` 三个用例全部通过，也印证了从站本身没有问题。

由于候选端口由进程 ID 决定、连续 16 个，一旦基线落入保留段就是"整段命中"，概率低但一旦命中必然失败，这与偶发特征吻合。

## 实施内容

只改测试夹具，未触碰 `thirdparty/`、`src/` 与生产接口。

### tests/xcp_v12_slave_process.cpp

- 新增 `ReserveEphemeralPort(const std::string& transport) noexcept`：按传输类型创建 `SocketKind::Tcp` / `SocketKind::Udp` 套接字，绑定 `127.0.0.1:0` 后由 `getsockname` 取回操作系统分配的端口并立即关闭；任何失败返回 0。
- `Start()` 的端口选择改为：每个候选先取 `ReserveEphemeralPort()`；仅当返回 0 时回退到旧的 `44000 + pid % 10000 + attempt`，保证异常平台上行为与旧实现一致。
- `kPortSpan` 的含义从"端口跨度"改为"候选重试次数"（每次都会重新申请端口），并补充中文注释说明 Windows 保留段背景。

### tests/xcp_v12_interop_test.cpp

- 新增回归用例 `XcpliteV12PortAllocationTest.StartsWhenLegacyCandidatePortsOccupied`：先按旧公式占用 `44000 + pid % 10000` 起 16 个回环 UDP 端口（`PortHolder` RAII 类，析构统一释放），再断言 `slave.Start()` 不抛异常、`slave.Port()` 大于 0 且不落在被占集合内。若一个端口都占不住（前提不成立）则 `GTEST_SKIP()`。
- 该用例在旧实现下必然失败（16 个候选全被占用），在 OS 动态分配下通过，构成本次修复的有效护栏。
- 合并保留 `XcpliteV12AutoProfileTest.ResolvesAsamV14EightBitFromRealV12Slave`：使用真实第三方 V1.2 TCP 从站验证自动 framing 协商选中 ASAM V1.4/8 bit，确保端口分配回归测试与既有真实线路 profile 证据同时保留。

## 验证状态

Windows 本机，Visual Studio 17 2022 生成器 + Release 配置，`cmake-build-vs`：

- `cmake --build cmake-build-vs --config Release --target XcpliteV12InteropTest` 构建成功。
- `XcpliteV12InteropTest.exe --gtest_filter='*V12*'`：`7 tests from 4 test suites ran. [ PASSED ] 7 tests.`，包含端口分配回归和自动 profile 真实对端测试。
- 新用例单独运行通过（69 ms），未触发 SKIP，说明其成功占住了旧候选端口；从站实际端口为操作系统动态分配（日志中为 `port=55319` 等）。
- 未整体运行 `ctest` 全量测试，也未在 Linux / macOS 上验证；本次结论只覆盖上述 6 个用例。

## 边界与遗留

- 本次合并将 V1.2 从站端口分配修复与自动 framing profile 的真实对端测试整合到 `feat/tcp-a2l-wire-evidence`；需以合并后的 CI 结果确认相关变更在目标分支通过。
- 本机 `netsh` 显示本机保留段为 `50000-50059`（administered，实测不阻止 bind）与 `63940-64739`，而失败时基址 51556 在本机不处于保留段，因此无法在本机复现同一失败路径；修复的正确性由"旧候选端口全部被占用时从站仍能就绪"这一等价场景验证。
- 端口仍是"申请后释放、再交给子进程绑定"的两步，理论上存在被其他进程抢占的窗口。仍保留了 16 次重试作为兜底，但没有消除该窗口。
