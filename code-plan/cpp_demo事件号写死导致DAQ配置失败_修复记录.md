# cpp_demo 事件号写死导致 DAQ 配置失败修复记录

## 问题

Linux CI 中 `ExampleXcpIntegrationDemoCppDemo` 失败：

```text
SET_DAQ_LIST_MODE (daq=0,event=0) event channel redefinition, DaqListEventChannel=1
XCP error: Slave 返回负响应 命令 0xE0 收到 ERR ERR_DAQ_CONFIG
```

## 根因

`examples/xcp_integration_demo/xcp_integration_demo.cpp` 的 `cpp_demo` 预设写死事件号
（SigGen1=0、SigGen2=1、mainloop=2），而从站实际为 mainloop=0、SigGen1=1、SigGen2=2。
从站启用动态寻址，要求 DAQ 列表事件与 `WRITE_DAQ` 地址里编码的事件一致，不一致即拒绝。
SigGen1/SigGen2 在各自线程中注册，事件号顺序不保证跨平台、跨调度稳定，写死数字
在任何固定映射下都不可靠。

## 修改

- `ApplyCppDemoProfile` 不再写死事件元数据与符号绑定。
- 新增 `DeriveEventsFromA2l`：A2L 加载后，事件元数据取自 IF_DATA XCP 的事件列表，
  符号绑定取自符号唯一的固定 DAQ 事件；命令行显式给出的通道与绑定优先。
  符号找不到、或没有唯一固定事件时明确报错。
- `cpp_demo` profile 下，"每个符号必须有事件绑定"的校验推迟到推导之后。
- `main` 中 `options` 由 `const` 改为可变，README 同步更新。
- 运行时 `BuildMeasurementRuntimeProfile` 仍会与从站 `GET_DAQ_EVENT_INFO` 交叉校验。

## 验证

- GCC（C++20）对该示例源文件语法检查通过。
- 完整构建与 `ExampleXcpIntegrationDemoCppDemo` 未能在本地运行：本地缺少
  `thirdparty/a2llib` 子模块内容与 Boost `process` 组件，需以 CI 结果为准。
- 本次不新增单测：该示例已有真实拉起从站的集成测试覆盖此路径。
