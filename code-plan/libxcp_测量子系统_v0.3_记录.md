# libxcp 测量子系统 v0.3 实现记录 — GET_DAQ_EVENT_INFO

> 对应计划：`code-plan/libxcp_测量子系统代码增长计划.md` §5.2 / :532-574
> 前置：v0.2（DTO Envelope Decoder）已封板。

## 一、范围与结论

本里程碑在核心库追加 `GET_DAQ_EVENT_INFO`（0xD7）命令的完整链路：
编码 → 执行 → 解析 → XcpMaster 薄转发，并配套 L1/L2 测试。构建、全套
377 通过（含 6 个新增用例）、A2lIsolation 门禁 S1/S2 0 命中，全部绿灯。

## 二、新增 / 修改文件

**修改：**

| 路径 | 动作 |
|------|------|
| `include/libxcp/protocol_types.hpp` | `CommandCode` 追加 `GetDaqEventInfo = 0xD7`；更正 :97 原"0xD7 刻意未收录"注释 |
| `include/libxcp/response_parser.hpp` | 追加 `GetDaqEventInfoResponse{properties,max_daq_lists,name_length,time_cycle,time_unit,priority}` + `ParseGetDaqEventInfo` 声明 |
| `src/response_parser.cpp` | 追加 `kGetDaqEventInfoResMinSize = 6` + `ParseGetDaqEventInfo` 实现 |
| `include/libxcp/command_codec.hpp` | 追加 `EncodeGetDaqEventInfo(uint16_t)` 声明 |
| `src/command_codec.cpp` | 追加 `EncodeGetDaqEventInfo` 实现（`[D7][event WORD LE]`，3 字节无 reserved） |
| `include/libxcp/command_executor.hpp` | 追加 `ExecuteGetDaqEventInfo(uint16_t) → optional<...>` 声明 |
| `src/command_executor.cpp` | 追加 `ExecuteGetDaqEventInfo` 实现（对齐 `ExecuteGetDaqListInfo`） |
| `include/libxcp/xcp_master.hpp` | 追加 `QueryDaqEventInfo(uint16_t) → optional<...>` 薄转发声明 |
| `src/xcp_master.cpp` | 追加 `QueryDaqEventInfo` 薄转发实现 |
| `tests/command_codec_test.cpp` | 追加 codec golden：`GetDaqEventInfoHasNoReservedByte` |
| `tests/CMakeLists.txt` | 注册 `get_daq_event_info_test.cpp` |

**新增：**

| 路径 | 内容 |
|------|------|
| `tests/get_daq_event_info_test.cpp` | L1（解析 2）+ L2（执行器 3）+ 1 代码标准，共 6 用例 |

## 三、接口落地

```cpp
// response_parser.hpp
struct GetDaqEventInfoResponse {
    std::uint8_t properties{0};      // EVENT_PROPERTIES 原码（XCPlite: DAQ|EVENT_CONSISTENCY）
    std::uint8_t max_daq_lists{0};   // 该事件可用 DAQ List 数（XCPlite 恒 0xFF）
    std::uint8_t name_length{0};     // 事件名长度（新字段，计划未列）
    std::uint8_t time_cycle{0};      // 事件周期（缩放单位）原码
    std::uint8_t time_unit{0};       // 周期单位码 原码
    std::uint8_t priority{0};        // 优先级（0xFF=最高）
};

// command_codec.hpp — [D7][event_channel WORD LE]（3 字节，无 reserved）
Bytes EncodeGetDaqEventInfo(std::uint16_t event_channel) const;

// command_executor.hpp — 对齐 ExecuteGetDaqListInfo（Optional；CmdUnknown→nullopt）
std::optional<GetDaqEventInfoResponse> ExecuteGetDaqEventInfo(std::uint16_t event_channel);

// xcp_master.hpp — 薄转发，零策略
std::optional<GetDaqEventInfoResponse> QueryDaqEventInfo(std::uint16_t event_channel);
```

## 四、偏差说明（相对计划 §5.2）

计划里写入的六字段结构为：`event_channel, properties, max_daq_lists, time_cycle,
time_unit, priority`。XCPlite 实然（`thirdparty/XCPlite/src/xcp.h:816-825`，
CRO_GET_DAQ_EVENT_INFO_LEN=4 / CRM_LEN=7）与之有三处不符，本实现按计划 §5.2 硬核对项
"XCPlite 实然行为以 xcp.c 实现为准"（计划 :558）做了**实然优先**的调整：

