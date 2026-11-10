# B4 DAQ Packed Mode 设计方案（待确认，未实施）

> 状态：**设计草案，等待确认后编码**。计划 [§3 B4](XCP_V1.5_测量标定修复与测试实施计划.md) 明确要求"开始编码前须单独复核布局与输出接口设计并取得确认；不能仅删除 `UnsupportedCapability::DaqPackedMode` 标志"。本文件即该复核材料。
> 规范依据：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf` 原件 §4.1.10、§7.5.4.7/§7.5.4.8；[Markdown 版](../docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.md) 仅用于检索定位。

## 1. 规范决定的设计事实

1. **Packed 不改变 DAQ 配置，只改变一帧承载的采样数**。ODT 条目仍是"一条 = 一个变量"，但一帧里出现该变量的 `DPM_SAMPLE_COUNT` 个连续采样。
2. **两种排列**：
   - element-grouped（`DAQ_PACKED_MODE=1`）：同一变量的 S 个采样连续排放，变量之间再串接；
   - event-grouped（`DAQ_PACKED_MODE=2`）：同一采样时刻的所有变量先排，再排下一个时刻。
   - **不同 ODT 的数据不混排**——ODT 与 DTO 的对应关系在 packed 下依然成立。
3. **只有一帧带时间戳**（`DPM_TIMESTAMP_MODE`）：`0` = 最后一个采样的时刻，`1` = 第一个采样的时刻；其余采样时刻必须由接收方用 `EventPeriod × Prescaler` 反推。规范同时要求时间戳**严格单调递增**。
4. **前置条件**：DAQ List 必须支持 PACKED（`GET_DAQ_LIST_INFO` 的 `DAQ_LIST_PROPERTIES` bit4），事件通道必须支持 PACKED（`GET_DAQ_EVENT_INFO` 的 `DAQ_EVENT_PROPERTIES` bit4）**并且是周期事件**（`EVENT_CHANNEL_TIME_CYCLE != 0`），否则不得使用。
5. `DPM_SAMPLE_COUNT` 合法范围 `2..0xFFFF`，`0` 和 `1` 非法；组合不被接受时 Slave 回 `ERR_MODE_NOT_VALID`，列表不支持或不存在回 `ERR_OUT_OF_RANGE`。
6. `GET_DAQ_PACKED_MODE` 正响应：`mode=0` 只有 3 字节（含 PID）；`mode=1/2` 才有 `DPM_TIMESTAMP_MODE` 与 `DPM_SAMPLE_COUNT`。
7. **`CLEAR_DAQ_LIST` 会把该列表复位为 non-packed**；因此任何"清表 → 重配"的路径都必须重新设置 packed 模式，本地账本也要同步失效。
8. 采集中途丢采样时，Slave 丢弃本组已收集数据、从头重新收集；停止列表时未满一组的数据同样丢弃。**所以收到的一帧总是完整的 S 个连续采样**——这是接收方可以信任的前提。

## 2. 当前代码现状（B4 需要触碰的点）

| 位置 | 现状 | B4 影响 |
| --- | --- | --- |
| `include/libxcp/xcp_master.hpp:861` | `UnsupportedCapability::DaqPackedMode` 把 packed 列为"本库确定不支持" | 需改为按 Slave 能力判定，或保留枚举但语义改为"未协商" |
| `include/libxcp/protocol_types.hpp:523` | `DaqListPropertyBit` 只有 PREDEFINED/EVENT_FIXED/DIRECTION | 需补 `kPacked = 0x10` |
| 同上 | 没有事件属性位枚举 | 需补 `DaqEventPropertyBit{kDaq=0x04,kStim=0x08,kPacked=0x10}` |
| `include/libxcp/protocol_types.hpp:120` | 0xC0 族只登记了 `GetVersion`，注释写明 packed 子命令未提供编码路径 | 需新增子命令码与 `Level1OtherSubcommand` 语义调整 |
| `include/libxcp/daq/dto_envelope_types.hpp` | `DtoFrameLayout` 只有单采样语义 | 需携带 packed 参数（模式、S、时间戳模式、事件周期、prescaler） |
| `src/daq/dto_envelope_decoder.cpp` | 切出 `envelope.payload` 后即结束 | 不拆样本，拆样本留给上层切片阶段 |
| `include/libxcp/daq/dto_payload_decoder.hpp` | `PayloadSlice{name,offset,size}` 一变量一段 | 需按样本索引算偏移，或由上层循环 |
| `include/libxcp/measurement/measurement_planner.hpp:34` | `MeasurementRoute` 单偏移 | 需补 `packed_stride`；容量公式需乘 S |
| `include/libxcp/measurement/measurement_session.cpp` | 一 DTO → 一 `MeasurementFrame`；时间戳逐帧延展 | Packed 下一 DTO → S 帧，时间戳需重建 |
| `include/libxcp/measurement/measurement_sample.hpp:51` | `MeasurementFrame{daq_list,odt,timestamp,timestamp_raw,timestamp_valid,counter,counter_lost_before,samples}` | 需补样本序号与时间戳来源标记 |

## 3. 数据面布局（核心公式）

设某 ODT 内条目按配置顺序为 `e = 0..E-1`，条目宽度 `w(e)` 字节，ODT 单采样净宽 `W = Σ w(e)`，样本数 `S`，打包模式 `M`。

净荷总长恒为 `S × W`（两种模式一致，差别只在排列）。

条目 `e` 的第 `k` 个采样（`k = 0..S-1`）在净荷中的偏移：

```
element-grouped (M=1):  base(e) = S × Σ_{e'<e} w(e')      offset(e,k) = base(e) + k × w(e)
event-grouped   (M=2):  base(e) =     Σ_{e'<e} w(e')      offset(e,k) = base(e) + k × W
```

两式可统一成 `offset(e,k) = base(e) + k × stride(e)`，其中

```
M=1: stride(e) = w(e)      M=2: stride(e) = W
```

**这正是 `MeasurementRoute` 只需增加一个 `packed_stride` 字段**就能表达两种模式的原因：`base` 在规划期已算好（就是现有 `payload_offset`），解码期只多做一次乘法。

`S = 1` 与非 packed 完全兼容（`stride` 取 0 或任意，`k` 只有 0），所以非 packed 路径不需要分支。

## 4. 输出接口设计

### 方案 A（推荐）：一 DTO → S 个 `MeasurementFrame`

- 每个采样时刻产出 **一个 `MeasurementFrame`**，`samples` 仍是"该时刻全部变量的值"，与现有非 packed 语义完全一致；
- 新增字段：
  - `std::uint16_t sample_index`：该帧在本 DTO 内的采样序号 `0..S-1`；
  - `std::uint16_t sample_count`：本 DTO 的 S（1 表示非 packed）；
  - `MeasurementTimestampSource timestamp_source`：`Direct`（Slave 报的时刻）/ `Reconstructed`（按事件周期反推）/ `Unavailable`（缺事件周期或时间戳无效，只给原始值）。
- 时间戳按 §3 公式重建，并做**严格单调**后处理（见 §6）；
- 回调调用次数从"每 DTO 一次"变为"每 DTO S 次"。这是唯一对外可见的行为变化。

### 方案 B（备选）：一 DTO → 一个 `MeasurementFrame`，`samples` 变 `S × E` 矩阵

- 保持"一 DTO 一次回调"，但 `samples` 语义从"一次采样"变成"多采样矩阵"，需要给每个样本配时间戳数组。
- 缺点：调用方必须改循环结构；`timestamp` 单值字段失效；与非 packed 语义分叉最大。
- 优点：回调频率低（高频场景下 callback 开销更小）。

**取舍**：方案 A 让"帧 = 一个时刻"这条既有契约在两种模式下都成立，调用方不需要按模式分支；代价是回调次数按 S 放大。若后续出现 `S` 极大且回调开销成为瓶颈的场景，可在 A 之上加"批量回调"重载，而不必推翻 A 的语义。

## 5. 协商、配置与代际

状态机（任一环节失败都必须退回非 packed 或明确失败，**绝不能**一边配 packed 一边按普通 DTO 解码）：

```
探测能力 ──┬─ 列表/事件不支持 PACKED，或事件周期为 0 → 记 Unsupported → 保持非 packed
           └─ 支持 → SET_DAQ_PACKED_MODE(mode, ts_mode, S)
                        │
                        ├─ ERR_MODE_NOT_VALID / ERR_OUT_OF_RANGE → 记为"该组合不支持"，回退
                        └─ ACK → GET_DAQ_PACKED_MODE 回读
                                    │
                                    ├─ 回读 mode / S / ts_mode 与请求不一致 → 失败（Slave 静默降级）
                                    └─ 一致 → 置 packed 生效，重建账本与路由，代际 +1
