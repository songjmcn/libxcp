# XCPlite V1.2 TCP/UDP 能力补全实施记录

- 实施日期：2026-10-28
- 验证平台：Windows / MSVC Release
- 分支：`refactor/tcp-udp-platform-isolation`
- 对应专项结果：[XCPlite V1.2 专项验收矩阵](XCPliteV1.2_专项验收矩阵_结果.md)

## 1. 目标

在不修改 `thirdparty/XCPliteV1.2`、不改变现有 TCP/UDP 公共接口的前提下：

1. 修复 Windows 下 V1.2 示例 A2L 后处理因目标文件已存在而无法覆盖的问题；
2. 将 V1.2 UDP/TCP 真实跨进程最小命令闭环纳入可重复 CTest；
3. 用直接证据区分“V1.2 最小传输通过”和“V1.2 全功能（DAQ/标定/CAL page）仍未验证”。

## 2. 修改内容

### 2.1 `examples/xcp_slave_demo_v12/main.cpp`

新增 `ReplaceA2lFile(...)`（当前约 `:524-570`）：

- Windows 使用 `MoveFileExW` 的 `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` 覆盖替换；
- 若受限工作区返回 `ERROR_ACCESS_DENIED`，使用 `std::filesystem::copy_file(..., overwrite_existing)` 覆盖后清理临时文件；该回退不保证原子性；
- POSIX 继续使用 `std::filesystem::rename`；
- 保留临时文件回读校验；原子替换优先路径不会先删除目标。

原阻断已被复现并定位：目标文件由 XCPlite 生成器先创建，Windows `std::filesystem::rename` 不覆盖已存在目标，错误为 `替换 A2L 失败：Access is denied.`。修复后 A2L 可重复生成、桥接验证可完成、从站能进入监听阶段。

### 2.2 `tests/xcp_v12_transport_interop.py`

新增 Python 标准库跨进程探针：

- 启动真实 `xcp_slave_demo_v12` 子进程；
- 直接构造 XCPlite V1.2 `[DLC:uint16][CTR:uint16][packet]` 头；
- 分别覆盖 UDP 和 TCP；
- 每个模式先生成两次 A2L，验证覆盖替换、端点和 `READ_WRITE`；
- 发送 `CONNECT`、`GET_STATUS`、`GET_COMM_MODE_INFO`、`DISCONNECT`；
- 收集从站输出，失败时保留诊断；
- 明确不发送 `POLL`：V1.2 的 `0xFC` 是 `SYNCH`，不是后续版本的 `POLL`。

### 2.3 `examples/xcp_slave_demo_v12/CMakeLists.txt`

在找到 Python3 时注册：

- `XcpliteV12UdpInterop`；
- `XcpliteV12TcpInterop`；
- 两项 CTest 超时均为 60 秒；
- 使用 `$<TARGET_FILE:xcp_slave_demo_v12>`，不写死构建目录。

### 2.4 文档

- 更新 `examples/xcp_slave_demo_v12/README.md`：说明 Windows 替换策略、新门禁命令、V1.2 `SYNCH` 与 `POLL` 的区别，以及 DAQ/标定仍未纳入该最小门禁；
- 更新 `code-plan/XCPliteV1.2_专项验收矩阵_结果.md`：记录本轮执行证据、矩阵状态和剩余边界。

## 3. 测试结果

### 3.1 Release 构建

```powershell
cmake --build cmake-build-release --target clean
cmake --build cmake-build-release -j 4
```

结果：365/365 targets built successfully。

### 3.2 模型单测

```powershell
& '.\cmake-build-release\tests\XcpSlaveV12ModelTest.exe' --gtest_color=no
```

结果：12/12 passed。

### 3.3 V1.2 真实 TCP/UDP 门禁

```powershell
ctest --test-dir cmake-build-release -R "^XcpliteV12(Udp|Tcp)Interop$" --output-on-failure
```

结果：2/2 passed：

- `XcpliteV12UdpInterop`：通过，0.31 秒；
- `XcpliteV12TcpInterop`：通过，0.73 秒；
- 总耗时 1.10 秒。

