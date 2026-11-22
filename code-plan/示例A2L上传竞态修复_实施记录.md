# 示例 A2L 上传竞态修复 — 实施记录

## 1. 问题现象

按 CI 精确形态（`LIBXCP_ENABLE_LOG=ON` + tools + examples + tests）在 Linux/GCC 11 跑全量 ctest：

```
1428/1446 Test #1428: ExampleXcpIntegrationDemoCppDemo ...***Failed 0.45 sec
...
Error: A2L 中找不到符号：cpp_demo::temperature
```

生成的 A2L（`uploaded.a2l`）里只有 4 个 `MEASUREMENT`（`SigGen1`/`SigGen2` 各自的 `value_`、`time`），
缺少 `temperature`、`speed`、`counter`、`sum`。

## 2. 对照实验（定位依据）

| 条件 | 结果 | A2L 中 `MEASUREMENT` 数 |
| --- | --- | --- |
| Linux，`LIBXCP_ENABLE_LOG=ON`（同一构建目录） | 失败，连跑 3 次全失败 | 4 |
| Linux，同一目录改成 `LIBXCP_ENABLE_LOG=OFF` 重建 | 通过 | 10 |
| Windows，`LIBXCP_ENABLE_LOG=ON` 手动跑同一命令 | 通过（11939 帧） | 10 |
| Windows，默认 `LIBXCP_ENABLE_LOG=OFF` | 通过 | 10 |

- `cpp_demo` 从进程二进制在两种配置下**完全相同**（同为 166232 字节，都含 `Motor temperature` 字符串），
  所以不是从进程被编译成了不同行为。
- 差异只在主机侧：`xcp_integration_demo` 在 LOG=ON 时链接 glog 并输出日志，启动/连接时序随之改变。

## 3. 根因

1. `StartLocalCppDemo()`（`examples/xcp_integration_demo/xcp_integration_demo.cpp`）只负责拉起从进程，**没有任何就绪等待**。
2. 紧接着 `FetchAndSaveA2l()` 立刻 CONNECT 并上传 A2L。XCPlite 的 A2L 是 **FINALIZE_ON_CONNECT**：连接那一刻的内容就是最终内容。
3. 从进程的 `kParameters`/`SigGen1`/`SigGen2` 标定分段是在 `main` 里创建的，`temperature`/`speed`/`counter`/`sum`
   更是主循环跑起来之后才注册完。主机连得过早 → 上传到的是「半个 A2L」。
4. 更棘手的是：从进程发现 run-dir 里已有同名同版本 A2L 时会**跳过生成并直接复用**，
   于是第一次传得早之后，后面每次重启都沿用那份残缺文件，问题会自我固化。

即：这是示例里一个**启动时序竞态**，不是格式化后端改动引入的缺陷；只是把 CI 换成 GCC 11 后时序变化把它暴露出来了。

## 4. 改法（只动示例，不动库）

`examples/xcp_integration_demo/xcp_integration_demo.cpp` 增加两个辅助函数，并把上传改成「校验 + 有限次重试」：

| 函数 / 改动 | 作用 |
| --- | --- |
| `bool UploadedA2lCoversSymbols(const Options&)` | 用 `a2l::A2lBridge::Load` 加载刚写盘的 A2L，逐个 `Find()` 校验 `options.symbols` 是否齐全 |
| `void RemoveGeneratedA2l(const Options&)` | 清理 run-dir 里由从进程生成的 `*.a2l`（保留随包的 `XCP_104.aml`），解除「同名即复用」的固化 |
| 上传块 | 最多 5 次尝试：每次先让步 `400ms × 尝试次数`（0.4/0.8/1.2/1.6/2.0 s），再上传并校验；失败则重启从进程、清理生成物后重试 |

两个关键细节（都是第一版漏掉、实测补上的）：

1. **上传本身抛错也要重试**：第一次实现只重试「符号不全」，而重启从进程后若 CONNECT 还没就绪，
   `FetchAndSaveA2l` 会直接抛 `等待 XCP Slave CONNECT 超时：当前状态不允许进入恢复: Connecting`，
   整个用例就以这个错误结束。现在把异常收敛为 `upload_error` 参与重试。
2. **让步要放在上传之前**：XCPlite 在收到 CONNECT 时才定稿 A2L，事后重试没有意义；
   必须先让从进程有时间跑到「标定量注册完成」，再去连。

设计取舍：

- **不引入固定 sleep 作为主手段**：延时只是给从进程推进的机会，判据始终是「A2L 里符号是否齐全」。
- **校验用 A2L 数据库而不是文本 grep**：`Find()` 是产品侧同一套解析逻辑，避免格式变体误判。
- **重试必须重启从进程并删掉旧 A2L**：否则新进程看到同名同版本文件会跳过生成，永远复用残缺内容。

## 5. 验证结果

| 场景 | 结果 |
| --- | --- |
| Linux（CMake 3.22.1 + GCC 11.4 + LOG=ON）连跑 5 次该用例 | **5/5 通过**，每次生成的 A2L 均为 **10** 个 `MEASUREMENT` |
| Linux（CMake 3.31 + GCC 11.4 + LOG=ON）单跑 | `Passed 5.53 sec`，10 个 `MEASUREMENT` |
| Windows LOG=ON 重建后手动跑 | `RUN_EXIT=0`，11939 帧，10 个 `MEASUREMENT` |
| Windows 默认 LOG=OFF 重建 + `ctest -R ExampleXcpIntegrationDemoCppDemo` | `100% tests passed`，11.33 s |

未修复前（同一 CMake 3.22.1 目录，LOG=ON）：该用例每次失败，`uploaded.a2l` 只有 4 个 `MEASUREMENT`。

## 6. 全量回归

按 CI 精确形态（Ubuntu 22.04 / **CMake 3.22.1** / GCC 11.4 / `LIBXCP_ENABLE_LOG=ON` / tools+examples+tests）：

```
100% tests passed, 0 tests failed out of 1445
Total Test time (real) = 674.43 sec
```

未运行 5 项均为既有 Disabled/Skipped（`TcpStreamDecoderFinish.HeaderOnlyIsTruncated`、
`Ag1_Cto8`、`SnapshotTest` 三项）。修复前同一目录为 `1444 通过 / 1 失败`，失败项即本用例。

## 7. 已知限制

1. 重试上限 5 次（合计让步 2 s）；从进程若异常缓慢仍可能耗尽重试并抛错，错误信息会明确指出「缺少待观测符号」。
2. 校验只在 `--profile cpp_demo` 且 `--symbols` 非空时生效；其它 profile 维持原行为。
3. 没有真正的「就绪信号」：从进程的输出继承父进程控制台、未被捕获，无法用日志行判定就绪，因此改用「结果校验 + 重试」达成等价效果。
4. 该修复只针对示例与测试；不要据此认为 libxcp 侧存在同样问题——库侧 `FetchA2lViaUpload` 是对协议的原样封装。
