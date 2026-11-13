# B0 批次实施记录：规范纠偏与 GET_DAQ_LIST_MODE

- 实施日期：2026-11-04
- 依据：`code-plan/XCP_V1.5_测量标定修复与测试实施计划.md`（§3 B0、§6.1、§8 时序）
- 规范：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`（原始 PDF 为权威）；Markdown 转换版仅用于检索定位
- 代码基线：C++20，Windows/Release/Ninja + MSVC 19.44.35229，Release 构建目录 `cmake-build-release`

## 1. 批次范围

B0 只做两件事：

1. **规范纠偏**：把此前的“完整性分析”文档里对命令编号与支持等级的错误标注改正，撤销无定义的百分比结论；
2. **能力基线补全**：实现 `GET_DAQ_LIST_MODE`（0xDF）四层贯通，使 Master 能回读 Slave 侧的 DAQ 列表**当前状态**，而不只是能写。

不在本批范围：标定 Block 下载（B1）、`WRITE_DAQ_MULTIPLE` 与 `DTO_CTR_PROPERTIES`（B2）、时间关联（B3）、Packed（B4）、A2L 复杂标定对象（B5）。

## 2. 规范纠偏结果（B0-1）

依据规范 Table 25/27/28/29 原文核对，确认并修正 5 处：

| 项目 | 原表述（错误） | 更正后 |
| --- | --- | --- |
| `DOWNLOAD_NEXT` | 0xEF / 0xCF | **0xEF**（0xCF 是 `PROGRAM_RESET`，属编程命令） |
| `DOWNLOAD_MAX` | 0xEE / 0xCE | **0xEE**（0xCE 是 `GET_PGM_PROCESSOR_INFO`） |
| `DOWNLOAD_MAX` 语义 | 变长下载 | **固定满帧大小**，不是变长 |
| `SHORT_DOWNLOAD`、`MODIFY_BITS`、`READ_DAQ` | 标为 Mandatory | 均为 **Optional** |
| “测量 85% / 标定 90%” | 完成度百分比 | **撤销**：规格未定义“百分比”口径，不可作为验收指标 |

支持等级改用规范原文措辞：`Mandatory for CAL/PAG`、`Mandatory for DAQ/STIM`、`Optional`。

`code-plan/XCP_V1.5_测量标定功能完整性分析.md` 已同步：新增“纠偏记录（B0 批次修订）”，表格支持等级全量替换，3.1/3.2 改为“证据状态”表，结论去掉百分比。该文档现为**历史审计记录**，当前实施以计划文档为准。

## 3. 代码变更（B0-2）

四层贯通，遵循既有 `GET_DAQ_LIST_INFO` 的分层结构，不新增无关接口。

### 3.1 命令码与状态位定义

`include/libxcp/protocol_types.hpp`

- `:100` 新增 `GetDaqListMode = 0xDF`，位于 `SetDaqListMode = 0xE0` 与 `StartStopDaqList = 0xDE` 之间；
- `:203` 新增 `enum class DaqListStatusBit : std::uint8_t`：
  `kNone=0x00`、`kSelected=0x01`、`kStim=0x02`、`kDtoCounter=0x08`、`kTimestamp=0x10`、`kPidOff=0x20`、`kRunning=0x40`、`kResume=0x80`；
- `:215`/`:836` 新增 `constexpr bool HasDaqStatus(DaqListStatusBit status, DaqListStatusBit mask) noexcept`；`:219`/`:830` 新增 `operator|`。

**为什么另立枚举**：规范里 bit0 在 `SET_DAQ_LIST_MODE` 请求中是 `ALTERNATING`，在 `GET_DAQ_LIST_MODE` 响应中是 `SELECTED`。数值相同、语义不同，若复用 `DaqListModeBit` 会让“把回读值写回去”这种错误在类型上合法。测试 `GetDaqListModeParser.StatusBitsAreNotSetModeBits` 把这条约束钉死。

### 3.2 Codec

- `include/libxcp/command_codec.hpp:291`：`[[nodiscard]] Bytes EncodeGetDaqListMode(std::uint16_t daq_list) const;`
- `src/command_codec.cpp:314`：CTO 为 `[0xDF][0x00 reserved][DAQ_LIST WORD]`，共 4 字节，WORD 按会话端序写入。

### 3.3 Parser

- `include/libxcp/response_parser.hpp:127`：`struct GetDaqListModeResponse { std::uint8_t mode; std::uint16_t reserved; std::uint16_t event_channel; std::uint8_t prescaler; std::uint8_t priority; };`
- `include/libxcp/response_parser.hpp:434`：`[[nodiscard]] std::optional<GetDaqListModeResponse> ParseGetDaqListMode(BytesView res_data) const;`
- `src/response_parser.cpp:34`：`kGetDaqListModeResMinSize = 7`；
- `src/response_parser.cpp:347`：按 `mode=[0]`、`reserved=ReadU16(1)`、`event_channel=ReadU16(3)`、`prescaler=[5]`、`priority=[6]` 切分；不足 7 字节返回 `nullopt`，**不做字段默认值补全**。

### 3.4 Executor

- `include/libxcp/command_executor.hpp:515`：`[[nodiscard]] std::optional<GetDaqListModeResponse> ExecuteGetDaqListMode(std::uint16_t daq_list);`
- `src/command_executor.cpp:1520`：仅捕获“明确 `ERR_CMD_UNKNOWN`”转 `nullopt`；其余负响应照常抛 `XcpException`；响应长度不足抛 `MalformedPacket` 并带实际字节数与期望值（与 `ExecuteGetDaqListInfo` 的处置一致）。

### 3.5 Master

- `include/libxcp/xcp_master.hpp:116`：`struct DaqListModeSnapshot { std::uint16_t daq_list; std::uint8_t mode; std::uint16_t event_channel; std::uint8_t prescaler; std::uint8_t priority; };`
- `include/libxcp/xcp_master.hpp:469`：`[[nodiscard]] std::optional<DaqListModeSnapshot> QueryDaqListMode(std::uint16_t daq_list);`
- `src/xcp_master.cpp:1294`：`daq_list` 由入参回填（Slave 响应里没有列表号）；`nullopt` 直接透传。

**为什么不读本地账本补齐**：账本记的是“Master 想配置成什么”，本接口回答的是“Slave 现在是什么”。两者在 Slave 侧被外部改写（例如另一主机、或 Slave 自行 `RESUME`）时会分叉，用账本补空会把观测值伪装成事实。注释已写明这一点。

## 4. 测试（B0-3）

新增 `tests/get_daq_list_mode_test.cpp`，注册于 `tests/CMakeLists.txt:82`，共 10 个用例：

| 用例 | 断言要点 |
| --- | --- |
| `GetDaqListModeCodec.IssuesReservedByteThenDaqWord` | Intel `{DF 00 07 00}`、Motorola `{DF 00 00 07}` |
| `GetDaqListModeCodec.KeepsDaqListNumberRange` | `0x1234` → `{DF 00 34 12}` |
| `GetDaqListModeParser.SplitsStatusEventPrescalerPriority` | 七字段切分；`RUNNING`/`SELECTED` 判定；`RESUME` 不误判 |
| `GetDaqListModeParser.ReadsWordInMotorolaOrder` | Motorola 端序读 `event_channel` |
| `GetDaqListModeParser.RejectsTruncatedResponse` | 6 字节 → `nullopt`；空 → `nullopt` |
| `GetDaqListModeParser.StatusBitsAreNotSetModeBits` | `kSelected==kAlternating==0x01` 但类型分离；`kRunning=0x40`、`kResume=0x80` |
| `XcpMasterGetDaqListMode.ReturnsSlaveStateSnapshot` | 快照回填入参 `daq_list`；确认只发一次查询 |
| `XcpMasterGetDaqListMode.ReturnsNulloptOnCmdUnknown` | `ERR_CMD_UNKNOWN` → `nullopt`，且仍实际发出查询 |
| `XcpMasterGetDaqListMode.EmptyEventChannelIsPreserved` | `event_channel=0` 视为**有效观测**，不当作缺失 |

模拟 Slave 采用最小实现：只回答 `CONNECT`、`GET_STATUS`、`GET_DAQ_LIST_MODE`，其余一律 `ERR_CMD_UNKNOWN`，避免测试替身掩盖协议路径。

### 4.1 实测结果

- 新增用例：`--gtest_filter='*DaqListMode*:*DaqListStatusBit*'` → **10/10 通过**
- 全量回归：`libxcp_tests.exe` → **836 个用例，835 通过，1 跳过（`AgIntegration/XcpMasterAgTest.ByteApiRequiresMultipleOfAg/Ag1_Cto8`，本批之前即为 skipped），0 失败**，耗时 45.9 s
- 构建：`cmake --build cmake-build-release -j 4 --target libxcp_tests` → `EXIT=0`

## 5. 构建环境记录（本次实际踩坑，供后续批次复用）

本机构建有三个与代码无关、但会导致“看起来卡死”的环境问题：

1. **CMake 在受限沙箱下 SIGSEGV**。`cmake -S . -B <dir>` 在文件系统受限模式下于编译器检测阶段直接段错误（`exit 139`，无任何输出），表现为 `cmake --build` 长时间零 CPU 挂起。放开为完全访问后同一命令正常完成并给出可诊断错误。后续构建需在完全访问模式下运行。
2. **必须处于 MSVC 开发者环境**。仅把 `cl.exe` 绝对路径交给 CMake 不够：`INCLUDE`/`LIB` 未初始化时，编译报 `fatal error C1083: 无法打开包括文件: "cstdint"`。需先执行 `VC\Auxiliary\Build\vcvars64.bat`。
3. **`LIBXCP_BUILD_TESTS` 在缓存中曾为 `OFF`**。若 `ninja: error: unknown target 'libxcp_tests'`，先确认 `-DLIBXCP_BUILD_TESTS=ON` 重新配置，再构建。

另外，`tests/CMakeLists.txt` 的改动会触发一次 CMake 重配置（`verify_globs` 每次构建都会重跑），因此“新增测试文件”必然伴随一次配置；配置失败时不要让 ninja 反复重试。

## 6. 边界与遗留

- `QueryDaqListMode` 只报告 stdout 事实，**不代表** Master 本地账本与 Slave 状态已一致；一致性判定留给使用方。
- 本批未改变 `ConfigureDaqList`/`ConfigureDaqListsDynamic` 的写入路径，`SET_DAQ_LIST_MODE` 失败回滚语义维持原状。
- 下一批 B1（标定闭环：`DOWNLOAD_NEXT` Block 下载、`SET_REQUEST/STORE_CAL_REQ`、`BUILD_CHECKSUM` 写后校验）尚未开始，需先读 `CommandExecutor` 的 MTA 推进与 `MemoryAccess` 分块实现，再落 API。