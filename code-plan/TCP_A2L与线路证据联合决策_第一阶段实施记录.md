# TCP A2L 与线路证据联合决策：第一阶段实施记录

## 状态

实施进行中（Windows 侧）。选择逻辑、A2L 扫描器与受控快照均已实现并接入 `xcp_master`；快照的失败关闭路径已在本机实测，成功路径因本机无"仅本人可写"目录而被显式跳过，因此 A2L 正向 alignment 证据在本机仍不可达。线路侧已补独立进程 V1.5 对齐夹具（8/16/32 + 三项负向）并通过；Windows 全量 `ctest` 为 1122/1122 通过、0 失败。POSIX（Linux/macOS）编译与运行、跨平台矩阵尚未执行；第二阶段组件提取需另经设计评审。

## 已确认设计

- `auto` 不能把首个 CONNECT 成功当作唯一识别；存在不可区分候选时在试连前报告歧义。
- 显式 `auto-legacy` 保留旧式首成功行为，告知非唯一识别及可能影响 ECU 会话。
- A2L alignment 仅在受限子集、共享受控临时快照、严格词法及端点交叉核对成功时可作为静态证据；不能核实时标记不可信。保留原文件，不修改受限第三方；不承诺抵御同权限恶意进程。
- 显式用户配置优先于可比 A2L 声明，报告双方及来源；不得把配置当成对端已识别事实。
- 自动测试通过工具内部共享决策代码及 C++ 独立进程夹具，覆盖两套 XCPlite 和 8/16/32；无独立 V1.5 对端或跨平台运行时需如实记录。
- 已批准的待实现设计：快照根目录增加 `--a2l-trusted-snapshot-root`，默认可执行文件所在目录下的 `tmp`；不存在时创建，存在则复用。无论默认还是显式目录，均需校验完整祖先、权限和平台文件系统安全条件；创建或校验失败时提示原因，不能信任 A2L alignment，改由用户显式指定。默认目录并不天然可信。用户已确认，同身份恶意进程、管理员及拥有备份/还原等 ACL 绕过特权的账户不在防护范围内；普通其他身份仍不可替换快照。
- 本阶段完成后，第二阶段组件提取须再次设计评审。

## 当前改动与测试

