# XCP V1.5 测量与标定支持完成度检测报告

> 检测日期：2026-10-22  
> 检测对象：当前工作区 `libxcp` 生产代码及本次更新  
> 规范基线：`docs/Asam-Ae-Mcd-1-Xcp-Bs-Protocol-Layer-v1-5-0.pdf`，Version 1.5.0  
> 检测范围：XCP Master 的测量、DAQ/STIM、标定、Page Switching 及直接依赖的标准命令、版本裁决、DTO 数据面  
> 排除范围：按用户要求，不评价 `examples/`、`tools/`、`tests/` 下的修改；Programming、DBG 仅在影响能力声明时说明  
> 检测方式：官方 PDF 逐项对照、生产源码静态审查、当前差异审查、已有 Release 测试产物辅助验证  
> 修改声明：本报告生成前的检测过程未修改生产代码

---

## 1. 执行摘要

**总体判定：部分支持 XCP V1.5。**

当前实现已经形成质量较高的常用测量与标定主路径：

- Polling 测量：`SET_MTA + UPLOAD/SHORT_UPLOAD`；
- 基础、静态、动态 DAQ 配置及 DAQ DTO 接收解码；
- 标定内存写入、短地址写入、原子位修改；
- 完整的八条 Page Switching 命令链路；
- Seed & Key、版本探测、错误恢复及 UDP 数据帧处理。

但是，下列 V1.5 能力尚未形成完整链路：

- Packed DAQ；
- DAQ 时钟关联与 Time Correlation；
- STIM DTO 端到端发送；
- DTO Counter Properties；
- Block Download；
- 标定数据持久化请求；
- ECU 侧 Checksum。

因此，准确的产品声明应为：

> **支持 XCP V1.5 常用测量/标定 Profile；不构成完整 XCP V1.5 Protocol Layer 实现，也未完成通用第三方对端互操作认证。**

### 1.1 量化结果

| 功能组 | 规范命令数 | 已形成完整执行链路 | 命令覆盖率 | 判定 |
|---|---:|---:|---:|---|
| DAQ/STIM Mandatory CTO | 10 | 10 | 100% | DAQ 配置控制面完整；不代表 STIM 数据面完整 |
| DAQ/STIM Optional CTO | 11 | 5 | 45.5% | V1.5 高级能力缺口明显 |
| DAQ/STIM CTO 合计 | 21 | 15 | 71.4% | 常规 DAQ 可用，完整 V1.5 不成立 |
| Calibration 命令 | 5 | 3 | 60.0% | 缺少 Block Download 两命令 |
| Page Switching 命令 | 8 | 8 | 100% | 命令链路完整 |
| CAL/PAG 专用命令合计 | 13 | 11 | 84.6% | 标定主路径成熟 |

> 说明：命令覆盖率只衡量 `Codec + Executor + Parser（如需）+ 顶层可调用路径`，不等于完整功能符合率。Optional 命令未实现不直接构成协议违规，但必须作为能力边界公开。

---

## 2. 检测口径

### 2.1 规范证据

官方 PDF 的 Table 24～29（PDF 正文页 106～107）定义：

- Standard commands；
- Calibration commands；
- Page switching commands；
- Basic DAQ/STIM commands；
- Static DAQ/STIM commands；
- Dynamic DAQ/STIM commands。

规范同时规定：

1. 未实现的 Optional 命令应返回 `ERR_CMD_UNKNOWN` 且不得产生副作用；
2. 资源相关 Mandatory 命令在 Slave 不声明对应资源时视为 Optional；
3. 若实现 `GET_SEED`，则必须实现 `UNLOCK`；
4. 若实现 `SET_CAL_PAGE`，则必须实现 `GET_CAL_PAGE`。

### 2.2 “已支持”的判定条件

本报告不以命令枚举存在作为“已实现”证据。只有以下链路可达才记为支持：

1. 请求编码；
2. CommandExecutor 执行；
3. 响应解析或无数据正响应确认；
4. XcpMaster/业务编排层可达；
5. 对依赖隐含状态的命令具备必要的状态建立与恢复逻辑。