1. **`event_channel` 不在响应中回显** —— 计划的响应字段含 `event_channel`，但 XCPlite 的
   6 字节 RES 数据只有 `[PROPERTIES][MAX_DAQ_LISTS][NAME_LENGTH][TIME_CYCLE]
   [TIME_UNIT][PRIORITY]`。事件通道号是调用方**入参**，无需也**不该**作为响应字段
   （否则是伪造数据）。→ 响应结构不含 `event_channel`。
2. **响应含 `name_length`** —— 计划遗漏了 `NAME_LENGTH`（事件名长度，XCP 1.3.0 中可选、
   XCPlite 恒给 `STRNLEN`）。实际 6 字节中该字段占第 3 字节。→ 响应结构追加 `name_length`。
3. **字段总数不变但集合不同** —— 计划"六字段"与 XCPlite 六字节集合不同。本实现忠实于线格式；
   `time_unit` 单位码语义（XCPlite 1ns=0,10ns=1,100ns=2,1us=3,…,1ms=6）写入 doxygen 供 v0.4
   语义层使用，本结构只保留原码（与计划一致）。

另一处对齐性说明（非偏差）：计划的 `ExecuteGetDaqEventInfo` 签名写的是非 optional
（返回 `GetDaqEventInfoResponse`），但同节的验收标准要求"Slave 无此命令 → nullopt（不抛）"，
且 `xcp_master.hpp` 的薄转发为 optional。执行器我按 **`ExecuteGetDaqListInfo` 现役口径
（返回 `std::optional`，CmdUnknown→nullopt）** 实现，与验收标准及 master 薄转发完全自洽。

## 五、验证结果

- **构建**：`cmake --build build-v02 --config Release` 成功（libxcp + libxcp_tests）。
- **新增测试**：6/6 通过（`--gtest_filter=*GetDaqEventInfo*`）：
  - `ParseGetDaqEventInfo.ParsesSixXCpliteFields`
  - `ParseGetDaqEventInfo.TooShortReturnsNullopt`
  - `ExecuteGetDaqEventInfo.IssuesD7WithEventChannelWordAndParsesResponse`（校验 CRO=[D7][0x02][0x01]）
  - `ExecuteGetDaqEventInfo.CmdUnknownReturnsNullopt`
  - `ExecuteGetDaqEventInfo.MalformedShortResponseThrows`
  - `CommandCodecGolden.GetDaqEventInfoHasNoReservedByte`（[D7][event WORD LE]，3 字节）
- **全套回归**：378 total, **377 passed / 1 skipped**（`Ag1_Cto8` 存量 skip）。
- **A2lIsolation**：`S1 扫描 21 个桥接层文件、S2 扫描 25 个主树文件，命中均为 0` ✓

## 六、关键决策

- 事件通道号**不作为响应字段**（实然不回显，避免伪造数据）。
- 保留 `name_length`，供 v0.4 planner 校验 A2L EventChannelInfo 的事件名长度一致性。
- 执行器与 master 薄转发的 Optional 口径统一为 nullopt（不抛），对齐 `ExecuteGetDaqListInfo`。

## 七、AGENTS.md 自检

- [x] 不重构既有代码：仅增量追加新方法/字段，未改动任何既有逻辑
- [x] 不动 thirdparty（XCPlite/a2l-sdk 零改动）
- [x] 新增测试在 `tests/`；已注册进 `tests/CMakeLists.txt`
- [x] 新增接口/字段全部 doxygen 中文化
- [x] 本次改动写 MD 记录
- [x] A2lIsolation 门禁维持 S1/S2 0 命中

## 八、开放事项 / 下一步

- **v0.4 MeasurementPlanner**：规划分组（按事件通道）+ DAQ List 分配 + ODT 打包；
  用本里程碑的 `QueryDaqEventInfo` 做 `time_cycle/time_unit/priority` 运行时取证。
- 事件名主体（MAIN_UPLOAD 通路）在 v0.4/后续按 MTA 读取，本里程碑只取 `name_length`。

---

*记录时间：v0.3 封板后。*