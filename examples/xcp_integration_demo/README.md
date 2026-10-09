# xcp_integration_demo — 面向集成工程师的测量与标定示例

这是一个**可编译的示例程序**，展示本仓库 `libxcp` 当前提供的测量与标定 API。它不是完整 XCP 1.3.0 实现声明，也不能代替特定 ECU 的安全评审。

## 覆盖范围

- CONNECT、运行时能力查询、UDP/XCP 会话关闭。
- 本地 A2L 加载，或通过 `GET_ID + UPLOAD` 拉取 A2L；A2L 数据库适配与显式 symbol→event 绑定。
- `MeasurementSession` DAQ 配置/启动/停止，输出 raw bytes、物理值、DAQ/ODT、时间戳，并报告 received/dropped/decode errors/timestamp wraps。
- 原始标量临时标定：备份→写入→读回→恢复→再次读回；默认禁止写。
- CAL/PAG 能力查询、页/segment 信息查询；显式确认后可临时切换 XCP 页、临时冻结 segment、执行 MODIFY_BITS 写后恢复；COPY_CAL_PAGE 有单独高风险确认且不会自动回滚。
- Seed&Key 通过 `SeedKeyCalculator` 接入厂商算法，不含伪算法或内置 key。

明确不覆盖 STIM、Flash Programming、NVM 持久化，也不宣称完成商业工具/非 XCPlite ECU 互操作。

## 构建

在根目录打开 CMake 示例门（`LIBXCP_BUILD_EXAMPLES=ON` 会自动跟随打开
`LIBXCP_BUILD_XCPLITE_SLAVE`）：

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DLIBXCP_BUILD_TESTS=ON `
  -DLIBXCP_BUILD_EXAMPLES=ON
cmake --build build --config Release --target xcp_integration_demo
ctest --test-dir build -C Release -R ExampleXcpIntegrationDemo --output-on-failure
```

目前 CI/本地自动化证据是 Windows + VS2022 Release。Linux/macOS 尚未在本批实际运行。

## 一键运行 vendored cpp_demo

程序自动启动 XCPlite `cpp_demo`，上传 A2L，按已核实的 `SigGen1`/`SigGen2`/`mainloop` 事件绑定配置 DAQ，运行后自动停止 Slave。请使用隔离的 `--run-dir`；程序保留其中已有的 `.aml`/持久化文件，不自动删除用户文件：

```powershell
build\examples\xcp_integration_demo\Release\xcp_integration_demo.exe `
  --profile cpp_demo --run-dir build\cpp_demo_integration_run --seconds 5
```

也可以显式演示 `counter_max` U16 的临时原始值写入与恢复（本地 profile 的已知地址、字节序和默认原值；只适用于这个 vendored cpp_demo）：

```powershell
...\xcp_integration_demo.exe --profile cpp_demo `
  --run-dir build\cpp_demo_cal_run --seconds 4 `
  --write-address 0x80010000 --address-extension 0 `
  --write-value 1400 --expected-original e803 --confirm-write
```

`1400`/`e803` 是 little-endian 原始字节，不是物理值。命令执行会先核对原值；任何不匹配都会拒绝写入。此新示例验证内存写入/读回/恢复；在线标定对 DAQ 行为的实时影响已由既有 `ExampleCppDemoCalibration` / `measurement_demo --calibration-probe` 验证，不应把二者混为一项。

## 通用 ECU 运行方式

必须根据**目标 ECU 的 A2L、Slave 运行时查询和设备文档**明确填写事件绑定、DTO envelope 和 timestamp 单位。示例不会猜测通用 XCP 方言：

```text
xcp_integration_demo --a2l ECU.a2l \
  --symbols App::EngineSpeed,App::CoolantTemp \
  --symbol-event App::EngineSpeed=3 \
  --symbol-event App::CoolantTemp=7 \
  --event-meta 3:1:3 --event-meta 7:10:3 \
  --id-field relative-byte --header-bytes 4 \
  --tick-ns 1000 --timestamp-first-odt-only \
  --host 192.0.2.10 --port 5555 --seconds 10
```