例如 `BUILD_CHECKSUM`、`DOWNLOAD_NEXT`、`DOWNLOAD_MAX` 虽然存在 `CommandCode` 枚举，但没有完整执行链路，故判定为未实现。

---

## 3. 本次更新项评价

本次更新的主要价值是修正协议语义、版本裁决、DTO 数据面和恢复能力，而不是新增大量 CAL/PAG 命令。

| 更新项 | 生产代码证据 | 评价 |
|---|---|---|
| `GET_VERSION` 与 PL/TL 双版本证据 | `include/libxcp/protocol_version.hpp`、`include/libxcp/version_resolver.hpp`、`src/protocol_version.cpp`、`src/version_resolver.cpp`、`src/xcp_master.cpp:506-697` | 能正确区分运行时版本、配置与 A2L 证据；避免仅凭 CONNECT 主版本猜测 V1.5 |
| V1.5 Level-1 能力边界 | `include/libxcp/protocol_types.hpp:116-122`、`include/libxcp/xcp_master.hpp:629-654` | 正确支持 `GET_VERSION`，并显式阻断未实现的 Packed DAQ 子命令 |
| DTO Identification/Counter/Timestamp | `include/libxcp/daq/dto_envelope_types.hpp`、`src/daq/dto_envelope_decoder.cpp` | 明显增强 DAQ 数据面正确性；支持标准识别字段、Counter、时间戳和 PID-off 组合 |
| DAQ Entry 单位校正 | `src/measurement/measurement_planner.cpp:100-144` | 按 AG 与字节单位分别校验，修复 MAX_ODT_ENTRY_SIZE 语义混用风险 |
| Event Channel 分组 | `src/measurement/measurement_planner.cpp:186-239` | 规划器已支持多事件通道分组 |
| A2L/运行时能力交叉验证 | `adapter/a2l/measurement_runtime_profile.cpp:81-285` | 能校验 Processor、Resolution、EVENT 元数据和运行时响应 |
| UDP Packet Alignment 与多帧 | `include/libxcp/udp_header_codec.hpp`、`src/udp_header_codec.cpp`、`src/udp_transport.cpp` | 提升 Ethernet/UDP 数据帧处理与关闭唤醒的健壮性 |
| 按命令语义恢复 | `include/libxcp/command_executor.hpp`、`src/command_executor.cpp` | 区分幂等命令、分段事务和结果未知写操作，避免危险盲重放 |

### 3.1 更新后的能力边界

本次更新使实现更接近“可验证的 V1.5 子集”，但以下事实仍成立：

- 能读取 `GET_VERSION` 不等于实现全部 V1.5 Optional 能力；
- DTO 解码器支持 Counter/Timestamp，不等于实现 Packed DAQ；
- DAQ List 可以设置 STIM direction，不等于具备 STIM DTO 数据发送链路；
- 已实现 UDP 对齐不等于完成全部 Ethernet Transport Layer Optional 功能。

---

## 4. 测量与 DAQ/STIM 详细矩阵

### 4.1 Mandatory CTO

| 命令 | 状态 | 主要证据 |
|---|---|---|
| `SET_DAQ_PTR` | 已支持 | `include/libxcp/command_codec.hpp`、`src/command_executor.cpp`、`src/xcp_master.cpp` |
| `WRITE_DAQ` | 已支持 | 同上；配置过程维护 DAQ Ledger |
| `SET_DAQ_LIST_MODE` | 已支持 | `src/xcp_master.cpp` 支持 DAQ/STIM direction、timestamp 等模式位 |
| `START_STOP_DAQ_LIST` | 已支持 | 单列表 Select/Start/Stop 与 FIRST_PID 处理 |
| `START_STOP_SYNCH` | 已支持 | 全局同步启动/停止 |
| `CLEAR_DAQ_LIST` | 已支持 | 静态 DAQ 清理及本地账本同步 |
| `FREE_DAQ` | 已支持 | 动态配置事务入口 |
| `ALLOC_DAQ` | 已支持 | 动态分配 DAQ List |
| `ALLOC_ODT` | 已支持 | 动态分配 ODT |
| `ALLOC_ODT_ENTRY` | 已支持 | 动态分配 Entry |