- 应用参数解析和执行路径已区分严格 `auto` 与 `auto-legacy`，帮助及 README 已更新；旧模式显示会话副作用与非唯一识别风险，未确定的对齐输出为“未确定”。
- 修正 `tests/xcplite_tcp_interop_test.cpp` 误将当前 XCPlite 对端称为 V1.2 的注释；独立 V1.2 门禁另行维护。
- Windows VS Release 增量构建 `xcp_master`、`XcpMasterSymbolPolicyTest` 成功；`ctest --test-dir cmake-build-vs -C Release --output-on-failure -R '^TransportSelection\.'` 为 33/33 通过（1.27 秒）。这些是选择逻辑测试，不等同真实设备或完整交互式应用验证；完整回归尚待执行。
- 两套真实 XCPlite TCP 测试夹具已使用 A2L 端点与应用共享的显式 V1.4/8 决策创建传输。Windows VS Release `ctest --test-dir cmake-build-vs -C Release --output-on-failure -R 'XcpliteV12|XcpliteTcpInterop'` 12/12 通过（7.12 秒）；夹具自身有 CONNECT 就绪探测，不是严格 auto 零连接证明，也不是独立 V1.5 对端验证。
- TCP 解码、协商和传输配置筛选回归：`ctest --test-dir cmake-build-vs -C Release --output-on-failure -R '^(TcpStreamDecoder|TcpFramingNegotiation|TcpTransport)'` 为 54/54 通过（6.05 秒）；这些测试不能代替独立 V1.5 对端或本计划新增的全部边界验收。
- 实际 CLI 拒绝重复 `--tcp-profile`，避免严格模式被后续兼容选项静默覆盖；未验证 alignment 的多个 TCP 实例不再按未知值去重。`ctest --test-dir cmake-build-vs -C Release --output-on-failure -R 'A2lTcpCandidateExperiment|TransportSelection|XcpMasterToolRejectsDuplicateProfile'` 在 Windows VS Release 下 37/37 通过（0.95 秒）。扫描实验只验证受限词法，不是 SDK 差分或生产可信证明。
- 新增 SDK 与候选扫描器差分样本：无声明及 8/16/32、主机名和地址均与桥接结果一致；重复字段、未知 alignment、反斜杠和未知字符存在 SDK 接受而候选扫描拒绝的情况。Windows Release 下 2/2 差分测试通过；这只是有限样本，不证明解析器或快照可安全用于生产。候选扫描器目前只接受极窄的 PROJECT/MODULE/唯一 IF_DATA/TCP 骨架，拒绝模块中的其他元素；因此不能假定它覆盖包含测量、标定等常见内容的真实 A2L，也不能以此声称真实对端的 A2L 自动对齐已实现。
- SDK 差分反例：修复前 `IF_DATA XCP/*c*/` 的候选扫描结果有值，但 `A2lBridge::Load` 失败，不能取得 SDK 端点。现已收紧实验性扫描器中 IF_DATA 协议名前后的原始空白规则，修复后双方均拒绝该反例；Windows Release 直接运行 `A2lGolden.exe --gtest_filter=DifferentialTest.*`，差分测试 3/3 通过；候选扫描器定向测试 2/2 通过。按 `^DifferentialTest\.` 筛选 CTest 时未找到测试，不能算测试通过。有限样本不能证明 SDK 选块等价，仍禁止接入可信路径。
- 显式 TCP 端点与 A2L 单一端点不匹配、或有多个实例时，丢弃不能归属当前端点的 TL 版本和 alignment 证据；在选择器中也拒绝误用未核验的 TCP alignment。Windows Release 选择逻辑 36/36 通过；UDP 路径不变。
- 本次 Windows Release 增量构建 `xcp_master`、`XcpMasterSymbolPolicyTest`、`libxcp_tests` 成功；TCP decoder/negotiation/transport、候选扫描和重复参数筛选共 57 项通过、1 项禁用（7.91 秒），禁用项不计作通过。
- 新增独立手工字节的 V1.5 8/16/32 对齐测试，覆盖长度 1–4 的连续帧、非零 FILL、单次合包和逐字节输入；Windows Release 定向测试 1/1 通过。它是自有解码器测试，不代表独立 V1.5 对端验证。
- 补充 Windows Release 回归：`ctest --test-dir cmake-build-vs -C Release --output-on-failure -R '^(TcpStreamDecoder|TcpFramingNegotiation|TcpTransport|XcpliteV12|XcpliteTcpInterop)'` 共 63 项通过、1 项禁用（11.43 秒）。此筛选仅匹配 V1.2 A2L 生成测试，并未覆盖所有 V1.2 互操作测试；禁用项不算通过。
- 额外边界诊断：`libxcp_tests.exe --gtest_also_run_disabled_tests --gtest_filter=TcpStreamDecoderFinish.DISABLED_HeaderOnlyIsTruncated` 未通过，V1.4 和 V1.5 在仅收到完整头且 LEN 非零后 EOF 均返回 `malformed=false`。此用例保持禁用以免把已知缺陷伪装成通过；尚未修改解码器，需单独设计修复及回归。
- 真实结构与选块审查：`tests/data/a2l/cpp_demo_V201_corrected.a2l` 原始模块级 XCP 块没有 TCP，不能当作 TCP 正例；文件另有测量和内存段嵌套 IF_DATA。SDK 优先查找多个作用域中的 XCPplus，再查 XCP；因此必须排除竞争 XCPplus 并核对 SDK 的选块事实，不能仅按端点数值匹配。Windows Release 已执行原样及插入 TCP/16 副本的差分测试 1/1：SDK 均可加载，原样无端点，插入后有一个 TCP/16 端点；实验扫描器均拒绝。8/32、嵌套竞争及特殊词法的真实结构差分矩阵仍待执行。
- Windows 快照安全审查补充：SDK 和 a2llib 会多次通过路径打开文件；写入句柄与 `std::ifstream` 的双向共享兼容性不能假设。候选方案是受保护目录内独占创建、写入及读回后改持只读且不共享写/删除的句柄，核验文件标识，并保留祖先与文件句柄至 SDK 完成；仍须证明祖先路径竞争、ACL、映射、特权及 SDK 打开兼容性，尚未实现或验证。独立 `tests/a2l_windows_share_guard_experiment.cpp` 在本机 MSVC 2022 Release 成功实测：只读守护句柄与 SDK 三次加载兼容，现有或后续写句柄均因共享冲突被拒绝；该实验未注册 CTest，使用普通临时目录，不能证明 ACL、路径安全和跨平台行为。
- 正向 A2L alignment 严格校验尚未实现：a2llib 外层与 XCP 子解析器的词法不同，未知字符可能被忽略，原始终止扫描还可能被字符串或注释中的 `/end IF_DATA` 提前截断；尚未证明受限词法与桥接层选块等价。安全快照还需防范非受信主体通过父目录替换路径，不能只依赖临时目录自身权限。SDK 会按路径多次访问文件且处理 INCLUDE；方案必须验证完整祖先权限、ACL、重解析与文件系统语义，并拒绝无法证明的环境，当前尚无此实现。现正分离验证器候选与快照方案，未经端到端差分和安全验证不启用正向信任。当前临时保守策略是 TCP 的 A2L alignment 不作为配置依据，显式 V1.5 必须指定 alignment；UDP 不变。不能将该策略视为完成 A2L 联合决策。
- 受限扫描器修正（本轮）：修掉 scoped enum 默认初始化写成 `kWord` 的编译错误；版本/端口改用 `ParseUnsignedLiteral()` 按字面量进制解析（此前 `std::stoull("0x104")` 只得 0，导致与 SDK 事实"看似不一致"而一律不信任）；标识符允许点号（`conv.bool` 等真实 A2L 名称），末尾点号仍拒绝。修正后 `tests/data/a2l/cpp_demo_V201_corrected.a2l` 全文件词法通过：原样无候选，插入 TCP/16 块后得到候选（version 0x104、port 5555、host ecu.local、alignment 16）。
- Windows VS Release 实测：`A2lGolden.exe --gtest_filter=DifferentialTest.*` 4/4 通过（含真实 fixture 原样与插入 TCP 的 SDK 差分、语法相邻注释反例、重复/词法反例）；`XcpMasterSymbolPolicyTest.exe --gtest_filter='A2lTcpCandidate*:A2lAlignmentCrossCheck*'` 5/5 通过。运行差分测试须把 `TEMP`/`TMP` 指向工程内可写目录，否则系统临时目录 `Access is denied`。
- 仍需：受控快照已实现并接入（见下节）；两套 XCPlite 的真实 A2L 正向对齐与 8/16/32 组合验收、跨平台运行均未执行。

