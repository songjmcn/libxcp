# DAQ 解码（daq）

> 归属：`include/libxcp/daq/`、`src/daq/`（v1.0 冻结面之一）
> 本文档描述 DTO 信封/净荷解码与 DAQ 时间戳换算，全部 **A2L-free**。

## 1. 模块定位

把 Transport 收到的原始 DTO 字节逐层剥出测量数据：

```text
原始 DTO 帧 → DtoEnvelopeDecoder（剥信封：识别字段/PID/计数器/时间戳）
            → envelope.payload（ODT 变量区）
            → DtoPayloadDecoder（按切片规格取原始字节）
            → IMeasurementDatabase::ToPhysical（物理换算，上层完成）
```

信封布局只来自会话参数（COMM_MODE_BASIC、GET_DAQ_PROCESSOR_INFO 的
DAQ_KEY_BYTE、GET_DAQ_RESOLUTION_INFO 的时间戳宽度）与本端 DAQ 账本回填的
pid，**绝不 include 任何 A2L 头**。

## 2. 组成文件

| 文件 | 职责 |
|---|---|
| `dto_envelope_types.hpp` | 识别字段模式 `IdentificationFieldType`、运行时信封布局、信封结构 |
| `dto_envelope_decoder.hpp` / `src/daq/dto_envelope_decoder.cpp` | 按运行时布局把原始 DTO 帧切出信封 |
| `dto_payload_decoder.hpp` / `src/daq/dto_payload_decoder.cpp` | ODT 净荷的字节级切片解码 |
| `daq_timestamp.hpp` / `src/daq/daq_timestamp.cpp` | 原始计数 → 单调纳秒，含回卷延展 |

## 3. 关键类型

### `IdentificationFieldType`（DTO 前导段四种 PID/Identification 模式）

对应 GET_DAQ_PROCESSOR_INFO 响应 DAQ_KEY_BYTE 中 identification 位的运行时真值：

- `Absolute` — byte0 = 绝对 ODT 号
- `RelativeByte` — byte0 = 相对 ODT 号 + byte1 = DAQ 扩展字节
- `RelativeWord` — WORD 识别：低字节 DAQ 号，高字节相对 ODT（Intel）
- `RelativeWordAligned` — WORD 识别且 4 字节对齐

### 运行时信封布局

由**会话调用方**依据会话取证构造，解码器只按它解析、不自行取证：
`identification_field_type`、`first_odt`、`counter_enabled`、
`timestamp_enabled`、`overflow_indicator`、`pid_off`、`timestamp_size_bits`
（0 = 无/未知，绝不猜测）。

### `PayloadSlice`（净荷切片规格）

`name` + `offset`（相对净荷起点）+ `size`（实占字节数）。越界切片 → 该样本
`valid=false`（不整体丢弃帧，`decode_errors` 由上层计数）。物理换算一律经
`IMeasurementDatabase::ToPhysical` 由上层完成。

### `DaqTimestamp` 与回卷延展

- 位宽（1/2/4 字节）来自 GET_DAQ_RESOLUTION_INFO 取证（不猜）
- 时间单位（tick→ns）以 `unit_ns` 显式注入；0 = 未知 → `valid=false` 但保留
  `raw`（码表本仓库无权威来源，见 R13）
- 回卷判定：`raw` 相对 `prev_raw` 递减 → 按位宽模 2^bits 连续延展，并计数
  `timestamp_wraps`
- 每个 DAQ List（每条时间戳流）持有一个独立转换器实例

## 4. 相关测试

`tests/dto_envelope_decoder_test.cpp`、`tests/daq_timestamp_test.cpp`、
`tests/xcp_daq_test.cpp`、`tests/get_daq_event_info_test.cpp`