命令级输出：

```text
PASS UDP XCPlite V1.2 CONNECT/STATUS/COMM_MODE/DISCONNECT
PASS TCP XCPlite V1.2 CONNECT/STATUS/COMM_MODE/DISCONNECT
```

### 3.4 相关回归

执行 52 项 XCPlite/示例筛选回归，43 通过、9 失败。9 项失败均在既有 `XcpSlaveDemoIntegrationTest` 创建系统临时目录时出现：

```text
create_directories: Access is denied.: "C:\Users\songj\AppData\Local\Temp\libxcp_xcp_slave_demo_..."
```

失败发生在被测断言之前，是当前环境系统临时目录 ACL 问题；V1.2 两项门禁已独立复跑并全绿。该环境问题不改变本轮 V1.2 TCP/UDP 门禁结果，但应在后续处理以免污染回归报告。

## 4. 当前能力边界（截至本记录末尾的增量）

已确认：

- Windows V1.2 模型层通过；
- A2L/AML 生成、双次覆盖、`READ_WRITE` 后处理和 a2lbridge 自校验通过；
- Windows 真实 UDP/TCP Master 测量读取通过；
- Windows 真实 UDP/TCP 工作页标定写入、回读及测量输出变化通过；
- 示例 Segment 0 的参考页1→工作页0复制、XCP 页切换与 GET_CAL_PAGE 校验通过。

仍未确认：

- V1.2 真实 DAQ；
- 全部标定类型/大小/边界以及 ECU/XCP 双侧 page 状态；
- 双 V1.2 进程并行；
- 完整 UDP/TCP 异常、CTR、截断和边界矩阵；
- Linux/macOS 同门禁。

因此目前只能标记“Windows V1.2 测量读取与示例标定写回通过”，不能标记“XCPlite V1.2 全功能兼容通过”。

## 5. 测量读取与标定写回验收增量

用户选择先补测量读取与标定后，新增 C++ Master 跨进程测试；用户明确不接受 Python 启动 V1.2 从站，因此 CTest 已改为注册 `XcpliteV12InteropTest`，旧 Python 探针留在仓库中但不再用于该门禁。

### 5.1 实现

- `tests/xcp_v12_slave_process.hpp/.cpp`：跨平台子进程夹具，Windows 用 CreateProcess、POSIX 用 fork/exec；独立运行 `xcp_slave_demo_v12`，CTest cleanup 终止回收子进程和临时文件；启动通过真实 XCP CONNECT/DISCONNECT 判就绪。
- `tests/xcp_v12_interop_test.cpp`：A2L 两次生成/覆盖后解析；TCP/UDP 各覆盖 A2L 地址测量读取、COPY_CAL_PAGE/SET_CAL_PAGE/GET_CAL_PAGE、`corrections[0]` 写回/回读，以及从测量数组观察修正生效。
- `examples/xcp_slave_demo_v12/CMakeLists.txt`：测试 target 只链接 libxcp Master/a2lbridge，不链接 V1.2 从站库，子进程 target 独立运行，防止 V1.2/V2.2 C 符号冲突；GTest discovery 全项 `TIMEOUT=60`、`RUN_SERIAL=TRUE`。
- `examples/xcp_slave_demo_v12/README.md` 与 `code-plan/XCPliteV1.2_专项验收矩阵_结果.md` 更新当前能力边界与命令。

### 5.2 验证

```powershell
cmake --build cmake-build-release --target XcpliteV12InteropTest -j 4
ctest --test-dir cmake-build-release --output-on-failure -R "XcpliteV12|XcpSlaveV12ModelTest"
```

结果：**17/17 passed，0 failed，3.13 秒**（模型12项 + A2L生成/覆盖1项 + Master测量/标定4项，Windows/MSVC Release）。初次试验发现 A2L `fast` 实例的 address extension 是 3 且 address 0 合法；测试已按 A2L 实际元数据校正，测量与标定跨进程用例全绿。