**判定：10/10。**

### 4.2 Optional CTO

| 命令 | 状态 | 说明 |
|---|---|---|
| `READ_DAQ` | 已支持 | `include/libxcp/xcp_master.hpp:454-465` |
| `GET_DAQ_PROCESSOR_INFO` | 已支持 | `include/libxcp/xcp_master.hpp:414-422` |
| `GET_DAQ_RESOLUTION_INFO` | 已支持 | `include/libxcp/xcp_master.hpp:424-430` |
| `GET_DAQ_EVENT_INFO` | 已支持 | `include/libxcp/xcp_master.hpp:442-452` |
| `GET_DAQ_LIST_INFO` | 已支持 | `include/libxcp/xcp_master.hpp:432-440` |
| `WRITE_DAQ_MULTIPLE` | 未实现 | 无 Codec/Executor/Master API |
| `GET_DAQ_CLOCK` | 未实现 | 无命令执行及时间关联模型 |
| `GET_DAQ_LIST_MODE` | 未实现 | `CommandCode` 中亦未登记；不得误计为已支持 |
| `DTO_CTR_PROPERTIES` | 未实现 | 无配置、查询及事件通道 counter 属性工作流 |
| `SET_DAQ_PACKED_MODE` | 明确不支持 | `include/libxcp/protocol_types.hpp:116-121` |
| `GET_DAQ_PACKED_MODE` | 明确不支持 | 同上 |

**判定：5/11。**

### 4.3 Polling 测量

**状态：已支持。**

当前实现可通过 `SET_MTA + UPLOAD` 读取任意地址；短数据优先使用 `SHORT_UPLOAD`，不支持时可回退。该路径不依赖 DAQ 资源，是基础测量的稳定后备方案。

### 4.4 DAQ DTO 接收与测量会话

**状态：基础路径已支持，高级模式部分支持。**

已具备：

- DTO/RES/ERR/EV/SERV 分类；
- Absolute、RelativeByte、RelativeWord、RelativeWordAligned 标识字段解码；
- PID-off；
- DTO Counter；
- 1/2/4 字节 Timestamp；
- Motorola/Intel 多字节解释；
- UDP 一个 Datagram 内多 XCP Frame 拆分；
- 有界队列、工作线程、Counter 连续性和解码错误上报；
- 根据 DAQ Ledger 进行 payload 解码和 A2L 物理值换算。

限制：

- 自动运行时 Profile 主要覆盖保守的 Absolute PID 与经验证的 XCPlite RelativeWordAligned；
- 其他合法组合虽有底层解码能力，但尚未全部形成自动协商、配置和独立对端验证闭环。

### 4.5 STIM

**状态：部分实现，不具备端到端能力。**

已具备：

- DAQ List 的 STIM direction 配置位；
- STIM granularity/max-size 等响应字段；
- STIM、BIT_STIM、EV_STIM_TIMEOUT 等协议类型定义。

缺少：

- 面向业务层的 STIM DTO 构造与发送 API；
- STIM DTO Counter 检查/复制策略；
- STIM Timestamp 发送；
- Bitwise Stimulation 数据面；
- Bypass consistency 工作流；
- `EV_STIM_TIMEOUT` 完整业务处理。

因此不能将当前实现声明为“支持 STIM”。

### 4.6 时间关联

**状态：未实现。**

缺少：

- `GET_DAQ_CLOCK`；
- `TIME_CORRELATION_PROPERTIES`；
- `GET_DAQ_CLOCK_MULTICAST`；
- `EV_TIME_SYNC`；
- Clock Info、Grandmaster、Cluster 等模型与换算流程。

当前 DTO Timestamp 只能作为 ECU 本地时间值解析，不能建立完整的 Host/ECU 时钟关联。

### 4.7 A2L Event 绑定

`A2lMeasurementDatabase::Find()` 当前默认：

```cpp
info.event_channel = 0;
```

证据：`adapter/a2l/a2l_measurement_database.cpp:143-149`。

