# XCPlite Slave 协议调试 实施记录 批次17 —— 对手端集成（第一阶段读通 + 第二阶段写通）

> 依据计划：`code-plan/XCPlite_Slave_协议调试集成计划.md`
> 状态：**第一阶段 + 第二阶段全部完成并全绿**（结构体成员叶子路径按批次16 边界 SKIP）
> 构建开关：`LIBXCP_BUILD_XCPLITE_SLAVE`（默认 OFF，OFF 基线零影响）

## 0. 交付概览

| 里程碑 | 内容 | 结果 |
|---|---|---|
| Phase1-01 | CMake 构建集成（add_subdirectory(thirdparty/XCPlite)） | ✅ ON/OFF 双侧 configure+build 通过 |
| Phase1-02 | 专用测试 Slave `xcp_test_slave`（XCPlite 独立进程，UDP） | ✅ 运行时生成 A2L，绑定 127.0.0.1 |
| Phase1-03 | 进程管理夹具（启动/就绪探测/端口避让/优雅终止） | ✅ CONNECT/DISCONNECT 探测，无固定 sleep |
| Phase1-04 | XCP 基本协议走通（连接/参数/读/多块/重复会话/负例） | ✅ 10 用例全绿 |
| Phase1-05 | A2L 加载 + 基本/数组变量读取 + 一致性比对 | ✅ 6 用例（5 绿 + 1 计划内 SKIP） |
| Phase2 | 写回：标量/数组元素/结构体成员/嵌套叶子/多块/CalSeg | ✅ 8 用例全绿 |
| 门禁 | 存量回归 352 项（含 A2lSmoke/A2lGolden/A2lE2E/A2lIsolation） | ✅ 100% passed |

测试总数：`ctest -R Xcplite` → **24 项，1 计划内 SKIP（批次16 leaf），0 FAIL**；
存量 `-E Xcplite` → **352 项全绿**。

## 1. 对手端事实核证表（写前读，全部来自 XCPlite 源码/实测输出）

| # | 事实 | 证据位置 | 对本库的影响 |
|---|---|---|---|
| F1 | 默认配置 CASDD 寻址：绝对寻址 `ECU_ADDRESS_EXTENSION=0x01`、段寻址=0x00 | `src/xcp_cfg.h:159-167` | Master 读必须透传 A2L extension，不能假设 0 |
| F2 | `printAddrExt` 仅在 ext>0 时打印扩展字段 | `src/a2l.c:89-91` | SEG 符号 A2L 行无 extension token（扫描器需兜底，见 §4.3） |
| F3 | A2L 无条件写 `/include "XCP_104.aml"`，但库不产出 AML 文件 | `src/a2l_writer.c:474` | 测试 Slave 自写空壳 AML，保证桥接层 include 链预扫描可解析 |
| F4 | TRANSPORT_LAYER（XCP_ON_UDP_IP+ADDRESS）仅在绑定地址首字节≠0 时写出 | `src/a2l_writer.c:346-361` | 测试 Slave 必须绑 127.0.0.1（非 ANY），A2L 才有端点信息 |
| F5 | A2L 文件名：WRITE_ALWAYS → `<project>.a2l`（无 EPK 后缀，每次运行重写） | `src/a2l.c:337-348` | 固定文件名 → 夹具可预判路径；重写规避 ASLR 陈旧地址 |
| F6 | XCPlite 全局变量 ECU_ADDRESS = 进程镜像基址相对偏移（跨进程重启稳定） | 实测 A2L `0x19001` 量级 | WRITE_ONCE 复用 A2L 在"同一二进制重跑"下仍成立（我们仍用 WRITE_ALWAYS 求确定性） |
| F7 | CONNECT RES PROTOCOL_VERSION 字节 = `XCP_PROTOCOL_LAYER_VERSION>>8` = 0x01（仅 major；偏离规范 nibble 约定） | `src/xcplite.c:2078` | 测试换算加特例分支并**钉住断言**（对手端偏差如实记录，不静默） |
| F8 | 绝对寻址经 `XcpSetMta` 解析为进程指针并转 PTR(0xFE) 模式后 memcpy 写 | `src/xcplite.c:690-691` + `XcpWriteMta` | Phase2 直接写全局变量可行，无需 page/unlock |
| F9 | 默认构建**未编译** GET_SEED/UNLOCK（`XCP_ENABLE_SEED_KEY` 注释） | `src/xcp_cfg.h:343`、`xcplite.c:2197` | 真实行为 = ERR_CMD_UNKNOWN（非"Length 0 短路"）；用例按实然断言 |
| F10 | SET_MTA/SHORT_UPLOAD 的 CRO 均为 8 字节，ext@byte3、addr@byte4-7（union dw[1]） | `src/xcp.h:549-565` | 引产本库 SET_MTA 编码修复（§3.1） |
| F11 | 运行时生成 A2L 的 RECORD_LAYOUT 写 `FNC_VALUES 1 <TYPE> …`（1 起始） | 实测 A2L + `a2l_writer.c` | 引产 SDK B-4 可证性判定放宽（§3.2） |
| F12 | 全局数组注册为 `CHARACTERISTIC … VAL_BLK <addr> <RL> …  MATRIX_DIM n 1`，非 MEASUREMENT | 实测 A2L | 桥接层/扫描器须支持 VAL_BLK+RL+MATRIX_DIM 推导（既有能力，经 F11 修复后可用） |

