# XCP 测量标定差距收口 P2 模拟实施记录

## 范围与边界

按用户批准的方案，仅补充 UDP Loopback 测试和计划记录；未修改 Master 生产实现、公开 API 或 `thirdparty/XCPlite`。当前没有指定独立外部 ECU/Slave，因此这些测试只证明本仓库模拟链路行为，不构成真实受保护标定互操作验收，P2 仍未完成。

## 实施内容

在 `tests/xcp_udp_loopback_test.cpp` 新增 `SeedKeyEndToEnd.ProtectedCalibrationWriteRequiresUnlockAndRelocks`：

1. 启用 `UdpTestSlave` 的 DOWNLOAD 模拟并保护 CAL/PAG 资源。
2. 锁定时 `WriteMemoryBytes` 必须返回 `ProtocolError(AccessLocked)`。
3. 使用 `test::TestKeyAlgorithm` 解锁，确认测试地址原值未被拒绝请求改变，写入测试数据后读回一致。
4. DISCONNECT 后以新建的 `XcpMaster` 重新 CONNECT，确认 GET_STATUS 恢复 CAL/PAG 保护位，未解锁读取再次被拒绝。

测试使用本地模拟内存地址 `0x70012340` 与固定四字节测试值，不涉及设备生产区。

## 测试结果

- Windows Release 构建：`cmake --build build-v09 --config Release --target libxcp_tests -j 1` 成功，0 警告、0 错误。
- 聚焦受保护 CAL / Seed&Key：3/3 通过。
- `SeedKeyEndToEnd|UdpLoopback`：25/25 通过。
- 全量 Release CTest：498 项中 497 通过、1 项按既有配置 Skip，无失败。Skip 为 `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`。
- `git diff --check` 无空白错误；Git 提示若干既有文件的 LF 将被转换为 CRLF。

## 观察与未决

最初尝试在同一个 `XcpMaster` 对象 DISCONNECT 后立即再次 CONNECT，触发 `当前状态不允许进入恢复: Connecting`。最终测试改为销毁旧 Master 并创建新实例，以验证同一模拟 Slave 新会话重锁；此次不扩展到修复同实例重连行为，需后续独立确认其是否属于受支持用法。

**结论：**模拟保护写回闭环已覆盖；独立 Slave/ECU、真实保护策略与密钥互操作仍未验证，不能宣称完整受保护标定已通过。
