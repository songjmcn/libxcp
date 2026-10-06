# libxcp 测量子系统 v0.2 实施记录 —— DTO Envelope Decoder

> 依据 code-plan/libxcp_测量子系统代码增长计划.md §5.1 / § v0.2（目标、验收标准、
> 测试、记录要求），本次落地"统一 DTO Envelope Decoder"。

## 0. 范围与结论

| 项 | 内容 |
|----|------|
| 里程碑 | v0.2 —— 统一 DTO Envelope Decoder |
| 状态 | **完成**（实现 + L1 单测 + L2 A2lIsolation 门禁全部通过） |
| 分支 / 工作区 | `feat_add_xcp_slave_tests`（未提交，未触碰 thirdparty） |
| 相对计划偏差 | 1 处口径澄清（见 §4），不影响验收语义 |

## 1. 新增文件

| 路径 | 内容 |
|------|------|
| `include/libxcp/daq/dto_envelope_types.hpp` | `IdentificationFieldType` / `DtoFrameLayout` / `DtoIdentity` / `DtoEnvelope`（§5.1，A2L-free） |
| `include/libxcp/daq/dto_envelope_decoder.hpp` | `DtoEnvelopeDecoder` 声明（§5.1） |
| `src/daq/dto_envelope_decoder.cpp` | 四种识别模式解析 + counter / timestamp 尾段切分 + 严格越界校验 |
| `tests/dto_envelope_decoder_test.cpp` | L1 单测（14 用例） |

## 2. 修改文件（增量，不重构）

- `CMakeLists.txt`：`target_sources(libxcp PRIVATE ...)` 追加
  `src/daq/dto_envelope_decoder.cpp`。
- `tests/CMakeLists.txt`：`libxcp_tests` 源列表追加
  `dto_envelope_decoder_test.cpp`。

## 3. 接口与核心规则落地情况

### 3.1 `DtoFrameLayout`（`dto_envelope_types.hpp`）

```cpp
enum class IdentificationFieldType : std::uint8_t {
    Absolute, RelativeByte, RelativeWord, RelativeWordAligned,
};
struct DtoFrameLayout {
    IdentificationFieldType identification_field_type = Absolute;
    std::uint8_t first_odt = 0;      // Absolute 模式换算绝对 ODT 用
    bool counter_enabled = false;    // DaqListModeBit::kDtoCounter
    bool timestamp_enabled = false;  // DaqListModeBit::kTimestamp
    std::uint8_t timestamp_size_bits = 0;  // 位宽（比特），来历：分辨率取证
    bool overflow_indicator = false;
    bool pid_off = false;            // 无识别字段，解码侧必拒（B-7）
    std::size_t header_bytes = 1;    // 识别字段头长
};
struct DtoIdentity { std::uint16_t daq_list = 0; std::uint8_t odt = 0; };
struct DtoEnvelope {
    DtoIdentity identity;
    std::optional<std::uint8_t> counter;
    std::optional<std::uint64_t> raw_timestamp;
    BytesView payload;
};
```

### 3.2 `DtoEnvelopeDecoder::Decode(BytesView dto, const DtoFrameLayout&) const`

纯函数内核：布局由调用方按会话取证构造，解码器不取证、不触网、不访问 XcpMaster。

- **Absolute**：byte0 = 绝对 ODT 号；`identity.daq_list=0`（Absolute 模式下由调用方在
  路由阶段按"哪条 DAQ List 在跑"补充）；`header_bytes=1`。
- **RelativeByte（XCPlite 实然）**：`[relODT][0xAA][DAQ16 LE]`，`header_bytes=4`；
  校验 `dto[1]==0xAA`（缺则 MalformedPacket）；`identity.odt = first_odt + relODT`，
  `identity.daq_list = LE16(dto[2..3])`。
- **RelativeWord / RelativeWordAligned**：WORD 识别，低字节 DAQ 号、高字节相对 ODT
  （Intel 文档口径）；`header_bytes=2`（Aligned 为 4，payload 起点天然对 4）。
- **counter_enabled**：识别字段后紧跟 1 字节计数器。
- **timestamp_enabled**：先做位宽→整字节切分合法性（`bits%8==0 && bits/8∈(0,8]`，
  否则 `UnsupportedFeature`），再小端读 `bits/8` 字节为 `raw_timestamp`。
- **pid_off**：直接抛 `MalformedPacket`（B-7）。
- 任何越界/不足 → `MalformedPacket`；不允许部分解释。

错误工厂沿用 `detail::MakeMalformedPacket` / `detail::MakeUnsupportedFeature`
（对齐仓库统一异常分类）。

## 4. 相对计划的偏差 / 口径澄清（唯一一项）

> 计划原文（§v0.2 核心规则）："首 ODT 时间戳字节计入 `header_bytes`"。