## 2. 新增文件

| 文件 | 说明 |
|---|---|
| `tests/xcp_test_slave/xcplite_test_types.hpp` | Slave 与测试共享的结构体布局/常量/期望值（offsetof 单一事实来源） |
| `tests/xcp_test_slave/main.cpp` | XCPlite 测试 Slave 独立进程：基本标量、结构体、数组、嵌套（结构体含结构体+结构体数组）、CalSeg、600B 大块 blob；UDP 端口由 argv[1] 传入 |
| `tests/xcp_test_slave/CMakeLists.txt` | `xcp_test_slave` 目标（链接 `xcplite::xcplite`，Windows 附 ws2_32+_CRT_SECURE_NO_WARNINGS） |
| `tests/xcplite_slave_fixture.hpp/.cpp` | 子进程生命周期（Win: CreateProcess/Terminate；POSIX: fork/exec+SIGTERM/SIGKILL）、CONNECT 探测就绪、端口自动避让重试、A2L 轻量扫描（MEASUREMENT/INSTANCE/CHARACTERISTIC/TYPEDEF_STRUCTURE） |
| `tests/xcplite_protocol_test.cpp` | Phase1-04 十用例（连接/会话参数/五类标量/结构体/嵌套/数组元素/结构体数组/多块 UPLOAD/重复会话/越界负例） |
| `tests/xcplite_a2l_read_test.cpp` | Phase1-05 六用例（运行时 A2L 加载+端点、Find+读+ToPhysical、数组元素地址、INSTANCE 基址读、一致性 B-16、leaf 路径计划内 SKIP） |
| `tests/xcplite_write_test.cpp` | Phase2 八用例（u8/u32/f32 写、数组元素、结构体成员、嵌套叶子、600B 多块 DOWNLOAD、CalSeg 段寻址写读、Unlock 实然报错+会话存活） |
| `code-plan/XCPlite_Slave_协议调试集成计划.md` | 本批次所依据的计划（P1 轮） |

## 3. 修改的既有代码（互操作缺陷修复）

### 3.1 `src/command_codec.cpp` —— SET_MTA CRO 编码（生产代码）

**现象**：对 XCPlite 的 `SET_MTA` 被回 `ERR_CMD_SYNTAX`（UPLOAD 分块读 600B 失败）。
**根因**：libxcp 按旧口径编码 7 字节 `[F6][00][EXT@2][ADDR@3..6]`，与库自身
`SHORT_UPLOAD`（8 字节、EXT@3）都不一致；XCP 1.3 与 XCPlite（`xcp.h` F10 证据）
均为 `[F6][MODE][rsv][EXT@3][ADDR@4..7]` 8 字节。
**修复**：`EncodeSetMta` 改为 8 字节布局（MODE=0）。
**同步面**（全为测试设施，语义随协议码修正）：
`tests/udp_test_slave.cpp`（解析）、`tests/udp_test_slave_test.cpp`（4 条手工报文）、
`tests/memory_access_test.cpp`（ParseSetMtaAddress + ext 读取位）、
`tests/xcp_master_integration_test.cpp`、`tests/xcp_daq_test.cpp`（mock 解析）、
`tests/command_codec_test.cpp`（3 条黄金报文 + 长度断言 7→8）。