上述数字是**格式示例，不是推荐 ECU 参数**。`--event-meta CHANNEL:CYCLE:UNIT` 中周期/单位应与目标 ECU 的 A2L 和 `GET_DAQ_EVENT_INFO` 对上；`--tick-ns` 只填设备文档/运行时证据给出的每 tick 纳秒数。0 可表示时间戳单位未知，程序此时保留 raw timestamp 而不制造换算值。每个待测符号必须有明确事件绑定；通道 0 是合法值，不能作为“未设置”标记。

若目标 Slave 支持本仓库验证过的 XCPlite dialect，可以传 `--xcplite-profile`，但仍需提供 `--event-meta` 和 symbol 绑定。该 helper 会查询 DAQ processor/resolution/event 并检查 profile；它不是通用 XCP 识别规则。

A2L 上传模式例：

```text
xcp_integration_demo --upload-a2l downloaded.a2l \
  --symbols App::EngineSpeed --symbol-event App::EngineSpeed=3 \
  --event-meta 3:1:3 --id-field relative-word --header-bytes 2 \
  --host 192.0.2.10 --port 5555
```

A2L 的 `/include` 文件必须与主 A2L 的相对路径关系保持完整，A2L 上传本身不一定包含所有 include 文件。

## 标定操作示例

### 原始内存临时写入

```text
--write-address 0x80010000 --address-extension 0 \
--write-value 1400 --expected-original e803 --confirm-write
```

仅接受 1–4 字节标量；`--confirm-write` 是必需的显式 opt-in。原值备份之后会尝试写入、读回、恢复和复读。如果写命令超时，程序先读当前值：仍为原值则不重试；等于目标值时才尝试一次恢复；与二者都不匹配时报告 outcome unknown，不盲目重放。目标超时后无法读回时，操作者须按 ECU 供应商恢复规程处理。

### CAL/PAG、页、segment

```text
--page-info 0:0
--set-page 0:1 --confirm-page-change
--freeze-segment 0 --confirm-freeze
```

页切换只切换 XCP 访问页，并尝试恢复原页；segment freeze 会查询原状态，只对未冻结 segment 做临时冻结并复原。目标不支持相应可选命令时会报告 unsupported/抛出协议错误；不要把它理解为所有 Slave 必须支持。

### MODIFY_BITS / COPY_CAL_PAGE

```text
--modify-bits 0x80010000:0:65535:1 --expected-original 00000000 --confirm-modify-bits
--copy-page 0:0:0:1 --confirm-page-copy
```

MODIFY_BITS 仅处理指定地址 DWORD，必须提供经现场核实的 4 字节 `--expected-original`；程序先比对原值、执行后复读并尝试恢复。命令超时结果可能未知，程序不会重发。上面的 `00000000` 只是格式占位，不是推荐 ECU 原值。COPY_CAL_PAGE 是页面写操作，**不会自动备份/回滚目标页**；仅可在经批准且已外部备份的台架/安全 RAM 页上使用。每次运行最多执行一个状态变更操作。

### Seed&Key 接入

库 API 的接入点形态如下；`VendorSeedToKey` 必须由 ECU 供应商提供或授权，必须保留 seed/key 的协议字节顺序，不能用通用占位算法替代：

```cpp
SeedKeyCalculator calculator =
    [](Resource resource, BytesView seed) -> Bytes {
        return VendorSeedToKey(resource, seed); // 集成人员实现
    };
const UnlockResult result = master.Unlock(Resource::CalPag, calculator);
```

示例不打印凭据，也不会因 `ERR_ACCESS_LOCKED` 自动触发解锁。

## 安全及能力边界

- 写操作默认关闭；只对明确目标地址/extension/类型、允许的值范围和目标 ECU 执行。优先使用台架/HIL 与供应商批准的 RAM calibration page，不在量产 ECU 上试写未知地址。
- 多字节 `WriteMemoryBytes` 不保证跨块原子性；示例只收 1–4 字节标量，但目标 ECU 的对齐/访问权限仍须核实。
- 页复制无法通过单一 XCP 命令自动回滚；目标页、备份和掉线恢复方案必须由操作者提前准备。
- cpp_demo profile 覆盖本仓库 Windows/UDP/本地 XCPlite 验证范围，不证明 Linux/macOS、真实 ECU、非 XCPlite Slave、Seed&Key 算法或商业工具互操作。