```

约束：

- 切换模式前必须 `StopDaq` 并等待 DTO 流停止；切换后 `DaqConfigGeneration()` 自增，旧代际帧在 worker 侧直接丢弃（现有 `m_routes_` 版本管理需确认是否已按代际过滤，没有则补）。
- `CLEAR_DAQ_LIST` 之后本地 packed 记账必须失效（规范 §4.1.10.1）。
- 断连重连后必须重新协商，不得沿用连接前的 packed 结论。
- 能力探测结果来自 `GET_DAQ_LIST_INFO`（bit4）与 `GET_DAQ_EVENT_INFO`（bit4 + 周期非 0）；`GET_DAQ_EVENT_INFO` 的 `time_unit` 取**低半字节**（与时间戳单位的高半字节编码不同，规范明确提示过）。

## 6. 时间戳重建

```
ts_mode = 0（Slave 报最后一个采样）:  TimeStamp(k) = TimeStampDAQ − (S − k − 1) × EventPeriod × Prescaler
ts_mode = 1（Slave 报第一个采样）:    TimeStamp(k) = TimeStampDAQ + k × EventPeriod × Prescaler
```

- `EventPeriod` 由 `GET_DAQ_EVENT_INFO` 的 `time_cycle × time_unit` 换算成纳秒（`time_unit` 覆盖 1ns/10ns/…/100ps，必须整表支持，不能只认 µs）；
- `Prescaler` 取该 DX 列表当前生效值（来自 `SET_DAQ_LIST_MODE` 下发值并已回读确认，不能只用本地期望值）；
- **严格单调**：规范要求时间戳严格递增，但名义周期与真实事件周期必有偏差。处理策略：前一个已输出帧的 `timestamp_raw` 记为 `last`，若重建值 `<= last`，则把该帧标记为 `Reconstructed` 并取 `last + 1`（最小 1 ns 步进），同时计数 `timestamp_reorders` 供上层观测。**绝不静默回退成"无时间戳"**，也不调整 `timestamp_raw`。
- `EventPeriod` 不可得（周期为 0、无时钟单位、`GET_DAQ_EVENT_INFO` 不支持）时：`ts_mode` 为 1 时第 0 个采样仍可用原始时间戳（`Direct`），其余采样 `Unavailable`；`ts_mode` 为 0 时只有第 S−1 个可用。**不做等间隔猜测**。

## 7. 错误与降级

| 场景 | 处理 |
| --- | --- |
| `SET_DAQ_PACKED_MODE` 回 `ERR_CMD_UNKNOWN` | 结论"该 Slave 不支持命令"，回退非 packed，不报致命错 |
| 回 `ERR_MODE_NOT_VALID` | "该 (mode, ts_mode, S) 组合不被接受"，回退非 packed 并保留错误信息供调用方换参数重试 |
| 回 `ERR_OUT_OF_RANGE` | "列表不存在或不支持 packed"，回退并标记该列表能力受限 |
| 回读不一致 | 抛 `UnsupportedFeature`，**不**把本地记账改成 packed |
| 帧长 ≠ `S×W + header + ts` | 该帧解码失败计入 `decode_errors`，不产出半帧（现有策略一致） |
| 会话中途断连 | packed 记账与代际一起失效，重连后重新协商 |

## 8. 测试矩阵（新增 `tests/daq_packed_mode_test.cpp`）

| 维度 | 用例 |
| --- | --- |
| Codec | SET 帧两种端序、`mode=0` 时忽略 5..7 字节、`S=0/1` 本地拒绝、`S=2/0xFFFF` 边界、GET 帧 4 字节 |
| Parser | GET 响应 mode0 三字段（3 字节）、mode1/2 五字段（6 字节）、短帧 `nullopt`、Motorola 端序 |
| Master | 不支持→回退；`ERR_MODE_NOT_VALID`→回退并带原因；回读不一致→`UnsupportedFeature`；`CLEAR_DAQ_LIST` 后 packed 记账失效；重连后重新协商 |
| Planner | S 使容量按 `S×W` 计算，超限时开新 ODT；`w(e)` 不被 S 整除时的对齐 |
| 解码（element-grouped） | A0..A3/B0..B3 顺序断言；`offset(e,k)` 公式对每个 (e,k) 逐一比对 |
| 解码（event-grouped） | A0B0C0D0..A1B1.. 顺序断言；跨 ODT 不混排 |
| 时间戳 | `ts_mode=0/1` 各自反推；单调性修正与 `timestamp_reorders` 计数；缺事件周期降级为 `Unavailable` |
| 端到端 | 模拟 Slave 端到端：协商→配置→S 帧输出→停止；旧代际帧被丢弃 |

不能以 XCPlite 代替 V1.5 专用端验证；XCPlite 不支持 packed 时只报"模拟验证"。

## 9. 需要确认的决策

| 编号 | 决策 | 推荐 |
| --- | --- | --- |
| D1 | 输出接口：方案 A（一 DTO → S 帧）还是方案 B（一帧多采样矩阵） | **A** |
| D2 | `UnsupportedCapability::DaqPackedMode` 的处置 | 保留枚举值但从"确定不支持"改为"未协商/未启用"，由 `IsCapabilityUnsupported` 改为只在对端不支持时返回 true；同时更新既有引用点与其测试断言 |
| D3 | 单调性冲突时的策略 | 取 `last + 1` 并计数，标记 `Reconstructed`（不上调 `timestamp_raw`） |
| D4 | 是否同期提供 packed 的"批量回调"重载 | 不提供，先按 A 落地，需要时再加 |

## 10. 实施顺序（确认后）

1. 协议层：`DaqListPropertyBit::kPacked`、`DaqEventPropertyBit`、packed 子命令码、`DaqPackedMode`/`DpmTimestampMode` 枚举与校验；
2. Codec/Parser：SET/GET 编解码；
3. Executor/Master：`SetDaqPackedMode`/`QueryDaqPackedMode`，能力探测与回读闭环；
4. 数据面：`DtoFrameLayout` 扩展、`MeasurementRoute.packed_stride`、planner 容量、worker 多帧输出与时间戳重建；
5. 测试：按 §8 矩阵；
6. 记录：`code-plan/B4_Packed模式实施记录.md`。
