# TCP XCP Master 示例

该示例演示通过 TCP 连接 XCP Slave，加载 A2L、读取固定宽度数值标量测量的物理值，并在用户显式启用时写入一个标定标量并回读。所有地址、地址扩展、数据类型和物理换算均来自 A2L；示例不硬编码符号偏移。

## 自动模式（默认）

自动模式会在独立运行目录生成 `xcp_slave_demo.a2l` 和 `XCP_104.aml`，启动本仓库的 TCP `xcp_slave_demo`，完成 XCP 连接和 A2L 测量读取，退出时清理自启动从站进程。内置从站明确使用 `AsamV14` profile 与 8-bit packet alignment。

```sh
xcp_master_tcp
xcp_master_tcp --run-dir ./tcp-demo --port 5555
```

默认只读。只有明确给出 `--write-calibration` 和 `--calibration-value` 才会写入；自动模式未指定 `--calibration-symbol` 时使用演示 A2L 标量 `xcp_slave_demo::cal.basic.real_32`：

```sh
xcp_master_tcp --run-dir ./tcp-demo --port 5555 \
  --write-calibration --calibration-value 2.375
```

写入前通过 A2L `FromPhysical` 检查类型、转换和范围；写后回读。写入成功后**不会恢复原值**。如果回读失败，程序会明确警告目标可能已改变，也不会尝试回滚。

## 外部从站模式

外部模式不会启动、停止或以其他方式管理外部从站进程。调用方必须提供 A2L 文件、目标地址/端口、TCP framing profile、packet alignment，以及用于读取的 A2L 测量符号：

```sh
xcp_master_tcp --mode external \
  --host 192.0.2.10 --port 5555 \
  --a2l ./ecu.a2l \
  --tcp-profile asam-v1.5 --alignment 8 \
  --measurement ECU::VehicleSpeed
```

支持的 profile 为 `asam-v1.4`、`asam-v1.5`；对齐值为 `8`、`16`、`32`（单位为 bit）。程序不根据 XCP 版本、A2L 对齐信息或数据帧内容推断 profile，也不在已建立的 TCP 流中切换 profile。选择错误会导致连接或解码失败；断链后由调用者重新启动程序，不自动重连。

外部标定需要额外明确指定 `--write-calibration`、A2L 全限定标量符号和物理目标值：

```sh
xcp_master_tcp --mode external \
  --host 192.0.2.10 --port 5555 --a2l ./ecu.a2l \
  --tcp-profile asam-v1.5 --alignment 8 \
  --measurement ECU::VehicleSpeed \
  --write-calibration --calibration-symbol ECU::TargetValue \
  --calibration-value 12.5
```

只接受 A2L 中可直接解析的数值 `CHARACTERISTIC/VALUE` 标量，并要求标记为 `READ_WRITE`；数组、结构体、其他 characteristic 类型及不可逆/越界物理映射会被拒绝。A2L 声明不能替代目标 ECU 的操作授权或现场安全流程；使用者需确认写入目标、单位、限值和车辆/台架状态。写后不恢复原值。

## 构建与测试

在仓库构建目录启用示例和测试：

```sh
cmake -B cmake-build-release -S . -DCMAKE_BUILD_TYPE=Release \
  -DLIBXCP_BUILD_EXAMPLES=ON -DLIBXCP_BUILD_TESTS=ON
cmake --build cmake-build-release --target xcp_master_tcp XcpMasterTcpIntegration -j 4
ctest --test-dir cmake-build-release -R XcpMasterTcpExample --output-on-failure
```

集成测试覆盖自动启动/读取，以及外部模式标定写入后通过独立会话验证目标值仍保留。