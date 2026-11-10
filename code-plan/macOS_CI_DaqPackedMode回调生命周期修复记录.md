# macOS CI：DAQ Packed Mode 测试回调生命周期修复

## 问题与根因

GitHub Actions Job `129790748793` 的 CTest 阶段有两个用例段错误：
`DaqPackedModeSession.RejectsWhenEventIsNotPeriodic` 和
`DaqPackedModeSession.RejectsWhenEventLacksPackedProperty`。

两个测试把局部 `PackedSessionSlave` 以引用捕获交给 `MockTransport`。
测试作用域结束后，`XcpMaster` 析构期间仍可能经由 transport 回调访问该对象，
造成悬空引用和未定义行为。

## 修复

上述测试的回调按值捕获 `PackedSessionSlave`，并声明为 `mutable`，使脚本化
Slave 副本在回调存续期内保持有效。当前 `tests/daq_packed_mode_test.cpp`
中的两个回调均已采用该方式。

## 验证

- 已检查两个测试的回调捕获，均为按值捕获。
- 已尝试配置 Release 测试构建；本地环境缺少 Boost CONFIG 包
  （locale、filesystem、process），CMake 配置失败，因此无法在此环境运行目标测试。
- 原 macOS Actions 日志中除上述两个用例外，其余执行用例均通过；本地无法复现验证。