**落地口径**：`DtoFrameLayout::header_bytes` 在本实现中表示**识别字段头长**（不含
counter / timestamp），counter 与 timestamp 作为识别字段之后的**独立尾段**，由其
使能位 + 位宽派生、动态累加，最终 `payload = subspan(header_bytes + counter + ts)`。

两者得到的**字节布局一致**：XCPlite 首 ODT 帧 `[relODT][0xAA][DAQ16 LE]`（4 字节）
+ 4B ts，本实现由 `header_bytes=4` + `timestamp_enabled + timestamp_size_bits=32`
复原，payload 起点 = 4+4 = 8，与既有 `SplitEnvelope`（`offs=8 when odt_rel==0`）
等价。选择"头长 = 识别字段宽、尾段按标志累加"是为让四种模式共用一条尾段切分逻辑、
并避免 counter 场景下"时间戳含在 header_bytes"造成的双口径。验收语义不变。

## 5. 验证结果

### L1 单元测试（`dto_envelope_decoder_test.cpp`，14 用例全过）

```
DtoEnvelopeDecoder                              14/14 pass
```

覆盖：Absolute 解析、RelativeByte XCPlite 头 + 缺 0xAA 抛错、RelativeWord /
RelativeWordAligned、counter、timestamp 32/64 LE、counter+timestamp 组合、时间戳
非整字节（20 bits → UnsupportedFeature）、pid_off 拒绝、帧过短、counter 越界、
空 payload 合法。

### L2 A2lIsolation 门禁（S1/S2 命中 0）

```
T10 隔离自动化通过：S1 扫描 21 个桥接层文件、S2 扫描 25 个主树文件，命中均为 0
```

新增核心文件 `include/libxcp/daq/*` + `src/daq/dto_envelope_decoder.cpp` 均不含
`liba2l` / `a2lbridge` / `calmcar::xcp::a2l` 命中（S2 保持 0）；桥接层不受影响（S1=0）。

### 全量回归

```
372 tests from 72 test suites ran. (10903 ms total)
PASSED 371，SKIPPED 1（既有 AgIntegration 用例的预置跳过，非本次引入）
```

## 6. 关键实现决策记录

1. **`header_bytes` = 识别字段头长**：四种模式各取其宽（1 / 4 / 2 / 4），counter / ts
   作为其后的独立尾段按使能位 + 位宽累加（见 §4）。
2. **RelativeByte 强制校验 0xAA 标志**：XCPlite 实然以 `0xAA` 作为相对 ODT 与 DAQ16
   的分隔符；缺标志视为非本库已知帧 → MalformedPacket，避免错位解释。
3. **时间戳一律小端读**：`raw_timestamp` 语义为"原始时间戳"，与 `ReadLe` 对 A2L 侧
   的取数口径一致；`byte_order` 参数当前仅接口保留（WORD 识别按 Intel 文档口径），
   供后续 Motorola 会话扩展，不提前下发。
4. **纯函数式、不取证**：布局入参由调用方按 `QueryDaqProcessorInfo`（DAQ_KEY_BYTE
   identification 位）与 `DaqTimestampBytesCached()` 构造，本解码器不自查 XcpMaster，
   保持 A2L-free 且可单测。

## 7. AGENTS.md 合规自查

- ✅ 中文思考 / 中文文档 / doxygen 中文注释（新头 + 新 cpp 全部接口与成员）。
- ✅ 不重构已完成代码：仅新增 `daq/` 目录与两个头 + 一个 cpp，未改动既有签名。
- ✅ thirdparty 零修改。
- ✅ 新测试放 `tests/`；新源已登记进 `CMakeLists.txt` / `tests/CMakeLists.txt`。
- ✅ A2lIsolation 门禁保持 S1=0 / S2=0。
- ✅ 路径均为相对路径；Release 构建 + VS17 2022 生成器，序列化构建（无 `-j`）。
- ✅ 本地 googletest 离线源落入 `.deps-cache/googletest`（网络 schannel 受阻，
  `git -c http.sslBackend=openssl` 一次性克隆；构建经
  `-DLIBXCP_GOOGLETEST_LOCAL=<本地源>` 使用，避免在线 FetchContent）。

## 8. 开放事项（沿用计划风险表，本步不闭环）

- **D1** 适配器物理落点（`adapter/a2l/` vs 改 thirdparty）：v0.5 前须用户确认。
- **D3** 真 ECU 识别字段类型 / 时间戳宽度：待 v0.9 L4 取证，本实现不硬编码，
  全部走运行时取证入参。

## 9. 下一步（v0.3）

- `GET_DAQ_EVENT_INFO`（0xD7）：`CommandCode` 登记枚举 + `response_parser` PR 解析 +
  `command_executor` `ExecuteGetDaqEventInfo` + `xcp_master.hpp/.cpp`
  `GetDaqEventInfo(uint16_t) → optional<GetDaqEventInfoResponse>`（薄转发；
  ERR_CMD_UNKNOWN → nullopt）；新增 `tests/` L1 用例；写
  `code-plan/libxcp_测量子系统_v0.3_记录.md`。