本次新增 `EventBoundMeasurementDatabase`，可由调用方传入 `MeasurementEventBindingCandidates`，对缺失或歧义绑定进行拒绝，并把唯一候选注入 `MeasurementSymbolInfo`，证据见 `adapter/a2l/measurement_runtime_profile.cpp:23-55`。

判定：

- 规划器已支持多 Event Channel；
- 绑定校验机制已存在；
- 但 A2L Measurement 到 Event Channel 的候选关系仍需上层显式提供，尚非完全自动化。

---

## 5. 标定详细矩阵

### 5.1 Calibration 命令

| 命令 | 状态 | 说明 |
|---|---|---|
| `DOWNLOAD` | 已支持 | 普通标定写入主路径 |
| `SHORT_DOWNLOAD` | 已支持 | 短地址写入；不支持时可由高层回退到普通 DOWNLOAD |
| `MODIFY_BITS` | 已支持 | `SET_MTA + MODIFY_BITS`，并限制非幂等超时重放 |
| `DOWNLOAD_NEXT` | 未实现 | `CommandCode` 枚举存在，但无完整执行链路 |
| `DOWNLOAD_MAX` | 未实现 | `CommandCode` 枚举存在，但无完整执行链路 |

**判定：3/5。**

缺少的两条命令主要影响 Block Mode 与吞吐率，不阻断基础标定写入。

### 5.2 Page Switching

| 命令 | 状态 |
|---|---|
| `SET_CAL_PAGE` | 已支持 |
| `GET_CAL_PAGE` | 已支持 |
| `GET_PAG_PROCESSOR_INFO` | 已支持 |
| `GET_SEGMENT_INFO` | 已支持 |
| `GET_PAGE_INFO` | 已支持 |
| `SET_SEGMENT_MODE` | 已支持 |
| `GET_SEGMENT_MODE` | 已支持 |
| `COPY_CAL_PAGE` | 已支持 |

**判定：8/8。**

顶层 API 及语义说明见 `include/libxcp/xcp_master.hpp:467-583`；执行实现见 `src/command_executor.cpp` 对应 Calibration/Page 方法。

### 5.3 标定持久化

**状态：未完成。**

`SET_REQUEST` 在 `include/libxcp/protocol_types.hpp:70` 中只有枚举，没有 `EncodeSetRequest`、`ExecuteSetRequest` 或顶层 API。因此：

- 可以通过 `SET_SEGMENT_MODE` 标记 FREEZE；
- 但无法由本库继续发出 `STORE_CAL_REQ`，完成标准化的非易失保存请求。

这属于实际标定工作流缺口，不应只按“可选命令未实现”淡化。

### 5.4 Checksum

**状态：未实现。**

`BUILD_CHECKSUM` 在 `include/libxcp/protocol_types.hpp:76` 有枚举，但没有 Codec、Executor 或 Master API。写后只能依靠回读、应用侧比较或 ECU 专用机制验证，无法请求 ECU 侧标准 Checksum。

### 5.5 A2L 高层标定语义

当前核心层主要提供协议地址读写和 Page/Segment 操作，尚未形成基于以下 A2L 对象的完整高层标定工作流：

- `CHARACTERISTIC`；
- `AXIS_PTS`；
- `RECORD_LAYOUT`；
- 复杂对象的布局、维度、轴和物理值回写。

这不属于 Protocol Layer wire-level 违规，但会影响完整标定工具的开箱即用程度。

---

## 6. 风险与差距清单