## 受控快照实现与本机实测（本轮）

### 实现

- 新增 `tools/xcp_master/a2l_trusted_snapshot.hpp`（纯头文件、无新增依赖）：
  - `StageTrustedA2lSnapshot(source, root)`：读原文 → 建立根目录 → 校验根目录及全部祖先 → 独占创建快照 → 全量写入并 `FlushFileBuffers`/`fsync` → 以只读句柄读回并逐字节比对 → 返回守卫。任一步无法证明即 `trusted=false` 并给出中文拒绝点。
  - 守卫（`std::shared_ptr<void>`）持有只读句柄，SDK 解析期间路径始终有效；释放时关闭句柄并删除快照，避免残留标定数据副本。
  - Windows：`CreateFileW` + `FILE_FLAG_OPEN_REPARSE_POINT` 取目录句柄，`GetSecurityInfo` 读 DACL，拒绝重解析点；只读句柄的共享模式为 `FILE_SHARE_READ`，实测 SDK 仍可按路径多次打开，而任何申请写/删除的打开都会共享冲突。
  - POSIX：`lstat` 拒符号链接、拒组/其他可写、属主须为本人或 root；快照文件 `O_EXCL|0600` 创建后校验 `fstat` 属主。
- 接入 `tools/xcp_master/main.cpp`：新增 `--a2l-trusted-snapshot-root`（默认"可执行文件目录/tmp"），仅在 `--transport tcp` 下建立快照，SDK 改从快照路径解析；`A2L 受控快照`/`A2L 受控快照不可信: <原因>` 分别打到 stdout/stderr。
- A2L alignment 判据改为双源核对：`ScanA2lTcpCandidate(快照字节)` + SDK 端点事实 → `CrossCheckA2lTcpAlignment()`；只有 `kTrusted` 且两端点声明值一致才把该位宽交给选择器，否则仍置 `instance_alignment_unverified` 并提示显式 `--packet-alignment`。