### 3.2 `thirdparty/a2l-sdk/src/liba2l_export.cpp` —— SDK 两处（自维护子工程，批次9 起持续开发惯例）

1. **udp_host 取值回退链**：`HOST_NAME → ADDRESS → IPV6`。
   上游 a2llib 把 ASAP2 的 `ADDRESS "IP"` 存进 `address_`（仅 `HOST_NAME` 进
   `host_name_`），而 XCPlite/Vector 生态只写 `ADDRESS` → 原实现下运行时 A2L 的
   端点主机恒空。未动上游 a2llib，仅改 SDK 采集。
2. **B-4 版式可证性放宽 1 起始书写**：`FNC_VALUES` 起始位置接受 `0`（本库
   golden 约定）与 `1`（ASAP2/CANape 惯例，XCPlite 输出），二者同语义
   "值从记录首槽开始"；>1 维持不可证拒绝。VAL_BLK 元素数仍必须由
   MATRIX_DIM 乘积证明（不猜测纪律不变）。

> 两处修复后 **Release prepared root 已重建**（build-sdk.ps1）；
> **Debug prepared 未同步**——列入 §6 遗留。存量 A2lGolden/A2lSmoke/A2lE2E
> 352 项回归全绿，证明放宽未破坏既有 0 起始语料判定。

### 3.3 构建集成（`CMakeLists.txt`、`tests/CMakeLists.txt`）

- 主树新增 `LIBXCP_BUILD_XCPLITE_SLAVE`（OFF 默认；开启时以 CMP0077 正常变量
  设定 XCPlite 子项目配置：default、无示例/测试/工具/安装）。**未修改
  thirdparty/XCPlite 任何文件**。
- `XcpliteIntegration`：仅 XCPLITE_SLAVE=ON 时构建；A2L 用例文件仅当
  `LIBXCP_BUILD_A2L` 同时开启时编入（运行期复制 liba2l.dll）。
- Slave 路径经 `XCPLITE_SLAVE_EXECUTABLE="$<TARGET_FILE:xcp_test_slave>"`
  编译定义注入，无硬编码绝对路径。

## 4. 测试设施设计要点

### 4.1 就绪探测（无固定 sleep）
`XcpliteSlaveProcess::Start()`：候选端口 = 45000+(pid%3000)+游标递增，逐端口
spawn → 存活检查 + `ProbeOnce`（真 XcpMaster CONNECT→DISCONNECT，300ms 超时）
循环至 4s 截止；Slave 早退（端口占用）自动换下一端口（最多 8 次）。
探测的 DISCONNECT 释放会话后，用例再建自己的 Master（真实对手端行为）。

### 4.2 A2L 轻量扫描器（协议层专用，非通用解析器）
只处理 Slave 自产的单行规整格式，按块类型取址（MEASUREMENT→`ECU_ADDRESS`
关键字；CHARACTERISTIC→`VAL_BLK/VALUE/...` 后随；INSTANCE→extension 前一
token，无 extension 时取 `/end` 前最后 0x token）；全部数值经
`ParseA2lNumber` 安全解析（注释含空格不再抛 stoul 异常——首轮测试实际踩中并
修复）。符号寻址的权威通路仍是桥接层（Phase1-05）；本扫描器只为协议层用例
提供运行时地址，且**必须使用 A2L 声明的 extension**（F1 核证落地）。

### 4.3 对手端偏差的测试策略
- F7（版本字节非 nibble 约定）：换算特例 + `EXPECT_EQ(c.protocol_layer_version, 0x01)`
  钉住事实——对手端将来改正时测试会失败提醒同步。
- F9（GET_SEED 未编译）：`Unlock` 用例断言实然 `ERR_CMD_UNKNOWN`（非假想
  Length-0 短路），并验证负响应后会话仍可用。
- CalSeg 写读（F8/RCU）：XCP 读路径读当前激活页，DOWNLOAD 后立读新值——实测
  证实（用例绿）。

## 5. 门禁执行记录（本机 Windows/MSVC/CMake 4.0，Release）