| 编号 | 严重度 | 差距 | 影响 |
|---|---|---|---|
| G-01 | 高 | STIM 数据面未完成 | 无法完成同步数据刺激和旁路闭环 |
| G-02 | 高 | DAQ Time Correlation 未实现 | 无法将 ECU 时间戳与 Host/Grandmaster 建立标准关联 |
| G-03 | 高 | Packed DAQ 未实现 | 不能使用 V1.5 高密度采样传输能力 |
| G-04 | 中 | `SET_REQUEST/STORE_CAL_REQ` 未实现 | 标定数据标准持久化流程中断 |
| G-05 | 中 | `DOWNLOAD_NEXT/MAX` 未实现 | 大块标定吞吐受限，无法使用 Block Download |
| G-06 | 中 | `BUILD_CHECKSUM` 未实现 | 缺少 ECU 侧标准写后校验 |
| G-07 | 中 | `DTO_CTR_PROPERTIES` 未实现 | 高级 Counter/Bypass 语义无法协商 |
| G-08 | 中 | `GET_DAQ_LIST_MODE` 未实现 | 无法从 Slave 回读 DAQ List 的实际模式 |
| G-09 | 低 | `WRITE_DAQ_MULTIPLE` 未实现 | 大型 DAQ 配置命令数和耗时增加 |
| G-10 | 中 | Event Binding 需显式提供 | 多事件自动规划依赖额外上层数据准备 |
| G-11 | 中 | 缺少独立标准对端验证 | 组件正确性不等于通用 ECU/CANape 互操作已验证 |

---

## 7. 建议实施优先级

### P0：补齐产品声明与核心闭环

1. 明确发布声明为“XCP V1.5 常用测量/标定 Profile”；
2. 完成 STIM DTO 发送、Counter 与 Timeout 工作流；
3. 实现 `GET_DAQ_CLOCK` 与基础 Time Correlation；
4. 增加至少一个独立标准 Slave/ECU 的互操作验证。

### P1：补齐 V1.5 高级测量

1. `SET_DAQ_PACKED_MODE`；
2. `GET_DAQ_PACKED_MODE`；
3. `DTO_CTR_PROPERTIES`；
4. A2L Measurement 到 Event Channel 的自动候选提取与歧义裁决。

### P2：补齐标定完整性与吞吐

1. `SET_REQUEST/STORE_CAL_REQ`；
2. `BUILD_CHECKSUM`；
3. `DOWNLOAD_NEXT`；
4. `DOWNLOAD_MAX`；
5. `WRITE_DAQ_MULTIPLE`；
6. `GET_DAQ_LIST_MODE`。

---

## 8. 验证状态

### 8.1 已完成

- 对官方 XCP V1.5 Protocol Layer PDF 的命令表和相关章节进行了逐项映射；
- 审查了生产头文件、Codec、Executor、Parser、Master、MemoryAccess、DAQ/DTO、Measurement、UDP 和 A2L adapter；
- 审查了当前生产代码差异；
- 复核了容易误计的“只有枚举、没有执行链路”命令；
- 复核确认 `GET_DAQ_LIST_MODE` 未实现，DAQ Optional 正确计数为 5/11。

### 8.2 测试证据边界

已有 Release 构建产物上的相关焦点测试结果为 **262/262 通过**。但本次检测没有先清理再从当前工作树重新构建，因此：

- 该结果可作为已有实现的辅助证据；
- 不能证明当前全部未提交更新已经由新二进制覆盖；
- 自有 Mock 与 XCPlite 闭环不能替代独立标准对端互操作认证。

### 8.3 未执行

- 未修改生产代码；
- 未执行安装；
- 未执行真实 ECU/CANape 互操作；
- 未执行完整 ASAM Conformance Test；
- 未评价 `examples/`、`tools/`、`tests/` 下的修改质量。

---

## 9. 最终结论

1. **标定**：基础地址写入、短写、位修改和 Page Switching 已达到较高完成度；CAL/PAG 专用命令覆盖为 **11/13（84.6%）**。Block Download、持久化请求和 Checksum 仍缺失。
2. **测量**：Polling 与常规 DAQ 已可用；Mandatory CTO 覆盖 **10/10**，Optional CTO 覆盖 **5/11**，合计 **15/21（71.4%）**。
3. **V1.5 高级能力**：Packed DAQ、Time Correlation 和完整 STIM 均未完成，是不能声明“完整支持 XCP V1.5”的决定性原因。
4. **本次更新质量**：版本裁决、DTO 规范语义、AG 单位、UDP 对齐和恢复机制得到明显加强，属于重要的正确性与健壮性升级。
5. **推荐声明**：当前版本可声明为 **“XCP V1.5 常用测量与标定 Profile 已实现；XCPlite Profile 有测试证据；完整 V1.5 与通用第三方互操作尚未验证。”**