### ACL 规则的两次收紧修正（均基于实测，而非推断）

- 跳过带 `INHERIT_ONLY_ACE` 的允许型 ACE：卷根常见的 `Authenticated Users:(OI)(CI)(IO)(M)` 不会作用于该目录本身，此前把它当成"他人可写"导致任何路径都被拒绝。
- 祖先目录与快照目录分用不同拒绝位宽：快照目录本身用 `kWriteLikeMask`（含数据写、删除、改 ACL/属主）；祖先用 `kAncestorForbiddenMask = FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL`。理由：祖先上"能在该目录内新建文件"（`C:\` 根上 `Authenticated Users:(AD)`、`D:\` 根上 `Authenticated Users:(M)`）不能改写快照内容，若一并拒绝则任何 Windows 卷根都无法通过，该能力将永远不可启用；而 `FILE_DELETE_CHILD` 才是"可删除/替换本路径"的关键权限。

### 本机实测（Windows，MSVC 2022，Release，`cmake-build-vs`）

- `XcpMasterSymbolPolicyTest.exe`（全量）：141 项 → 138 通过、3 跳过；`A2lGolden.exe`：52 项 → 51 通过、1 跳过（`Utf16LeBomExploratory`，先前既有跳过）。运行须把 `TEMP`/`TMP` 指向工程内可写目录。
- 快照用例 `SnapshotTest.*` 7 项：`RejectsMissingSource`、`RejectsEmptySource`、`RejectsRootThatIsAFile`、`RejectsRootWritableByOthers` 4 项通过；`StagesReadableCopyOfSource`、`CreatesDistinctFilesPerCall`、`DeletesSnapshotWhenGuardReleased` 3 项跳过。
- 跳过的原因（实测，非推测）：本机 `D:\project` 全链对 `BUILTIN\Users:(F)`、`NT AUTHORITY\Authenticated Users:(M)` 开放；`%TEMP%` 另有 `CodexSandboxUsers:(M)`；`%LOCALAPPDATA%` 自身虽只给 SYSTEM/管理员/本人，但当前受限会话在其下建目录被拒绝（诊断程序 `StageTrustedA2lSnapshot` 返回 `无法创建快照目录：C:\Users\songj\AppData\Local\libxcp_snapshot_base（Access is denied.）`）。因此本机不存在可证明的私人目录，成功路径以 `GTEST_SKIP` 显式标注为"不可验证"，未伪造通过。
- 两套真实 XCPlite 对端：`XcpliteV12InteropTest.exe` 5/5 通过（TCP+UDP，含标定写回）；`XcpliteIntegration.exe --gtest_filter='*Tcp*:*Interop*'` 8/8 通过（V2.2）。

### 结论与仍未完成

- 结论：受控快照与双源核对已实现，"失败关闭"在本机得到实测证据；"正向信任"在本机不可达，故 TCP 的 A2L alignment 依旧不作为配置依据，V1.5 仍需显式 `--packet-alignment`。这不是未实现，而是本机权限环境不满足已批准的前置条件。
- 仍未完成：①POSIX 分支（Linux/macOS）编译与运行验证；②跨平台 `ctest` 矩阵；③A2L 正向信任在本机仍不可验证（需权限环境满足已批准前置条件）。

## 独立进程 V1.5 对齐验收夹具（本轮）

### 为什么需要它

此前的对齐覆盖只有两类：自有编解码器单测（手工字节）与 loopback 夹具。二者都由同一份代码解码，属"自洽式证据"，不能证明 master 与**另一份 framing 实现**的实际线路行为。本轮把从站放进独立进程，且其 framing 由 `tests/tcp_test_slave.cpp` 独立实现，不复用生产 decoder。

### 实现

- `tests/xcp_v15_alignment_slave.cpp`：最小从站可执行文件 `xcp_v15_alignment_slave`，复用 `tests/tcp_test_slave.cpp` 的独立 framing，支持 `PACKET_ALIGNMENT` 8/16/32 与一个可读内存区，用于 CONNECT/SHORT_UPLOAD/断连重连。
- `tests/xcp_v15_slave_process.cpp` + `.hpp`：从站进程的启动、就绪等待、终止与日志接管，异常时保留双端日志便于定位。
- `tests/xcp_v15_alignment_interop_test.cpp`：9 项用例
  - `Alignment8_16_32/XcpV15AlignmentInteropTest.ReadsMemoryWithMatchingAlignment`（8/16/32 各一）
  - `Alignment8_16_32/XcpV15AlignmentInteropTest.ReconnectsAndReadsAgain`（8/16/32 各一）
  - `XcpV15AlignmentNegativeTest.MismatchedAlignmentNeverSucceeds`（master 用 16 连 8 对齐从站必须失败）
  - `XcpV15AlignmentNegativeTest.UnknownAlignmentIsRejectedByConfig`（未知对齐在配置层拒绝）
  - `XcpV15AlignmentNegativeTest.CorruptedLengthFailsInsteadOfSucceeding`（长度损坏必须失败而非"成功"）
- `tests/CMakeLists.txt:598-629`：两个目标 `xcp_v15_alignment_slave` / `XcpV15AlignmentInteropTest`，`gtest_discover_tests` 加 `RUN_SERIAL`（从站进程与端口独占）与 `TIMEOUT 180`；从站路径通过 `XCP_V15_SLAVE_EXECUTABLE`、临时根通过 `XCP_V15_TEST_ROOT` 编译期注入。
- 该组用例只覆盖对齐矩阵与三项负向行为，**不声称** V1.5 一致性认证，也**不声称**与第三方 V1.5 实现互操作。

### 实测（Windows，MSVC 2022，Release）

- `XcpV15AlignmentInteropTest.exe --gtest_color=no`：9/9 通过（约 2.2 s，TCP 单端口串行）。
- 前提：须把 `TEMP`/`TMP` 指向工程内可写目录（系统临时目录在本机受限）。

## 全量回归与本轮踩到的环境坑

- 首次全量 `ctest -C Release -j 4`（`cmake-build-vs`）：1113 项 → 98% 通过，**19 项失败**，6 项未运行。
- 19 项失败根因是**二进制陈旧**，不是逻辑回归：报错为 `Load 失败: AbiMismatch / liba2l ABI 版本不匹配或工厂失败（期望 10）`，而头文件 `thirdparty/a2l-sdk/include/liba2l/liba2l_api.hpp:90` 已是 `inline constexpr std::uint32_t kLibA2lAbiVersion = 11u;`——即相关 exe 仍是旧头编译产物、`liba2l.dll` 也未刷新。全量重建后同一批用例 20/20 通过。
- 全量重建注意：MSBuild 的 `CL.exe` 会因环境里同时存在两种大小写的代理变量而报 `error MSB6001: “CL.exe”的命令行开关无效。System.ArgumentException: 已添加项。字典中的关键字:“HTTP_PROXY”所添加的关键字:“http_proxy”`（同理 `NO_PROXY`/`no_proxy`）。构建前须清除这些重复变量；`$env:` 在 PowerShell 中大小写不敏感，清除不干净时改用 bash `unset http_proxy HTTP_PROXY https_proxy HTTPS_PROXY no_proxy NO_PROXY`。
- 重建后全量 `ctest -C Release -j 4`：**1122/1122 通过，0 失败**（约 110 s），6 项未运行且均非本轮引入：
  - `TcpStreamDecoderFinish.HeaderOnlyIsTruncated`（既有 Disabled，已知缺陷，见上）
  - `AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`（既有 gtest skip）
  - `SnapshotTest.StagesReadableCopyOfSource` / `CreatesDistinctFilesPerCall` / `DeletesSnapshotWhenGuardReleased`（本机无可证明私人目录，显式跳过）
  - `XcpMasterRenderBenchTest.RendersFrameWithFieldSizeCandidateList`（性能基准跳过）
