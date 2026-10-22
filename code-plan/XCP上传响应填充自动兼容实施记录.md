# XCP 上传响应填充自动兼容实施记录

## 目的

让协议层自动兼容 XCPlite 异步 XCP 上传响应的 4 字节对齐尾部填充，同时保持标准长度响应不变，并继续拒绝其他无法解释的错误长度。用户确认采用此方案（m00886）。

## 根因证据

- Debug 工具成功连接运行中的 `cpp_demo`，读取 `cpp_demo::SigGen1.time`、`SigGen1.value_`、`SigGen2.time` 均复现“请求 8 字节、响应数据 11 字节”。
- XCPlite `thirdparty/XCPlite/src/queue32.c` 的队列路径把 XCP 包补齐到 4 字节边界，填充计入线上长度；`thirdparty/XCPlite/src/xcp_cfg.h` 声明地址扩展 0x02–0x0F 用于动态异步寻址。
- 对期望数据长度 N，正响应包括 1 字节 PID，因此已知填充响应的数据段长度为 `align_up(1 + N, 4) - 1`；N=8 时即 11。ASAM 精确长度响应仍为 N。

## 实施改动

- `include/libxcp/command_executor.hpp`：将私有响应长度校验参数改为可变正响应，以便只在兼容分支裁剪尾部。
- `src/command_executor.cpp`：上传响应先严格接受精确长度；仅 UPLOAD/SHORT_UPLOAD 的长度精确等于 `align_up(1 + expected, 4) - 1` 时裁剪为期望长度；其他长度继续抛 `MalformedPacket`。无需用户开关、A2L over-read 或公开 API 变化。
- `tests/recovery_test.cpp`：覆盖 UPLOAD、SHORT_UPLOAD 的 8→11 字节填充裁剪，以及不匹配填充长度仍拒绝。

## 验证

- Release 配置构建 `libxcp_tests` 与 `xcp_master` 成功（MSVC 2022）。
- `ctest --test-dir cmake-build-release -C Release -R "UploadResponsePadding|RecoveryTimeout" --output-on-failure`：11/11 通过（新增 3 项 padding 测试及恢复回归）。
- Release CLI 连接当前运行的 Debug `cpp_demo.exe`（127.0.0.1:5555），加载 A2L 后成功读取 `cpp_demo::SigGen1.time`、`cpp_demo::SigGen1.value_`、`cpp_demo::SigGen2.time`；不再出现命令 244 长度 11/期望 8 的错误。
- 全量 `ctest --test-dir cmake-build-release -C Release --output-on-failure`：487 项中 486 通过、1 项既有 AgIntegration 参数化测试按设计跳过，0 失败。
- `git diff --check` 通过。