| 命令 | 结果 |
|---|---|
| `cmake -B cmake-build-xcplite -S . -DLIBXCP_BUILD_XCPLITE_SLAVE=ON [-DLIBXCP_BUILD_A2L=ON -DLIBXCP_LIBA2L_ROOT=...]` | configure 通过 |
| `cmake --build cmake-build-xcplite --config Release`（ON） | 全目标 0 error（xcplite 库仅 C4996 类告警已消） |
| `ctest -R Xcplite --output-on-failure` | **24 项：23 Passed + 1 Skipped（批次16 leaf），0 Failed** |
| `ctest -E Xcplite`（同目录） | **352 项 100% passed**（含 A2L 全套件 + A2lIsolation） |
| OFF 基线 configure + clean 全量 build（cmake-build-offcheck） | 构建图无 XCPlite 目标；构建通过（见 §7 备注） |

过程失败与修复实录（先红后绿，均留档于此）：
1. 7 用例 `invalid stoul argument` → 扫描器固定下标被注释空格破坏 → 关键字化取址 + 安全解析；
2. MultiChunk `ERR_CMD_SYNTAX@0xF6` → §3.1 SET_MTA 编码；
3. 全部符号 `0x80010000`+无 extension → CalSeg 构造把 A2L 生成器留在 SEG 模式，注册序须"先 CalSeg 后 `A2lSetAbsoluteAddrMode`"（cpp_demo 同约定）；
4. `remote_host=""` → §3.2-1；`PROTOCOL_VERSION_MAJOR` Error → F7 特例；`ByteSizeOf` B-3 拒绝 → §3.2-2；CalSeg 符号扫描不到 → F2 兜底分支。

## 6. 遗留与后续

| 项 | 说明 | 去向 |
|---|---|---|
| 结构体成员**叶子路径**（`MODULE::INSTANCE.member[ idx ].nested`） | 计划 §2/Phase1-05 注记：依赖批次16 STRUCTLEAF；本批协议层已用"基址+offset"覆盖等价读/写 | 批次16 实施时直接启用 `StructMemberLeafPaths*`（Slave 运行时 A2L 即批次16 计划的真实语料候选） |
| Seed&Key 真解锁闭环（计划 Phase2-03 原案） | XCPlite 默认未编译 GET_SEED/UNLOCK（F9）；需 `XCPLITE_CFG_OVERRIDE` 自定义头启用 `XCP_ENABLE_SEED_KEY` + Slave 侧 Appl 回调 | 属对手端配置扩展，另行立项（不改 thirdparty 源文件，机制是其公开的 override 头） |
| SDK **Debug prepared** 同步 §3.2 两修复 | 本批仅 Release 重建（测试全 Release） | 下一次 A2L 门禁批次双侧重建时带入 |
| 预置 A2L 基线入库（Phase1-06） | 运行时 A2L 含进程相关偏移（镜像基址/OS 布局），逐次运行不保证字节稳定 | 改为"结构断言回归"（用例 1/2/5 已承担），不入库快照 |
| `xcplite_runs/` 构建垃圾 | 已加入 `.gitignore`；跑测残留于 build tree | 由目录清理策略处理 |

## 7. 复现步骤（相对路径）

```bash
# 对手端集成构建（Release；A2L 用例需要 prepared root）
cmake -B cmake-build-xcplite -S . -DCMAKE_BUILD_TYPE=Release \
      -DLIBXCP_BUILD_XCPLITE_SLAVE=ON \
      -DLIBXCP_BUILD_A2L=ON \
      -DLIBXCP_LIBA2L_ROOT=build/liba2l-prepared/msvc-x64-release
cmake --build cmake-build-xcplite --config Release
ctest --test-dir cmake-build-xcplite -C Release -R Xcplite --output-on-failure
ctest --test-dir cmake-build-xcplite -C Release -E Xcplite   # 存量回归
```

（本机注：MSBuild 串行构建；gtest 离线源经
`-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=cmake-build-release/_deps/googletest-src`
复用，与既有约定一致。）

## 8. 变更记录

| 轮次 | 内容 |
|---|---|
| B17-1 | 计划拆解为 9 项小任务并全部落地：构建集成、测试 Slave、进程夹具、协议 10 用例、A2L 6 用例、写回 8 用例；三处互操作修复（SET_MTA 生产码、SDK udp_host、SDK FNC_VALUES 口径）；24+352 全绿；OFF 基线不变 |
