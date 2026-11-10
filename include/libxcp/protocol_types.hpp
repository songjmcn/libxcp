/**
 * @file protocol_types.hpp
 * @brief XCP 协议常量、强类型别名与基础枚举定义。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 3 节实现。
 * 本文件为叶子模块，不依赖项目内其他头文件（除标准库）。
 */

#ifndef CALMCAR_XCP_PROTOCOL_TYPES_HPP_
#define CALMCAR_XCP_PROTOCOL_TYPES_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace calmcar::xcp {

/// @brief XCP 字节缓冲区类型
using Bytes = std::vector<std::uint8_t>;

/// @brief 只读字节视图
using BytesView = std::span<const std::uint8_t>;

/// @brief 可写字节视图
using MutableBytesView = std::span<std::uint8_t>;

/// @brief XCP 32 位地址（Address 部分）
using Address = std::uint32_t;

/// @brief XCP 8 位地址扩展
using AddressExtension = std::uint8_t;

/// @brief 以 AG（地址粒度）为单位的元素计数
using ElementCount = std::uint32_t;

/// @brief 以字节为单位的计数
using ByteCount = std::uint32_t;

/// @brief 16 位 CTR（XCP on Ethernet Transport 计数器，按 Frame 递增）
using DatagramCtr = std::uint16_t;

/// @brief 16 位 LEN（XCP on Ethernet Transport 长度字段）
using DatagramLen = std::uint16_t;

/**
 * @brief XCP 命令码（Master -> Slave，范围 0xC0..0xFF）
 *
 * 本阶段使用 CONNECT/DISCONNECT/GET_STATUS/SYNCH/GET_COMM_MODE_INFO/
 * SET_MTA/UPLOAD/SHORT_UPLOAD；批次14 追加 DAQ 命令组与写回所需命令码。
 * 纪律：**只登记已实现（Encode/Parse/Execute 齐备）的码**，未实现的命令
 * 不进入枚举，避免死枚举误导调用方（批次14 T14-01）。
 * 类别与 mandatory/optional 出处：docs/XCP_1.3.0_document.md §7.4
 * L1662-1686（SET_DAQ_PTR/WRITE_DAQ/SET_DAQ_LIST_MODE/START_STOP_DAQ_LIST/
 * START_STOP_SYNCH/CLEAR_DAQ_LIST 为 Mandatory；GET_DAQ_LIST_INFO/
 * GET_DAQ_RESOLUTION_INFO/READ_DAQ/DOWNLOAD_NEXT/DOWNLOAD_MAX/
 * SHORT_DOWNLOAD 为 Optional）。
 */
enum class CommandCode : std::uint8_t {
    Connect = 0xFF,            ///< 建立逻辑 XCP 会话
    Disconnect = 0xFE,         ///< 断开逻辑 XCP 会话
    GetStatus = 0xFD,          ///< 查询当前会话状态
    Synch = 0xFC,              ///< 同步并恢复隐含状态
    GetCommModeInfo = 0xFB,    ///< 读取扩展通信模式信息
    GetId = 0xFA,              ///< 获取 Slave 标识或 A2L 信息
    SetRequest = 0xF9,         ///< 请求保存/清除非易失数据
    GetSeed = 0xF8,            ///< 读取解锁 Seed
    Unlock = 0xF7,             ///< 发送 Key 解锁资源
    SetMta = 0xF6,             ///< 设置 Memory Transfer Address
    Upload = 0xF5,             ///< 从当前 MTA 上传内存
    ShortUpload = 0xF4,        ///< 带地址的一次性上传
    BuildChecksum = 0xF3,      ///< 计算内存校验和
    TransportLayerCmd = 0xF2,  ///< 传输层专用命令
    UserCmd = 0xF1,            ///< 用户自定义命令
    Download = 0xF0,           ///< 下载到当前 MTA
    DownloadNext = 0xEF,       ///< Block Mode 续传下载
    DownloadMax = 0xEE,        ///< 最大长度下载
    ShortDownload = 0xED,      ///< 带地址的一次性下载
    // ---- 变量标定批次：MODIFY_BITS（0xEC，位修改）与 Page Switching 命令组
    //      （0xEB..0xE4；docs §7.5.2.5 / §7.5.3 系列，均为 Optional）。
    //      CRO/CRM 布局对照 thirdparty/XCPlite/src/xcp.h:604-660。----
    ModifyBits = 0xEC,          ///< 位修改（对当前 MTA 的 32bit 做 AND/XOR Mask）
    SetCalPage = 0xEB,          ///< 设置标定页（ECU/XCP/ALL 访问者）
    GetCalPage = 0xEA,          ///< 查询当前激活标定页
    GetPagProcessorInfo = 0xE9, ///< 读 Paging 处理器能力（MAX_SEGMENT/PAG_PROPERTIES）
    GetSegmentInfo = 0xE8,      ///< 读 Segment 信息（Mode 0/1/2 变长响应）
    GetPageInfo = 0xE7,         ///< 读 Page 属性（PAGE_PROPERTIES/INIT_SEGMENT）
    SetSegmentMode = 0xE6,      ///< 设置 Segment 模式（FREEZE）
    GetSegmentMode = 0xE5,      ///< 读 Segment 模式（FREEZE 状态）
    CopyCalPage = 0xE4,         ///< 复制标定页（源 Segment/Page → 目标）
    // ---- 批次14：DAQ 命令组（0xE3..0xD8；docs §7.5.4 系列） ----
    ClearDaqList = 0xE3,      ///< 清除 DAQ List 的全部 ODT Entry
    SetDaqPtr = 0xE2,         ///< 设置 DAQ 指针（后续 WRITE/READ_DAQ 用）
    WriteDaq = 0xE1,          ///< 向 DAQ 指针处写一个 ODT Entry
    SetDaqListMode = 0xE0,    ///< 设置 DAQ List 工作模式
    GetDaqListMode = 0xDF,    ///< 回读 DAQ List 当前模式（可选命令）
    StartStopDaqList = 0xDE,  ///< 启停/选择一个 DAQ List（响应含 FIRST_PID）
    StartStopSynch = 0xDD,    ///< 同步启停全部或选中的 DAQ List
    // ---- 批次 B3：基础时间关联（docs §7.5.4.10 / §7.5.6.1；均 Optional）----
    GetDaqClock = 0xDC,       ///< 读取与 DAQ 时间戳关联的时钟（legacy/extended）
    ReadDaq = 0xDB,           ///< 从 DAQ 指针处读一个 ODT Entry
    GetDaqResolutionInfo = 0xD9,  ///< 读 ODT Entry 粒度与时间戳信息
    GetDaqProcessorInfo = 0xDA,   ///< 读 DAQ 处理器能力（识别字段/扩展模式）
    GetDaqListInfo = 0xD8,        ///< 读 DAQ List 容量与固定事件信息
    // ---- v0.3 追加：GET_DAQ_EVENT_INFO（0xD7），单个事件通道的运行时信息；
    //      原先 :97 记的"0xD7 刻意未收录"在此更正为已收录（测量子系统 v0.3）。----
    GetDaqEventInfo = 0xD7,  ///< 读事件通道信息（响应六字段，见 response_parser）
    // ---- 批次20：动态 DAQ 分配命令组（docs §7.5.4.6-§7.5.4.9；
    //      命令码与 CRO 布局对照 thirdparty/XCPlite/src/xcp.h:87-90、
    //      :831-851；FREE_DAQ 是 0xD6）----
    FreeDaq = 0xD6,        ///< 释放 Slave 侧全部 DAQ 资源（FREE_DAQ）
    AllocDaq = 0xD5,       ///< 一次性分配 n 个 DAQ List（ALLOC_DAQ）
    AllocOdt = 0xD4,       ///< 为指定 List 追加 ODT（ALLOC_ODT）
    AllocOdtEntry = 0xD3,  ///< 为指定 ODT 追加 Entry 槽（ALLOC_ODT_ENTRY）
    // ---- 批次 B1：Level-1 命令族（原始 PDF 第 106 页 §7.4 / 第 114 页 Table 44）
    //      Level 1 由**双字节命令码**定义、以 0xC0 起始。本枚举按现有纪律
    //      「只登记已实现的码」收录；0xC0 族其余子命令
    //      （0xC0,0x01 SET_DAQ_PACKED_MODE、0xC0,0x02 GET_DAQ_PACKED_MODE）
    //      尚未实现，故不登记（方案 §4.3 将其归为 Block，返回
    //      UnsupportedFeature，不做内容探测式尝试）。----
    GetVersion = 0xC0,  ///< Level-1 命令族前缀；实际报文为 [0xC0][子命令 0x00]
    // ---- 批次 B2：DAQ 控制面命令（docs §7.5.4.6 / §7.5.4.15；均 Optional）----
    WriteDaqMultiple = 0xC7,   ///< 一次写入同一 ODT 内连续多个 Entry
    TimeCorrelationProperties = 0xC6,  ///< 时间关联属性查询/受控设置
    DtoCtrProperties = 0xC5,   ///< 查询/修改事件通道的 DTO 计数器属性
};

/**
 * @brief Slave -> Master 的 Packet Identifier（PID）分类
 *
 * XCP 规定 Slave 侧 PID 空间：0x00..0xFB 为 DAQ DTO，0xFC SERV，0xFD EV，
 * 0xFE ERR，0xFF RES。本阶段 DTO 仅识别、不解析内容。
 */
enum class PacketType : std::uint8_t {
    Res = 0xFF,   ///< Positive Response
    Err = 0xFE,   ///< Negative Response / Error
    Ev = 0xFD,    ///< Event
    Serv = 0xFC,  ///< Service Request
};

/**
 * @brief 从原始首字节判断 Packet 类型
 * @param first_byte 收到的 XCP Packet 首字节
 * @return PacketType 枚举值；若落在 DAQ DTO 范围（0x00..0xFB）则返回
 * std::nullopt
 * @note 返回 nullopt 表示该包是 DAQ DTO，调用方应按 DTO 路径处理。
 */
[[nodiscard]] std::optional<PacketType> ClassifyPacket(
    std::uint8_t first_byte) noexcept;

// ---------------------------------------------------------------------------
// 批次14：DAQ 位域常量（T14-01）
//
// 取值来源纪律（用户裁决）：字段**语义与顺序**取本仓库
// docs/XCP_1.3.0_document.md；该文档**没有任何 Position 字节表**，位值
// 统一交叉参考只读的 thirdparty/XCPlite/src/xcp.h（Vector 官方实现，
// CANape 同源）。XCPlite 把 Mode 的 bit2/bit6 标注为 Not used，本库因此
// 只使用 docs L2190-2196 明确列出的 5 个标志，不臆造其它位。
// ---------------------------------------------------------------------------

/// @brief DTO 的 PID 取值上界（0x00..0xFB 属 DAQ DTO；0xFC..0xFF 见
/// PacketType）
inline constexpr std::uint8_t kDtoPidMax = 0xFB;

/// @brief WRITE_DAQ/READ_DAQ 的 BIT_OFFSET：0xFF = 无位偏（整元素）
/// @details docs L1861 与 L2170：`bit_offset = 0xFF` 表示普通数据元素，
///          0..31 表示位元素。本库首版只发 0xFF（位元素 ODT 不自动展开）。
inline constexpr std::uint8_t kDaqBitOffsetNone = 0xFF;

/// @brief START_STOP_DAQ_LIST 的 Mode（docs L2212-2216）
enum class DaqListAction : std::uint8_t {
    Stop = 0x00,    ///< 停止该 DAQ List
    Start = 0x01,   ///< 立即启动该 DAQ List
    Select = 0x02,  ///< 标记为 SELECTED，交由 START_STOP_SYNCH 同步启停
};

/// @brief START_STOP_SYNCH 的 Mode（docs L2236-2240）
enum class DaqSynchAction : std::uint8_t {
    StopAll = 0x00,        ///< 停止全部 DAQ List
    StartSelected = 0x01,  ///< 启动全部 SELECTED 的 DAQ List
    StopSelected = 0x02,   ///< 停止全部 SELECTED 的 DAQ List
};

/**
 * @brief SET_DAQ_LIST_MODE 的 MODE 位（docs L2190-2196 的五个标志）
 * @details 位值取自 thirdparty/XCPlite/src/xcp.h:331-336。
 *          `kStim` 表示该 List 为 STIM 方向（Master→Slave）；不置位为 DAQ。
 */
enum class DaqListModeBit : std::uint8_t {
    kNone = 0x00,         ///< 无标志
    kAlternating = 0x01,  ///< bit0 ALTERNATING（仅 DAQ 方向可用）
    kStim = 0x02,         ///< bit1 DIRECTION=1 → STIM
    kDtoCounter = 0x08,   ///< bit3 DTO_CTR → DTO 携带计数器
    kTimestamp = 0x10,    ///< bit4 TIMESTAMP → DTO 携带时间戳
    kPidOff = 0x20,       ///< bit5 PID_OFF → DTO 不携带识别字段
};

/**
 * @brief GET_DAQ_LIST_MODE 响应中 Current Mode 的位（规范 Table 150）
 * @details 与 SET_DAQ_LIST_MODE 的请求 MODE **不是同一套位语义**：
 *          - bit0 在请求里是 ALTERNATING，在响应里是 SELECTED；
 *          - 响应额外携带 RUNNING(bit6) 与 RESUME(bit7) 两个状态位。
 *          因此禁止把本枚举取到的字节原样回填给 SET_DAQ_LIST_MODE。
 */
enum class DaqListStatusBit : std::uint8_t {
    kNone = 0x00,          ///< 无标志
    kSelected = 0x01,      ///< bit0 SELECTED：等待 START_STOP_SYNCH 同步启动
    kStim = 0x02,          ///< bit1 DIRECTION=1 → STIM 方向
    kDtoCounter = 0x08,    ///< bit3 DTO_CTR → DTO 携带计数器
    kTimestamp = 0x10,     ///< bit4 TIMESTAMP → DTO 携带时间戳
    kPidOff = 0x20,        ///< bit5 PID_OFF → DTO 不携带识别字段
    kRunning = 0x40,       ///< bit6 RUNNING：该 List 正在发送/接收
    kResume = 0x80,        ///< bit7 RESUME：停止后保留配置以便续传
};

/// @brief GET_DAQ_LIST_MODE 响应状态位命中的掩码判断
[[nodiscard]] constexpr bool HasDaqStatus(DaqListStatusBit status,
                                          DaqListStatusBit mask) noexcept;

/// @brief 状态位合并
[[nodiscard]] constexpr DaqListStatusBit operator|(DaqListStatusBit lhs,
                                                   DaqListStatusBit rhs) noexcept;

/// @brief DAQ List 模式位合并（供 Master 组装 MODE 字节）
[[nodiscard]] constexpr DaqListModeBit operator|(DaqListModeBit lhs,
                                                 DaqListModeBit rhs) noexcept;

/// @brief 模式位与掩码的可组合判断（返回是否命中掩码内任一标志）
/// @note 报文字节可用 `static_cast<DaqListModeBit>(raw)` 安全转入本枚举
///       （固定底层类型，取值覆盖整个 uint8 范围）。
[[nodiscard]] constexpr bool HasDaqMode(DaqListModeBit mode,
                                        DaqListModeBit mask) noexcept;

/**
 * @brief 一个待写入 ODT Entry 的线格式字段（WRITE_DAQ / WRITE_DAQ_MULTIPLE 共用）
 * @details 字段顺序与命令报文严格一致（docs §7.5.4.6 Table 122）：
 *          BIT_OFFSET、SIZE、Address、AddressExtension、Dummy。
 *          放在协议层而非 Master 层，是因为 Codec 需要它，而 Codec 不应
 *          反向依赖 XcpMaster 的入参结构。
 */
struct WriteDaqElement {
    std::uint8_t bit_offset{kDaqBitOffsetNone};  ///< 位偏；0xFF=无位偏
    std::uint8_t size{0};                        ///< 元素数（以 AG 为单位）
    Address address{0};                          ///< 32 位地址
    AddressExtension extension{0};               ///< 8 位地址扩展
};

/**
 * @brief DTO_CTR_PROPERTIES 请求的 MODIFIER 位（规范 Table 160/161）
 * @details 每个置位的标志表示"采用命令参数中的对应值"，否则保持 Slave
 *          当前值。未置位时对应命令参数应写 0。
 */
enum class DtoCtrModifierBit : std::uint8_t {
    kNone = 0x00,               ///< 全部保持当前值（纯查询）
    kModifyRelatedEvent = 0x01,  ///< bit0 采用命令里的相关事件通道号
    kModifyDaqMode = 0x02,       ///< bit1 采用命令里的 DAQ_MODE
    kModifyStimMode = 0x04,      ///< bit2 采用命令里的 STIM_MODE
};

/// @brief DTO_CTR_PROPERTIES 修改器位合并
[[nodiscard]] constexpr DtoCtrModifierBit operator|(
    DtoCtrModifierBit lhs, DtoCtrModifierBit rhs) noexcept;

/// @brief 修改器位命中判断
[[nodiscard]] constexpr bool HasDtoCtrModifier(DtoCtrModifierBit modifier,
                                               DtoCtrModifierBit mask) noexcept;

/**
 * @brief 事件通道的 DTO 计数器 DAQ 方向模式（规范 Table 163 的 DAQ_MODE）
 * @details 只在插入 DTO 计数器字段时生效，决定用的是相关事件自己的计数器
 *          还是相关事件的 STIM 计数器副本。
 */
enum class DaqCounterMode : std::uint8_t {
    InsertCounter = 0x00,          ///< DAQ_MODE=0 使用相关事件的计数器
    InsertStimCounterCopy = 0x01,  ///< DAQ_MODE=1 使用 STIM 计数器副本
};

/**
 * @brief DTO_CTR_PROPERTIES 响应的 PROPERTIES 位（规范 Table 165/166）
 * @details 注意位语义**不统一**：`kRelatedEventFixed`/`kDaqModeFixed`/
 *          `kStimModeFixed` 是"1 = 不可修改"，其余 `*Present` 是
 *          "1 = 存在/支持"。混用会得出反向结论，故在类型注释里显式区分。
 */
enum class DtoCtrPropertiesBit : std::uint8_t {
    kNone = 0x00,                ///< 无标志
    kRelatedEventFixed = 0x01,   ///< bit0 1=相关事件通道号不可修改
    kDaqModeFixed = 0x02,        ///< bit1 1=DAQ_MODE 不可修改
    kStimModeFixed = 0x04,       ///< bit2 1=STIM_MODE 不可修改
    kRelatedEventPresent = 0x08, ///< bit3 1=存在相关事件通道
    kDaqModePresent = 0x10,      ///< bit4 1=DAQ 方向支持 DTO 计数器
    kStimModePresent = 0x20,     ///< bit5 1=STIM 方向支持 DTO 计数器
    kStimCtrCopyPresent = 0x40,  ///< bit6 1=**不能**保存 STIM 计数器副本
    kEvtCtrPresent = 0x80,       ///< bit7 1=该事件通道带周期计数器
};

/// @brief PROPERTIES 位命中判断
[[nodiscard]] constexpr bool HasDtoCtrProperty(DtoCtrPropertiesBit properties,
                                               DtoCtrPropertiesBit mask) noexcept;

/**
 * @brief 时间关联载荷中单个时钟字段的格式（规范 Table 245/246 的 FMT_*）
 * @details 2 位编码，`0xFF` 不是合法值；解析时对 3（保留）必须报错，
 *          不能按"未知就当 DWORD"处理，否则会把 8 字节字段读成 4 字节，
 *          后面所有时间戳与 SYNC_STATE 全部错位。
 */
enum class ClockPayloadFormat : std::uint8_t {
    Absent = 0,  ///< 该时钟不在载荷中
    Dword = 1,   ///< 4 字节无符号
    Dlong = 2,   ///< 8 字节无符号
};

/// @brief 取 2 位编码对应的载荷格式；3（保留）返回 std::nullopt
[[nodiscard]] constexpr std::optional<ClockPayloadFormat>
DecodeClockPayloadFormat(std::uint8_t raw) noexcept;

/// @brief 该格式对应的时间戳字节数（Absent=0、Dword=4、Dlong=8）
[[nodiscard]] constexpr std::size_t
ClockPayloadFormatByteSize(ClockPayloadFormat format) noexcept;

/// @brief 触发源（规范 Table 243/244 的 TRIGGER_INITIATOR，TRIGGER_INFO bit0-2）
enum class TriggerInitiator : std::uint8_t {
    HardwareTrigger = 0,           ///< 硬件触发（如 Vector Syncline）
    TimeSyncEvent = 1,             ///< XCP 无关的时基同步事件（如 PPS）
    GetDaqClockMulticast = 2,      ///< 由 GET_DAQ_CLOCK_MULTICAST 触发
    TimeSyncBridge = 3,            ///< 经 Time Sync Bridge 的 multicast
    SyncStateChange = 4,           ///< 与 grandmaster 同步/锁频状态变化
    LeapSecond = 5,                ///< grandmaster 闰秒
    EcuResetRelease = 6,           ///< ECU 复位释放
    Reserved = 7,                  ///< 保留
};

/// @brief 取 TRIGGER_INFO 低 3 位的触发源
[[nodiscard]] constexpr TriggerInitiator
TriggerInitiatorOf(std::uint8_t trigger_info) noexcept;

/// @brief 时间戳采样时刻（规范 Table 244 的 TIME_OF_TS_SAMPLING，bit3-4）
enum class TimestampSamplingPoint : std::uint8_t {
    CommandProcessing = 0,    ///< 协议层命令处理期间采样
    HighPriorityInterrupt = 1, ///< 高优先级中断中采样（低抖动）
    PhysicalTransmission = 2,  ///< 物理发送时采样
    PhysicalReception = 3,     ///< 物理收到命令时采样
};

/// @brief 取 TRIGGER_INFO 的 bit3-4 采样时刻
[[nodiscard]] constexpr TimestampSamplingPoint
TimestampSamplingPointOf(std::uint8_t trigger_info) noexcept;

/// @brief 取 PAYLOAD_FMT 中的 XCP Slave 时钟格式（bit0-1）
[[nodiscard]] constexpr std::optional<ClockPayloadFormat>
XcpSlaveClockFormatOf(std::uint8_t payload_fmt) noexcept;

/// @brief 取 PAYLOAD_FMT 中的 grandmaster 时钟格式（bit2-3）
[[nodiscard]] constexpr std::optional<ClockPayloadFormat>
GrandmasterClockFormatOf(std::uint8_t payload_fmt) noexcept;

/// @brief 取 PAYLOAD_FMT 中的 ECU 时钟格式（bit4-5）
[[nodiscard]] constexpr std::optional<ClockPayloadFormat>
EcuClockFormatOf(std::uint8_t payload_fmt) noexcept;

/// @brief PAYLOAD_FMT bit6：是否在载荷中附加 Cluster Identifier 与 Counter
[[nodiscard]] constexpr bool
HasClusterIdentifier(std::uint8_t payload_fmt) noexcept;

/**
 * @brief SLAVE_CONFIG 中的 RESPONSE_FMT（规范 Table 213/214 的 bit0-1）
 * @details 0 = legacy；>=1 表示已启用高级时间关联（extended 格式）。
 */
enum class ResponseFormatMode : std::uint8_t {
    Legacy = 0,                 ///< 向后兼容格式（EV_TIME_SYNC 与 GET_DAQ_CLOCK 均为 legacy）
    TriggerInitiatorZeroTwoThree = 1,  ///< extended，EV_TIME_SYNC 仅对触发源 0/2/3 发送
    AllTriggers = 2,            ///< extended，对所有触发条件发送 EV_TIME_SYNC
};

/// @brief 取 SLAVE_CONFIG 的 bit0-1 响应格式
[[nodiscard]] constexpr ResponseFormatMode
ResponseFormatModeOf(std::uint8_t slave_config) noexcept;

/// @brief TIME_SYNC_BRIDGE 的**请求**语义（规范 Table 208/209 bit2-3）
enum class TimeSyncBridgeRequest : std::uint8_t {
    NoChange = 0,  ///< 不改变（写入位 0）
    Enable = 1,    ///< 启用桥接（写入位 1）
    Disable = 2,   ///< 禁用桥接（写入位 2）
};

/// @brief TIME_SYNC_BRIDGE 的**上报**语义（规范 Table 213/214 bit3-4）
enum class TimeSyncBridgeState : std::uint8_t {
    NotAvailable = 0,  ///< Slave 不提供桥接特性
    Disabled = 1,      ///< 提供但当前禁用
    Enabled = 2,       ///< 提供且已启用
};

/// @brief 取 SLAVE_CONFIG 的 bit3-4 桥接状态
[[nodiscard]] constexpr TimeSyncBridgeState
TimeSyncBridgeStateOf(std::uint8_t slave_config) noexcept;

/// @brief DAQ 时间戳关联的时钟（规范 Table 213/214 的 DAQ_TS_RELATION，bit2）
enum class DaqTimestampRelation : std::uint8_t {
    XcpSlaveClock = 0,  ///< DAQ 时间戳关联 XCP Slave 自己的时钟
    EcuClock = 1,       ///< DAQ 时间戳关联 ECU 时钟
};

/// @brief 取 SLAVE_CONFIG 的 bit2 时间戳关联时钟
[[nodiscard]] constexpr DaqTimestampRelation
DaqTimestampRelationOf(std::uint8_t slave_config) noexcept;

/**
 * @brief 组装 TIME_CORRELATION_PROPERTIES 的 SET_PROPERTIES 字节
 * @param mode 目标响应格式；`Legacy` 表示写 0（不改）
 * @param bridge 桥接请求
 * @param set_cluster_id 是否采用传入的 CLUSTER_ID（bit4）
 * @return 可直接下发的 SET_PROPERTIES 字节
 * @note 规范里 `RESPONSE_FMT=0` 与 `TIME_SYNC_BRIDGE=0` 都表示"不改变"，
 *       因此**无法用本命令把 Slave 降级回 legacy**；降级只能靠断开重连
 *       （规范要求上电/致命错误后回到 legacy）。这一点在文档里显式写明。
 */
[[nodiscard]] constexpr std::uint8_t EncodeTimeCorrelationSetProperties(
    ResponseFormatMode mode, TimeSyncBridgeRequest bridge,
    bool set_cluster_id) noexcept;

/// @brief XCP Slave 时钟的可观测性（OBSERVABLE_CLOCKS bit0-1）
enum class SlaveClockObservability : std::uint8_t {
    FreeRunning = 0,        ///< 自由运行、可随机读
    MaybeSynchronized = 1,  ///< 可能已同步/锁频到 grandmaster、可随机读
    NoSlaveClock = 2,       ///< 没有 Slave 时钟
};

/// @brief grandmaster 时钟的可观测性（OBSERVABLE_CLOCKS bit2-3）
enum class GrandmasterClockObservability : std::uint8_t {
    None = 0,            ///< 没有同步到 grandmaster 的专用时钟
    RandomReadable = 1,  ///< 有且可随机读
    EventOnly = 2,       ///< 有但不可随机读，只能靠 EV_TIME_SYNC
};

/// @brief ECU 时钟的可观测性（OBSERVABLE_CLOCKS bit4-5）
enum class EcuClockObservability : std::uint8_t {
    None = 0,          ///< 没有 ECU 时钟
    RandomReadable = 1,///< 能访问且可随机读
    EventOnly = 2,     ///< 能访问但不可随机读，只能靠 EV_TIME_SYNC
    NotReadable = 3,   ///< 只上报 ECU 时间戳，无法读取 ECU 时钟
};

/// @brief 取 OBSERVABLE_CLOCKS 的 bit0-1（XCP_SLV_CLK）
[[nodiscard]] constexpr SlaveClockObservability
SlaveClockObservabilityOf(std::uint8_t observable_clocks) noexcept;

/// @brief 取 OBSERVABLE_CLOCKS 的 bit2-3（GRANDM_CLK）
[[nodiscard]] constexpr GrandmasterClockObservability
GrandmasterClockObservabilityOf(std::uint8_t observable_clocks) noexcept;

/// @brief 取 OBSERVABLE_CLOCKS 的 bit4-5（ECU_CLK）
[[nodiscard]] constexpr EcuClockObservability
EcuClockObservabilityOf(std::uint8_t observable_clocks) noexcept;

/// @brief Slave 时钟的同步状态（SYNC_STATE bit0-2 的 SLV_CLK_SYNC_STATE）
enum class SlaveClockSyncState : std::uint8_t {
    Synchronizing = 0,  ///< 正在同步到 grandmaster
    Synchronized = 1,   ///< 已同步
    Syntonizing = 2,    ///< 正在锁频
    Syntonized = 3,     ///< 已锁频
    NotSupported = 7,   ///< 不支持同步/锁频
};

/// @brief 取 SYNC_STATE 的 bit0-2
[[nodiscard]] constexpr SlaveClockSyncState
SlaveClockSyncStateOf(std::uint8_t sync_state) noexcept;

/// @brief grandmaster 时钟的同步状态（SYNC_STATE bit3）
enum class GrandmasterSyncState : std::uint8_t {
    NotSynchronized = 0,  ///< 尚未同步
    Synchronized = 1,     ///< 已同步
};

/// @brief 取 SYNC_STATE 的 bit3
[[nodiscard]] constexpr GrandmasterSyncState
GrandmasterSyncStateOf(std::uint8_t sync_state) noexcept;

/// @brief ECU 时钟的同步状态（SYNC_STATE bit4-5）
enum class EcuSyncState : std::uint8_t {
    NotSynchronized = 0,  ///< 未同步
    Synchronized = 1,     ///< 已同步
    Unknown = 2,          ///< 未知
};

/// @brief 取 SYNC_STATE 的 bit4-5
[[nodiscard]] constexpr EcuSyncState
EcuSyncStateOf(std::uint8_t sync_state) noexcept;

/**
 * @brief CLOCK_INFO 位：哪些时钟信息块在 MTA 指向的数据块中
 * @details 规范 Table 219/220。位值即 `TIME_CORRELATION_PROPERTIES`
 *          正响应 CLOCK_INFO 字段的编码。
 */
enum class ClockInfoBlock : std::uint8_t {
    kNone = 0x00,               ///< 无数据块
    kSlaveClock = 0x01,         ///< bit0 XCP Slave 时钟信息（18 字节）
    kGrandmasterClock = 0x02,   ///< bit1 grandmaster 时钟信息（18 字节）
    kClockRelation = 0x04,      ///< bit2 时钟关系时间元组（16 字节）
    kEcuClock = 0x08,           ///< bit3 ECU 时钟信息（18 字节）
    kEcuGrandmasterClock = 0x10,///< bit4 ECU 的 grandmaster UUID（8 字节）
};

/// @brief 时钟信息块位合并
[[nodiscard]] constexpr ClockInfoBlock operator|(ClockInfoBlock lhs,
                                                ClockInfoBlock rhs) noexcept;

/// @brief 时钟信息块命中判断
[[nodiscard]] constexpr bool HasClockInfoBlock(ClockInfoBlock blocks,
                                               ClockInfoBlock mask) noexcept;

/// @brief 单个时钟信息块的字节数（Table 221..225：18/18/16/18/8）
[[nodiscard]] constexpr std::size_t
ClockInfoBlockByteSize(ClockInfoBlock block) noexcept;

/**
 * @brief GET_DAQ_LIST_INFO 响应的 DAQ_LIST_PROPERTIES 位
 * @details 语义见 docs L2459-2463（PREDEFINED / EVENT_FIXED / DAQ-STIM）；
 *          位值取自 thirdparty/XCPlite/src/xcp.h:419-423。
 */
enum class DaqListPropertyBit : std::uint8_t {
    kNone = 0x00,           ///< 无属性
    kPredefined = 0x01,     ///< PREDEFINED：布局由 A2L 固化，禁止 WRITE_DAQ 改
    kFixedEvent = 0x02,     ///< 事件通道固定（不可重设）
    kDirectionDaq = 0x04,   ///< 方向为 DAQ
    kDirectionStim = 0x08,  ///< 方向为 STIM
    kPacked = 0x10,         ///< bit4 PACKED：该列表支持 DAQ Packed Mode（V1.5）
};

/// @brief DAQ List 属性位合并
[[nodiscard]] constexpr DaqListPropertyBit operator|(
    DaqListPropertyBit lhs, DaqListPropertyBit rhs) noexcept;

/// @brief 属性位命中判断
[[nodiscard]] constexpr bool HasDaqProperty(DaqListPropertyBit props,
                                            DaqListPropertyBit mask) noexcept;

/**
 * @brief GET_DAQ_EVENT_INFO 响应的 DAQ_EVENT_PROPERTIES 位
 * @details 原始 PDF Table 155：bit2 DAQ、bit3 STIM、bit4 PACKED。
 *          **只有 PACKED 置位且事件为周期事件（TIME_CYCLE != 0）时**，
 *          关联的 DAQ List 才允许配置为 DAQ Packed Mode。
 */
enum class DaqEventPropertyBit : std::uint8_t {
    kNone = 0x00,    ///< 无声明能力
    kDaq = 0x04,     ///< bit2：该事件可用于 DAQ 方向
    kStim = 0x08,    ///< bit3：该事件可用于 STIM 方向
    kPacked = 0x10,  ///< bit4：该事件支持关联列表使用 DAQ Packed Mode
};

/// @brief 事件属性位合并
[[nodiscard]] constexpr DaqEventPropertyBit operator|(
    DaqEventPropertyBit lhs, DaqEventPropertyBit rhs) noexcept;

/// @brief 事件属性位命中判断
[[nodiscard]] constexpr bool HasDaqEventProperty(DaqEventPropertyBit props,
                                                 DaqEventPropertyBit mask) noexcept;

/**
 * @brief DAQ Packed Mode 的打包方式（SET/GET_DAQ_PACKED_MODE 的 DAQ_PACKED_MODE）
 * @details 原始 PDF Table 123：0 = 不打包；1 = element-grouped；
 *          2 = event-grouped；3..127 预留给未来扩展；128..255 保留。
 */
enum class DaqPackedMode : std::uint8_t {
    NotPacked = 0x00,      ///< 常规模式：一个 DTO 一个采样
    ElementGrouped = 0x01, ///< 同一变量的连续采样集中排放
    EventGrouped = 0x02,   ///< 同一采样时刻的所有变量集中排放
};

/// @brief Packed 模式名称（日志/诊断用）
[[nodiscard]] std::string_view DaqPackedModeName(DaqPackedMode mode) noexcept;

/// @brief 原始字节 → DaqPackedMode；3..255 返 nullopt（预留给未来扩展/保留值）
[[nodiscard]] constexpr std::optional<DaqPackedMode> ToDaqPackedMode(
    std::uint8_t raw) noexcept {
    switch (raw) {
        case 0x00U:
            return DaqPackedMode::NotPacked;
        case 0x01U:
            return DaqPackedMode::ElementGrouped;
        case 0x02U:
            return DaqPackedMode::EventGrouped;
        default:
            return std::nullopt;
    }
}

/**
 * @brief Packed 模式时间戳语义（DPM_TIMESTAMP_MODE）
 * @details 原始 PDF Table 126：只有 bit0-1 有意义，**bit1-7 置位即保留值**。
 *          规范中一帧只带**一个**时间戳，其余采样时刻由接收方按事件周期反推。
 */
enum class DaqPackedTimestampMode : std::uint8_t {
    LastSample = 0x00,   ///< 该帧时间戳 = 本组**最后**一个采样的时刻
    FirstSample = 0x01,  ///< 该帧时间戳 = 本组**第一个**采样的时刻
};

/// @brief 原始字节 → DaqPackedTimestampMode；bit1-7 置位或取值 2/3 返 nullopt
[[nodiscard]] constexpr std::optional<DaqPackedTimestampMode>
ToDaqPackedTimestampMode(std::uint8_t raw) noexcept {
    switch (raw) {
        case 0x00U:
            return DaqPackedTimestampMode::LastSample;
        case 0x01U:
            return DaqPackedTimestampMode::FirstSample;
        default:
            return std::nullopt;
    }
}

/**
 * @brief 事件通道时间单位码 → 纳秒（B4：packed 采样间隔重建用）
 * @details 原始 PDF Table 156 `EVENT_CHANNEL_TIME_UNIT` 位于**低半字节**：
 *          0=1ns、1=10ns、2=100ns、3=1µs、4=10µs、5=100µs、6=1ms、
 *          7=10ms、8=100ms、9=1s、10=1ps、11=10ps、12=100ps。
 *          注意这与时间戳单位（高半字节编码）**不是同一张表**，不可混用。
 * @param unit_code 原始单位码（调用方可直接传 `time_unit`，本函数只取低 4 位）
 * @return 对应纳秒数；13..15 等保留值返 `nullopt`（不猜）
 */
[[nodiscard]] constexpr std::optional<std::uint64_t>
EventChannelTimeUnitNanoseconds(std::uint8_t unit_code) noexcept {
    switch (static_cast<std::uint8_t>(unit_code & 0x0FU)) {
        case 0x00U:
            return 1ULL;
        case 0x01U:
            return 10ULL;
        case 0x02U:
            return 100ULL;
        case 0x03U:
            return 1000ULL;
        case 0x04U:
            return 10000ULL;
        case 0x05U:
            return 100000ULL;
        case 0x06U:
            return 1000000ULL;
        case 0x07U:
            return 10000000ULL;
        case 0x08U:
            return 100000000ULL;
        case 0x09U:
            return 1000000000ULL;
        case 0x0AU:
            return 1ULL;  // 1ps → 0.001ns，已低于纳秒精度，向上取整为 1ns
        case 0x0BU:
            return 1ULL;  // 10ps
        case 0x0CU:
            return 1ULL;  // 100ps
        default:
            return std::nullopt;
    }
}

/// @brief DPM_SAMPLE_COUNT 的最小合法值（规范：0 与 1 非法）
inline constexpr std::uint16_t kMinDaqPackedSampleCount = 2U;

/// @brief DPM_SAMPLE_COUNT 的最大合法值（16 位无符号上限）
inline constexpr std::uint16_t kMaxDaqPackedSampleCount = 0xFFFFU;

/**
 * @brief GET_DAQ_PROCESSOR_INFO 响应的 DAQ_PROPERTIES 位
 * @details 位值取自 thirdparty/XCPlite/src/xcp.h:353-359；未知位保留在
 *          原始字节中，调用方只用本枚举判断已知能力。
 */
enum class DaqProcessorPropertyBit : std::uint8_t {
    kNone = 0x00,               ///< 无声明能力
    kConfigType = 0x01,         ///< bit0：DAQ 配置类型（1=DYNAMIC）
    kPrescaler = 0x02,          ///< bit1：支持 Prescaler
    kResume = 0x04,             ///< bit2：支持 Resume
    kBitStim = 0x08,            ///< bit3：支持 BIT_STIM
    kTimestamp = 0x10,          ///< bit4：支持 Timestamp
    kNoPid = 0x20,              ///< bit5：支持 PID_OFF
    kOverloadIndicator = 0xC0,  ///< bit6-7：Overload Indicator 能力
};

/// @brief 合并 DAQ 处理器能力位
[[nodiscard]] constexpr DaqProcessorPropertyBit operator|(
    DaqProcessorPropertyBit lhs, DaqProcessorPropertyBit rhs) noexcept;

/// @brief 判断 DAQ 处理器能力是否包含指定掩码
[[nodiscard]] constexpr bool HasDaqProcessorProperty(
    DaqProcessorPropertyBit props, DaqProcessorPropertyBit mask) noexcept;

// ---------------------------------------------------------------------------
// 变量标定批次：Calibration / Page Switching 位域常量
//
// 取值来源纪律（同批次14）：字段**语义与顺序**取本仓库
// docs/XCP_1.3.0_document.md（§7.5.2.5 MODIFY_BITS、§7.5.3 Page Switching）；
// 位值交叉参考只读 thirdparty/XCPlite/src/xcp.h。
// ---------------------------------------------------------------------------

/**
 * @brief SET_CAL_PAGE 的 MODE 位（docs L2038-2048）
 * @details bit0 ECU：该 Page 供 ECU Application 使用；bit1 XCP：该 Page 供
 *          XCP Driver/Master 访问；两者可同时置位。bit7 ALL：忽略 Segment
 *          Number，对全部 Segment 应用同一设置。不可行组合 Slave 返回
 *          ERR_MODE_NOT_VALID。
 */
enum class CalPageModeBit : std::uint8_t {
    kNone = 0x00,   ///< 无标志（非法请求，编码层拒绝）
    kEcu = 0x01,    ///< bit0 ECU
    kXcp = 0x02,    ///< bit1 XCP
    kAll = 0x80,    ///< bit7 ALL（作用于全部 Segment）
};

/// @brief 合并 SET_CAL_PAGE Mode 位
[[nodiscard]] constexpr CalPageModeBit operator|(CalPageModeBit lhs,
                                                 CalPageModeBit rhs) noexcept;

/// @brief Mode 位命中判断
[[nodiscard]] constexpr bool HasCalPageMode(CalPageModeBit mode,
                                            CalPageModeBit mask) noexcept;

/**
 * @brief GET_PAG_PROCESSOR_INFO 响应的 PAG_PROPERTIES 位
 * @details 位值取自 thirdparty/XCPlite/src/xcp.h（PAG_FREEZE_SUPPORTED）。
 */
enum class PagPropertyBit : std::uint8_t {
    kNone = 0x00,             ///< 无声明能力
    kFreezeSupported = 0x01,  ///< bit0 FREEZE_SUPPORTED：支持冻结处理
};

/// @brief 合并 PAG 处理器能力位
[[nodiscard]] constexpr PagPropertyBit operator|(PagPropertyBit lhs,
                                                  PagPropertyBit rhs) noexcept;

/// @brief 判断 PAG 处理器能力是否包含指定掩码
[[nodiscard]] constexpr bool HasPagProperty(PagPropertyBit props,
                                            PagPropertyBit mask) noexcept;

/**
 * @brief SET_SEGMENT_MODE / GET_SEGMENT_MODE 的 MODE 位（docs L2108-2122）
 * @details FREEZE=1 把该 Segment 选入随后 STORE_CAL_REQ 的冻结处理范围。
 */
enum class SegmentModeBit : std::uint8_t {
    kNone = 0x00,     ///< 无标志（即不冻结）
    kFreeze = 0x01,   ///< bit0 FREEZE
};

/// @brief 合并 Segment Mode 位
[[nodiscard]] constexpr SegmentModeBit operator|(SegmentModeBit lhs,
                                                 SegmentModeBit rhs) noexcept;

/// @brief Segment Mode 位命中判断
[[nodiscard]] constexpr bool HasSegmentMode(SegmentModeBit mode,
                                            SegmentModeBit mask) noexcept;

/**
 * @brief GET_SEGMENT_INFO 的 Mode 字段（docs §7.5.3.4，决定响应布局）
 */
enum class SegmentInfoMode : std::uint8_t {
    BasicInfo = 0,           ///< Mode 0：基本信息（地址或长度，DWORD）
    StandardProperties = 1,  ///< Mode 1：标准属性（MAX_PAGES 等 5 字节）
    MappingInfo = 2,         ///< Mode 2：Address Mapping 信息
};

/**
 * @brief GET_SEGMENT_INFO 的 SEGMENT_INFO 字段
 * @details Mode 0：0=Segment Address、1=Segment Length；
 *          Mode 2：0=Mapping Source Address、1=Mapping Destination Address、
 *          2=Mapping Length。Mode 1 不使用该字段（发 0）。
 */
enum class SegmentInfoSelector : std::uint8_t {
    SegmentAddress = 0,       ///< Mode 0: Segment Address / Mode 2: Mapping Source Address
    SegmentLength = 1,        ///< Mode 0: Segment Length / Mode 2: Mapping Dest Address
    MappingLength = 2,        ///< Mode 2: Mapping Length（仅 Mode 2 有效）
};

/// @brief MODIFY_BITS 的 Shift Value S 合法上界（docs §7.5.2.5：0..16）
inline constexpr std::uint8_t kModifyBitsMaxShift = 16;

/**
 * @brief GET_PAGE_INFO 响应 PAGE_PROPERTIES 的三组访问者类型（各 2 bit）
 * @details docs §7.5.3.5：ECU_ACCESS_TYPE(bit0-1)、XCP_READ_ACCESS_TYPE
 *          (bit2-3)、XCP_WRITE_ACCESS_TYPE(bit4-5)。每组 4 档含义相同：
 *          0=不允许访问；1=仅当对方不访问时允许；2=仅当对方同时访问时允许；
 *          3=无所谓（任意时刻允许）。bit6-7 保留。
 */
enum class PageAccessType : std::uint8_t {
    NotAllowed = 0,               ///< 不允许该访问者访问
    WithoutOtherAccess = 1,       ///< 仅当另一访问者不访问时允许
    WithConcurrentAccess = 2,     ///< 仅当另一访问者同时访问时允许
    DontCare = 3,                 ///< 无论另一访问者状态如何均允许
};

/**
 * @brief GET_PAGE_INFO 响应的 PAGE_PROPERTIES 拆解结果
 */
struct PageProperties {
    std::uint8_t raw = 0;                        ///< 原始字节
    PageAccessType ecu_access = PageAccessType::NotAllowed;         ///< bits0-1
    PageAccessType xcp_read_access = PageAccessType::NotAllowed;    ///< bits2-3
    PageAccessType xcp_write_access = PageAccessType::NotAllowed;   ///< bits4-5
};

/// @brief GET_CAL_PAGE 的 Access Mode 合法值（docs §7.5.3.2：仅 ECU/XCP）
enum class CalPageAccessMode : std::uint8_t {
    Ecu = 0x01,  ///< 查询 ECU Application 侧激活页
    Xcp = 0x02,  ///< 查询 XCP Master 侧激活页
};

/// @brief 拆解 PAGE_PROPERTIES 字节（纯位运算；未知位原样保留在 raw）
[[nodiscard]] constexpr PageProperties ParsePageProperties(
    std::uint8_t raw) noexcept;

// ---------------------------------------------------------------------------
// GET_DAQ_RESOLUTION_INFO（docs L2340-2364）：粒度与时间戳
// ---------------------------------------------------------------------------

/// @brief ODT Entry 粒度（GET_DAQ_RESOLUTION_INFO 的 GRANULARITY_* 取值）
/// @details docs L2358「常见 Granularity 为 1、2、4、8 Byte」；本枚举即字节数，
///          与 AddressGranularity 同风格（值即字节数，不做序号推断）。
enum class DaqGranularity : std::uint8_t {
    Byte = 1,   ///< 1 字节粒度
    Word = 2,   ///< 2 字节粒度
    DWord = 4,  ///< 4 字节粒度
    DLong = 8,  ///< 8 字节粒度
};

/**
 * @brief GET_DAQ_RESOLUTION_INFO 响应的 TIMESTAMP_MODE 拆解结果
 * @details docs L2360-2364：位宽由 TYPE 低 nibble 表示，时间单位由高 nibble
 *          表示，`TIMESTAMP_FIXED` 置位时 Master 不允许关闭时间戳。
 * @note 单位码 → 数值（ticks/单位）的换算表**本仓库无权威来源**（R13
 * 外部阻塞）， 因此本结构只保存原始码，不做任何 μs 换算。
 */
struct DaqTimestampMode {
    std::uint8_t raw = 0;        ///< TIMESTAMP_MODE 原始字节
    bool fixed = false;          ///< bit3 TIMESTAMP_FIXED
    std::uint8_t size_code = 0;  ///< 低 3 位：时间戳位宽编码
    std::uint8_t unit_code = 0;  ///< 高 4 位：时间单位编码（无码表，原样保留）
};

/// @brief 拆解 TIMESTAMP_MODE 字节（纯位运算，不解释单位）
[[nodiscard]] constexpr DaqTimestampMode ParseDaqTimestampMode(
    std::uint8_t raw) noexcept;

/**
 * @brief GET_DAQ_PROCESSOR_INFO 响应的 DAQ_KEY_BYTE 拆解结果
 * @details 位段与 A2L `IF_DATA XCP DAQ` 的三类枚举一一对应，因此桥接层
 *          （B-16 比对）可与 A2L 声明逐项比较，无需中间翻译。
 *          位掩码与取值来源：thirdparty/XCPlite/src/xcp.h:367-388
 *          （OPT_TYPE 0x0F / EXT_TYPE 0x30 / HDR_TYPE 0xC0；
 *          EXT 的 3=DAQ 不是枚举序号 2，A2L 侧同口径）。
 */
struct DaqKeyByte {
    std::uint8_t raw = 0;                     ///< 原始字节
    std::uint8_t optimisation_type = 0;       ///< bit0-3（0..5）
    std::uint8_t address_extension_mode = 0;  ///< bit4-5（0=FREE,1=ODT,3=DAQ）
    std::uint8_t identification_field_type = 0;  ///< bit6-7（0..3）
};

/// @brief 拆解 DAQ_KEY_BYTE（纯位运算）
[[nodiscard]] constexpr DaqKeyByte ParseDaqKeyByte(std::uint8_t raw) noexcept;

/**
 * @brief XCP 协议错误码（ERR Packet 的 Byte 1）
 *
 * 仅列举本阶段可能遇到的取值；未知错误码由调用方以原始 uint8_t 保留并上报。
 */
enum class ErrorCode : std::uint8_t {
    CmdSynch = 0x00,                        ///< 命令同步（SYNCH 的成功确认）
    CmdBusy = 0x10,                         ///< Slave 忙
    DaqActive = 0x11,                       ///< DAQ 正在运行
    PgmActive = 0x12,                       ///< Programming 正在进行
    CmdUnknown = 0x20,                      ///< 未知命令
    CmdSyntax = 0x21,                       ///< 命令语法错误
    OutOfRange = 0x22,                      ///< 参数超出范围
    WriteProtected = 0x23,                  ///< 写保护
    AccessDenied = 0x24,                    ///< 访问被拒绝
    AccessLocked = 0x25,                    ///< 需要 Seed&Key 解锁
    PageNotValid = 0x26,                    ///< 页无效
    ModeNotValid = 0x27,                    ///< 模式无效
    SegmentNotValid = 0x28,                 ///< 段无效
    Sequence = 0x29,                        ///< 命令序列错误
    DaqConfig = 0x2A,                       ///< DAQ 配置错误
    MemoryOverflow = 0x30,                  ///< 内存溢出
    Generic = 0x31,                         ///< 通用错误
    Verify = 0x32,                          ///< 校验失败
    ResourceTemporaryNotAccessible = 0x33,  ///< 资源暂时不可访问
    SubcmdUnknown = 0x34,                   ///< 子命令未知
};

/// @brief 将错误码转换为字符串名称（用于诊断和日志）
[[nodiscard]] std::string_view ErrorCodeName(ErrorCode code) noexcept;

/// @brief 将原始错误码字节安全转为 ErrorCode；未知值返回 std::nullopt
[[nodiscard]] std::optional<ErrorCode> ToErrorCode(std::uint8_t raw) noexcept;

/**
 * @brief BUILD_CHECKSUM 的校验算法类型（原始 PDF 第 148 页 Table 75）
 *
 * 枚举值即线上 Checksum type 字节，不做重排，便于与原始报文对照。
 * 所有算法的结果都以 DWORD 返回（规范原文：The result is always given as a
 * DWORD, regardless of the checksum type）。
 */
enum class CalibrationChecksumType : std::uint8_t {
    Add11 = 0x01,        ///< XCP_ADD_11：BYTE 累加进 BYTE 校验和
    Add12 = 0x02,        ///< XCP_ADD_12：BYTE 累加进 WORD 校验和
    Add14 = 0x03,        ///< XCP_ADD_14：BYTE 累加进 DWORD 校验和
    Add22 = 0x04,        ///< XCP_ADD_22：WORD 累加进 WORD（块长须为 2 的倍数）
    Add24 = 0x05,        ///< XCP_ADD_24：WORD 累加进 DWORD（块长须为 2 的倍数）
    Add44 = 0x06,        ///< XCP_ADD_44：DWORD 累加进 DWORD（块长须为 4 的倍数）
    Crc16 = 0x07,        ///< XCP_CRC_16（poly 0x8005, init 0x0000, 反射）
    Crc16Citt = 0x08,    ///< XCP_CRC_16_CITT（poly 0x1021, init 0xFFFF, 不反射）
    Crc32 = 0x09,        ///< XCP_CRC_32
    UserDefined = 0xFF,  ///< XCP_USER_DEFINED：需外部算法实现，本库不自行计算
};

/// @brief 将校验算法类型转为字符串名称（用于诊断和日志）
[[nodiscard]] std::string_view ChecksumTypeName(
    CalibrationChecksumType type) noexcept;

/// @brief 将原始校验类型字节安全转为枚举；未知值返回 std::nullopt
[[nodiscard]] std::optional<CalibrationChecksumType> ToChecksumType(
    std::uint8_t raw) noexcept;

/**
 * @brief SET_REQUEST 的 Mode 位掩码（原始 PDF 第 128 页 Table 63）
 *
 * 本库只使用 STORE_CAL_REQ；其余位置位时按“请求其他持久化动作”原样下发给
 * Slave，不自行解释其副作用。
 */
enum class SetRequestModeBit : std::uint8_t {
    kNone = 0x00,                 ///< 无请求位
    kStoreCalReq = 0x01,          ///< bit0：请求把标定数据保存到非易失存储
    kStoreDaqReqNoResume = 0x02,  ///< bit1：保存 DAQ 列表且不进入 RESUME
    kStoreDaqReqResume = 0x04,    ///< bit2：保存 DAQ 列表并进入 RESUME
    kClearDaqReq = 0x08,          ///< bit3：清除非易失存储中的 DAQ 配置
    kClearCalPagCfgLost = 0x10,   ///< bit4：清除 CAL_PAG_CFG_LOST 标志
    kClearDaqCfgLost = 0x20,      ///< bit5：清除 DAQ_CFG_LOST 标志
};

/// @brief 组合两个 SET_REQUEST Mode 位
[[nodiscard]] constexpr SetRequestModeBit operator|(SetRequestModeBit lhs,
                                                    SetRequestModeBit rhs) noexcept;

/// @brief 判断 Mode 中是否置位指定掩码
[[nodiscard]] constexpr bool HasSetRequestMode(SetRequestModeBit mode,
                                               SetRequestModeBit mask) noexcept;

/**
 * @brief XCP 事件码（EV Packet 的 Byte 1）
 */
enum class EventCode : std::uint8_t {
    ResumeMode = 0x00,         ///< 进入 RESUME 模式
    ClearDaq = 0x01,           ///< 请求清除 DAQ 配置
    StoreDaq = 0x02,           ///< 请求保存 DAQ 配置
    StoreCal = 0x03,           ///< 请求保存标定数据
    CmdPending = 0x05,         ///< 命令待处理（需重启 Timer）
    DaqOverload = 0x06,        ///< DAQ 过载
    SessionTerminated = 0x07,  ///< 会话被 Slave 终止
    TimeSync = 0x08,           ///< 时间同步
    StimTimeout = 0x09,        ///< STIM 超时
    Sleep = 0x0A,              ///< 休眠
    WakeUp = 0x0B,             ///< 唤醒
    EcuStateChange = 0x0C,     ///< ECU 状态变化
    User = 0xFE,               ///< 用户自定义事件
    Transport = 0xFF,          ///< 传输层事件
};

/// @brief 将事件码转换为字符串名称
[[nodiscard]] std::string_view EventCodeName(EventCode code) noexcept;

/// @brief 将原始事件码字节安全转为 EventCode；未知值返回 std::nullopt
[[nodiscard]] std::optional<EventCode> ToEventCode(std::uint8_t raw) noexcept;

/**
 * @brief XCP 字节序（来自 COMM_MODE_BASIC bit0）
 */
enum class ByteOrder : std::uint8_t {
    Intel = 0,     ///< 小端
    Motorola = 1,  ///< 大端
};

/**
 * @brief 地址粒度（来自 COMM_MODE_BASIC bit1-2）
 *
 * 枚举值即“一个地址单位对应的字节数”。
 */
enum class AddressGranularity : std::uint8_t {
    Byte = 1,   ///< 1 Byte/Address
    Word = 2,   ///< 2 Byte/Address
    DWord = 4,  ///< 4 Byte/Address
};

/// @brief 将 AG 转为字节数
[[nodiscard]] constexpr std::uint8_t AgToBytes(AddressGranularity ag) noexcept;

/**
 * @brief DOWNLOAD 命令在数据段前所需的对齐字节数
 * @param ag 地址粒度
 * @return AG=DWord 时返回 2，其余返回 0
 *
 * 依据 ASAM XCP V1.5.0 原始 PDF 第 142 页 Table 82：
 *   - ".. BYTEs Used for alignment, **only if AG >2**"
 *   - "If AG = DWORD, 2 alignment bytes must be used"
 * 即仅 DWORD 需要在数据前插 2 字节对齐，BYTE/WORD 不插。
 *
 * @note 与 UPLOAD 响应**不对称**：Table 71（第 134 页）为
 *       "only if AG > 1"，WORD 也要对齐 —— 两者不得互相套用。
 */
[[nodiscard]] constexpr std::size_t DownloadAlignmentBytes(
    AddressGranularity ag) noexcept;

/**
 * @brief 将 COMM_MODE_BASIC 的 AG 位域编码（bit1-2）转为 AG
 * @param field_value 取自 COMM_MODE_BASIC 的 bit1-2（00/01/10 有效，11 保留）
 * @return 合法时返回对应 AG；11（保留值）返回 std::nullopt
 */
[[nodiscard]] std::optional<AddressGranularity> CommModeBasicToAg(
    std::uint8_t field_value) noexcept;

/// @brief 将 COMM_MODE_BASIC 的 AG 位域还原为 bit1-2 编码值
[[nodiscard]] std::uint8_t AgToCommModeBasicField(
    AddressGranularity ag) noexcept;

/**
 * @brief XCP 资源位定义（RESOURCE 字段）
 *
 * bit1 为保留位（CAL/PAG 共用 bit0），故枚举值不连续。
 */
enum class Resource : std::uint8_t {
    None = 0x00,    ///< 无资源
    CalPag = 0x01,  ///< bit0 标定与分页
    Daq = 0x04,     ///< bit2 数据采集
    Stim = 0x08,    ///< bit3 数据刺激
    Pgm = 0x10,     ///< bit4 刷写
};

/// @brief 资源掩码类型
using ResourceMask = std::underlying_type_t<Resource>;

/// @brief 检查掩码中是否包含指定资源
[[nodiscard]] constexpr bool HasResource(ResourceMask mask,
                                         Resource res) noexcept;

/// @brief 合并资源位
[[nodiscard]] constexpr ResourceMask operator|(Resource lhs,
                                               Resource rhs) noexcept;

/**
 * @brief GET_SEED 命令的 Mode 字段（Seed&Key 分段读取）
 * @details First=0 请求 Seed 首段并从响应获得 Seed 总长度；Remainder=1 续取
 *          后续分段（仅当 Seed 长于 MAX_CTO-2 时存在）。未先发 First 直接发
 *          Remainder 时 Slave 返回 ERR_SEQUENCE（规范 §7.5.1.8）。
 *          报文中的字段顺序为 [F8][mode][resource]（OpenBLT 与
 *          robotjatek/XCP 双源交叉验证）。
 */
enum class SeedMode : std::uint8_t {
    First = 0,      ///< 模式 0：请求 Seed 第一部分（获得总长度）
    Remainder = 1,  ///< 模式 1：请求 Seed 后续部分
};

/**
 * @brief Session 状态机状态
 */
enum class SessionState {
    Disconnected,   ///< 未连接
    Connecting,     ///< 正在建立连接（CONNECT 已发送，等待响应）
    Connected,      ///< 已连接
    Disconnecting,  ///< 正在断开（DISCONNECT 已发送，等待响应）
    Recovering,     ///< 超时恢复中（SYNCH 已发送，等待 ERR_CMD_SYNCH）
    Failed,         ///< 不可恢复故障，需重新连接
};

/// @brief 将 Session 状态转为字符串
[[nodiscard]] std::string_view SessionStateName(SessionState state) noexcept;

/**
 * @brief CONNECT 响应解析结果（COMM_MODE_BASIC 已拆解）
 */
struct ConnectResponse {
    ResourceMask resource_mask{};            ///< RESOURCE 字段
    ByteOrder byte_order{ByteOrder::Intel};  ///< COMM_MODE_BASIC 中的字节序
    AddressGranularity address_granularity{
        AddressGranularity::Byte};             ///< COMM_MODE_BASIC 中的 AG
    bool slave_block_mode_supported{false};    ///< COMM_MODE_BASIC bit6
    bool optional_comm_mode_available{false};  ///< COMM_MODE_BASIC bit7
    std::uint8_t max_cto{0};   ///< MAX_CTO（有效范围 0x08..0xFF）
    std::uint16_t max_dto{0};  ///< MAX_DTO（有效范围 0x0008..0xFFFF）
    std::uint8_t protocol_layer_version{0};   ///< Protocol Layer 主版本
    std::uint8_t transport_layer_version{0};  ///< Transport Layer 主版本
};

/**
 * @brief GET_STATUS 响应解析结果
 */
struct GetStatusResponse {
    bool resume{false};                  ///< Current Session Status bit7
    bool daq_running{false};             ///< Current Session Status bit6
    bool clear_daq_req{false};           ///< Current Session Status bit3
    bool store_daq_req{false};           ///< Current Session Status bit2
    bool store_cal_req{false};           ///< Current Session Status bit0
    ResourceMask resource_protection{};  ///< Current Resource Protection Status
    std::uint8_t state_number{0};        ///< STATE_NUMBER
    std::uint16_t session_config_id{0};  ///< Session Configuration ID
};

/**
 * @brief COMM_MODE_OPTIONAL 的 MASTER_BLOCK_MODE 位（bit0）
 * @details 该位置位才表示 Slave 接受 **Master→Slave** 的块下载序列
 *          （DOWNLOAD 首帧 + DOWNLOAD_NEXT 续帧）。它与 CONNECT 响应里
 *          COMM_MODE_BASIC bit6 的 SLAVE_BLOCK_MODE（Slave→Master 上传块模式）
 *          是两个不同方向的能力，不可互相替代。
 */
constexpr std::uint8_t kCommModeOptionalMasterBlockMode = 0x01U;

/**
 * @brief GET_COMM_MODE_INFO 响应解析结果
 */
struct GetCommModeInfoResponse {
    std::uint8_t comm_mode_optional{0};    ///< COMM_MODE_OPTIONAL 原始字节
                                           ///< bit0=MASTER_BLOCK_MODE
    std::uint8_t max_bs{0};                ///< Block Mode 最大块大小
    std::uint8_t min_st{0};                ///< 最小分离时间（单位 100μs）
    std::uint8_t queue_size{0};            ///< Interleaved Mode 队列深度
    std::uint8_t driver_version_major{0};  ///< Driver Version 高 nibble
    std::uint8_t driver_version_minor{0};  ///< Driver Version 低 nibble
};

/**
 * @brief XCP 40 位地址（32-bit Address + 8-bit Extension）
 */
struct XcpAddress40 {
    Address address{};             ///< 32 位地址
    AddressExtension extension{};  ///< 8 位地址扩展

    /**
     * @brief 地址前进指定元素数
     * @param elements 前进的元素数
     * @param ag 地址粒度（仅作为语义说明保留，不参与地址推进计算）
     * @return 前进后的新地址；32 位地址部分溢出时返回 std::nullopt
     * @note 地址按元素计数：前进 n 个元素即 address + n。依据 XCP V1.5.0
     *       原始 PDF 第 112 页（AG 是单个地址所含元素的尺寸）与第 113 页
     *       Table 43（WORD 下 Address n+1 = Byte 02）。AG 只用于字节长度换算。
     * @note 地址扩展不参与本次进位（跨扩展边界的处理属于后续 SEGMENT 功能）。
     */
    [[nodiscard]] std::optional<XcpAddress40> Advance(
        ElementCount elements, AddressGranularity ag) const noexcept;
};

/// @brief 相等比较
[[nodiscard]] bool operator==(const XcpAddress40& lhs,
                              const XcpAddress40& rhs) noexcept;

/**
 * @brief 完整 Session 参数快照（CONNECT + 后续查询的不可变结果）
 */
struct SessionParameters {
    ConnectResponse connect;  ///< CONNECT 协商结果
    std::optional<GetCommModeInfoResponse>
        comm_mode_info;                       ///< 仅在查询成功时存在
    std::optional<GetStatusResponse> status;  ///< 仅在查询成功时存在
    bool short_upload_available{true};        ///< SHORT_UPLOAD 是否可用
};

// ---------------------------------------------------------------------------
// 变量标定批次：请求 / 响应结构体
// ---------------------------------------------------------------------------

/// @brief MODIFY_BITS 请求字段（docs §7.5.2.5；操作对象为当前 MTA 处 32bit）
struct ModifyBitsRequest {
    std::uint8_t shift{0};     ///< Shift Value S（有效范围 0..16）
    std::uint16_t and_mask{0xFFFFU};  ///< AND Mask（左移 S 后与值相与）
    std::uint16_t xor_mask{0x0000U};  ///< XOR Mask（左移 S 后与值相异或）
};

/// @brief SET_CAL_PAGE 请求字段（docs §7.5.3.1）
struct SetCalPageRequest {
    CalPageModeBit mode{CalPageModeBit::kNone};  ///< ECU/XCP/ALL 位组合
    std::uint8_t segment{0};                      ///< Segment Number（ALL 置位时忽略）
    std::uint8_t page{0};                         ///< Page Number
};

/// @brief GET_CAL_PAGE 请求字段（docs §7.5.3.2）
struct GetCalPageRequest {
    std::uint8_t access_mode{0x01};  ///< Access Mode：仅 0x01(ECU)/0x02(XCP)
    std::uint8_t segment{0};         ///< Segment Number
};

/// @brief GET_CAL_PAGE 响应解析结果
struct GetCalPageResponse {
    std::uint8_t page{0};  ///< 该 Access Mode 下当前激活的逻辑 Page Number
};

/// @brief GET_PAG_PROCESSOR_INFO 响应解析结果（docs §7.5.3.3）
struct GetPagProcessorInfoResponse {
    std::uint8_t max_segment{0};            ///< MAX_SEGMENT（Segment 数上界）
    PagPropertyBit properties{};            ///< PAG_PROPERTIES（FREEZE_SUPPORTED）
};

/// @brief GET_SEGMENT_INFO 请求字段（docs §7.5.3.4）
struct GetSegmentInfoRequest {
    SegmentInfoMode mode{SegmentInfoMode::BasicInfo};  ///< 决定响应布局
    std::uint8_t segment{0};                           ///< Segment Number
    SegmentInfoSelector info{SegmentInfoSelector::SegmentAddress};  ///< SEGMENT_INFO
    std::uint8_t mapping_index{0};                     ///< Mapping Index（仅 Mode 2）
};

/// @brief GET_SEGMENT_INFO Mode 0 响应（基本信息：地址或长度）
struct SegmentBasicInfo {
    std::uint32_t value{0};  ///< DWORD：Segment Address 或 Length
};

/// @brief GET_SEGMENT_INFO Mode 1 响应（标准属性）
struct SegmentStandardProperties {
    std::uint8_t max_pages{0};             ///< MAX_PAGES
    std::uint8_t address_extension{0};     ///< ADDRESS_EXTENSION（SET_MTA 用）
    std::uint8_t max_mapping{0};           ///< MAX_MAPPING（Mapping 条目数）
    std::uint8_t compression_method{0};    ///< Compression Method（0=不支持）
    std::uint8_t encryption_method{0};     ///< Encryption Method（0=不支持）
};

/// @brief GET_SEGMENT_INFO Mode 2 响应（Address Mapping 信息）
struct SegmentMappingInfo {
    std::uint32_t value{0};  ///< DWORD：Source/Dest Address 或 Length
};

/// @brief GET_SEGMENT_INFO 响应解析结果（variant，按请求 Mode 三选一）
using SegmentInfoData =
    std::variant<SegmentBasicInfo, SegmentStandardProperties,
                 SegmentMappingInfo>;

/// @brief GET_SEGMENT_INFO 响应解析结果
struct GetSegmentInfoResponse {
    SegmentInfoMode mode{SegmentInfoMode::BasicInfo};  ///< 对应的请求 Mode
    SegmentInfoData data{SegmentBasicInfo{}};          ///< 按 Mode 填充
};

/// @brief GET_PAGE_INFO 请求字段（docs §7.5.3.5）
struct GetPageInfoRequest {
    std::uint8_t segment{0};  ///< Segment Number
    std::uint8_t page{0};     ///< Page Number
};

/// @brief GET_PAGE_INFO 响应解析结果
struct GetPageInfoResponse {
    PageProperties properties{};  ///< PAGE_PROPERTIES 拆解
    std::uint8_t init_segment{0}; ///< INIT_SEGMENT（Page 0 含初始化数据）
};

/// @brief SET_SEGMENT_MODE 请求字段（docs §7.5.3.6）
struct SetSegmentModeRequest {
    SegmentModeBit mode{SegmentModeBit::kNone};  ///< FREEZE 位组合
    std::uint8_t segment{0};                     ///< Segment Number
};

/// @brief GET_SEGMENT_MODE 请求字段（docs §7.5.3.7）
struct GetSegmentModeRequest {
    std::uint8_t segment{0};  ///< Segment Number
};

/// @brief GET_SEGMENT_MODE 响应解析结果
struct GetSegmentModeResponse {
    SegmentModeBit mode{};  ///< MODE（HasSegmentMode(mode, kFreeze) 判冻结）
};

/// @brief COPY_CAL_PAGE 请求字段（docs §7.5.3.8）
struct CopyCalPageRequest {
    std::uint8_t src_segment{0};  ///< Source Segment Number
    std::uint8_t src_page{0};     ///< Source Page Number
    std::uint8_t dst_segment{0};  ///< Destination Segment Number
    std::uint8_t dst_page{0};     ///< Destination Page Number
};

// ---------------------------------------------------------------------------
// constexpr 工具函数实现（声明见上方）
// ---------------------------------------------------------------------------

constexpr std::uint8_t AgToBytes(AddressGranularity ag) noexcept {
    return static_cast<std::uint8_t>(ag);
}

constexpr std::size_t DownloadAlignmentBytes(AddressGranularity ag) noexcept {
    // 见声明处注明之原始 PDF 第 142 页 Table 82："only if AG >2"
    return (ag == AddressGranularity::DWord) ? std::size_t{2} : std::size_t{0};
}

constexpr bool HasResource(ResourceMask mask, Resource res) noexcept {
    return (mask & static_cast<ResourceMask>(res)) != 0;
}

constexpr ResourceMask operator|(Resource lhs, Resource rhs) noexcept {
    return static_cast<ResourceMask>(lhs) | static_cast<ResourceMask>(rhs);
}

constexpr DaqListModeBit operator|(DaqListModeBit lhs,
                                   DaqListModeBit rhs) noexcept {
    return static_cast<DaqListModeBit>(static_cast<std::uint8_t>(lhs) |
                                       static_cast<std::uint8_t>(rhs));
}

constexpr bool HasDaqMode(DaqListModeBit mode, DaqListModeBit mask) noexcept {
    return (static_cast<std::uint8_t>(mode) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr DaqListStatusBit operator|(DaqListStatusBit lhs,
                                     DaqListStatusBit rhs) noexcept {
    return static_cast<DaqListStatusBit>(static_cast<std::uint8_t>(lhs) |
                                         static_cast<std::uint8_t>(rhs));
}

constexpr bool HasDaqStatus(DaqListStatusBit status,
                            DaqListStatusBit mask) noexcept {
    return (static_cast<std::uint8_t>(status) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr SetRequestModeBit operator|(SetRequestModeBit lhs,
                                      SetRequestModeBit rhs) noexcept {
    return static_cast<SetRequestModeBit>(static_cast<std::uint8_t>(lhs) |
                                          static_cast<std::uint8_t>(rhs));
}

constexpr bool HasSetRequestMode(SetRequestModeBit mode,
                                 SetRequestModeBit mask) noexcept {
    return (static_cast<std::uint8_t>(mode) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr DtoCtrModifierBit operator|(DtoCtrModifierBit lhs,
                                      DtoCtrModifierBit rhs) noexcept {
    return static_cast<DtoCtrModifierBit>(static_cast<std::uint8_t>(lhs) |
                                          static_cast<std::uint8_t>(rhs));
}

constexpr bool HasDtoCtrModifier(DtoCtrModifierBit modifier,
                                 DtoCtrModifierBit mask) noexcept {
    return (static_cast<std::uint8_t>(modifier) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr bool HasDtoCtrProperty(DtoCtrPropertiesBit properties,
                                 DtoCtrPropertiesBit mask) noexcept {
    return (static_cast<std::uint8_t>(properties) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr std::optional<ClockPayloadFormat>
DecodeClockPayloadFormat(std::uint8_t raw) noexcept {
    switch (raw & 0x03U) {
        case 0U:
            return ClockPayloadFormat::Absent;
        case 1U:
            return ClockPayloadFormat::Dword;
        case 2U:
            return ClockPayloadFormat::Dlong;
        default:
            return std::nullopt;  // 3 = 保留，不得猜测
    }
}

constexpr std::size_t
ClockPayloadFormatByteSize(ClockPayloadFormat format) noexcept {
    switch (format) {
        case ClockPayloadFormat::Dword:
            return 4U;
        case ClockPayloadFormat::Dlong:
            return 8U;
        case ClockPayloadFormat::Absent:
        default:
            return 0U;
    }
}

constexpr TriggerInitiator TriggerInitiatorOf(std::uint8_t trigger_info) noexcept {
    return static_cast<TriggerInitiator>(trigger_info & 0x07U);
}

constexpr TimestampSamplingPoint
TimestampSamplingPointOf(std::uint8_t trigger_info) noexcept {
    return static_cast<TimestampSamplingPoint>((trigger_info >> 3U) & 0x03U);
}

constexpr std::optional<ClockPayloadFormat>
XcpSlaveClockFormatOf(std::uint8_t payload_fmt) noexcept {
    return DecodeClockPayloadFormat(payload_fmt & 0x03U);
}

constexpr std::optional<ClockPayloadFormat>
GrandmasterClockFormatOf(std::uint8_t payload_fmt) noexcept {
    return DecodeClockPayloadFormat((payload_fmt >> 2U) & 0x03U);
}

constexpr std::optional<ClockPayloadFormat>
EcuClockFormatOf(std::uint8_t payload_fmt) noexcept {
    return DecodeClockPayloadFormat((payload_fmt >> 4U) & 0x03U);
}

constexpr bool HasClusterIdentifier(std::uint8_t payload_fmt) noexcept {
    return (payload_fmt & 0x40U) != 0U;
}

constexpr ResponseFormatMode
ResponseFormatModeOf(std::uint8_t slave_config) noexcept {
    return static_cast<ResponseFormatMode>(slave_config & 0x03U);
}

constexpr TimeSyncBridgeState
TimeSyncBridgeStateOf(std::uint8_t slave_config) noexcept {
    return static_cast<TimeSyncBridgeState>((slave_config >> 3U) & 0x03U);
}

constexpr DaqTimestampRelation
DaqTimestampRelationOf(std::uint8_t slave_config) noexcept {
    return static_cast<DaqTimestampRelation>((slave_config >> 2U) & 0x01U);
}

constexpr std::uint8_t EncodeTimeCorrelationSetProperties(
    ResponseFormatMode mode, TimeSyncBridgeRequest bridge,
    bool set_cluster_id) noexcept {
    // bit0-1 RESPONSE_FMT、bit2-3 TIME_SYNC_BRIDGE、bit4 SET_CLUSTER_ID
    std::uint8_t value = static_cast<std::uint8_t>(mode) & 0x03U;
    value = static_cast<std::uint8_t>(
        value | ((static_cast<std::uint8_t>(bridge) & 0x03U) << 2U));
    if (set_cluster_id) {
        value = static_cast<std::uint8_t>(value | 0x10U);
    }
    return value;
}

constexpr SlaveClockObservability
SlaveClockObservabilityOf(std::uint8_t observable_clocks) noexcept {
    return static_cast<SlaveClockObservability>(observable_clocks & 0x03U);
}

constexpr GrandmasterClockObservability
GrandmasterClockObservabilityOf(std::uint8_t observable_clocks) noexcept {
    return static_cast<GrandmasterClockObservability>(
        (observable_clocks >> 2U) & 0x03U);
}

constexpr EcuClockObservability
EcuClockObservabilityOf(std::uint8_t observable_clocks) noexcept {
    return static_cast<EcuClockObservability>((observable_clocks >> 4U) & 0x03U);
}

constexpr SlaveClockSyncState
SlaveClockSyncStateOf(std::uint8_t sync_state) noexcept {
    return static_cast<SlaveClockSyncState>(sync_state & 0x07U);
}

constexpr GrandmasterSyncState
GrandmasterSyncStateOf(std::uint8_t sync_state) noexcept {
    return static_cast<GrandmasterSyncState>((sync_state >> 3U) & 0x01U);
}

constexpr EcuSyncState EcuSyncStateOf(std::uint8_t sync_state) noexcept {
    return static_cast<EcuSyncState>((sync_state >> 4U) & 0x03U);
}

constexpr ClockInfoBlock operator|(ClockInfoBlock lhs,
                                   ClockInfoBlock rhs) noexcept {
    return static_cast<ClockInfoBlock>(static_cast<std::uint8_t>(lhs) |
                                       static_cast<std::uint8_t>(rhs));
}

constexpr bool HasClockInfoBlock(ClockInfoBlock blocks,
                                 ClockInfoBlock mask) noexcept {
    return (static_cast<std::uint8_t>(blocks) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr std::size_t ClockInfoBlockByteSize(ClockInfoBlock block) noexcept {
    switch (block) {
        case ClockInfoBlock::kSlaveClock:
        case ClockInfoBlock::kGrandmasterClock:
        case ClockInfoBlock::kEcuClock:
            return 18U;
        case ClockInfoBlock::kClockRelation:
            return 16U;
        case ClockInfoBlock::kEcuGrandmasterClock:
            return 8U;
        case ClockInfoBlock::kNone:
        default:
            return 0U;
    }
}

constexpr DaqListPropertyBit operator|(DaqListPropertyBit lhs,
                                       DaqListPropertyBit rhs) noexcept {
    return static_cast<DaqListPropertyBit>(static_cast<std::uint8_t>(lhs) |
                                           static_cast<std::uint8_t>(rhs));
}

constexpr bool HasDaqProperty(DaqListPropertyBit props,
                              DaqListPropertyBit mask) noexcept {
    return (static_cast<std::uint8_t>(props) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr DaqEventPropertyBit operator|(DaqEventPropertyBit lhs,
                                        DaqEventPropertyBit rhs) noexcept {
    return static_cast<DaqEventPropertyBit>(static_cast<std::uint8_t>(lhs) |
                                           static_cast<std::uint8_t>(rhs));
}

constexpr bool HasDaqEventProperty(DaqEventPropertyBit props,
                                   DaqEventPropertyBit mask) noexcept {
    return (static_cast<std::uint8_t>(props) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr DaqProcessorPropertyBit operator|(
    DaqProcessorPropertyBit lhs, DaqProcessorPropertyBit rhs) noexcept {
    return static_cast<DaqProcessorPropertyBit>(static_cast<std::uint8_t>(lhs) |
                                                static_cast<std::uint8_t>(rhs));
}

constexpr bool HasDaqProcessorProperty(DaqProcessorPropertyBit props,
                                       DaqProcessorPropertyBit mask) noexcept {
    return (static_cast<std::uint8_t>(props) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr CalPageModeBit operator|(CalPageModeBit lhs,
                                   CalPageModeBit rhs) noexcept {
    return static_cast<CalPageModeBit>(static_cast<std::uint8_t>(lhs) |
                                       static_cast<std::uint8_t>(rhs));
}

constexpr bool HasCalPageMode(CalPageModeBit mode,
                              CalPageModeBit mask) noexcept {
    return (static_cast<std::uint8_t>(mode) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr PagPropertyBit operator|(PagPropertyBit lhs,
                                   PagPropertyBit rhs) noexcept {
    return static_cast<PagPropertyBit>(static_cast<std::uint8_t>(lhs) |
                                       static_cast<std::uint8_t>(rhs));
}

constexpr bool HasPagProperty(PagPropertyBit props,
                              PagPropertyBit mask) noexcept {
    return (static_cast<std::uint8_t>(props) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr SegmentModeBit operator|(SegmentModeBit lhs,
                                   SegmentModeBit rhs) noexcept {
    return static_cast<SegmentModeBit>(static_cast<std::uint8_t>(lhs) |
                                       static_cast<std::uint8_t>(rhs));
}

constexpr bool HasSegmentMode(SegmentModeBit mode,
                              SegmentModeBit mask) noexcept {
    return (static_cast<std::uint8_t>(mode) &
            static_cast<std::uint8_t>(mask)) != 0U;
}

constexpr PageProperties ParsePageProperties(std::uint8_t raw) noexcept {
    // 位段来源：docs §7.5.3.5 PAGE_PROPERTIES（ECU bits0-1 / XCP_READ bits2-3 /
    // XCP_WRITE bits4-5；bit6-7 保留在 raw 中不解释）
    PageProperties props;
    props.raw = raw;
    props.ecu_access = static_cast<PageAccessType>(raw & 0x03U);
    props.xcp_read_access = static_cast<PageAccessType>((raw >> 2) & 0x03U);
    props.xcp_write_access = static_cast<PageAccessType>((raw >> 4) & 0x03U);
    return props;
}

constexpr DaqTimestampMode ParseDaqTimestampMode(std::uint8_t raw) noexcept {
    // 位掩码取自只读参考 thirdparty/XCPlite/src/xcp.h:394-396
    // （TYPE 0x07 / FIXED 0x08 / UNIT 0xF0）；只拆位，不把 unit_code 换算成
    // 数值——时间单位码表本仓库无权威来源（R13 外部阻塞），禁止臆造。
    DaqTimestampMode mode;
    mode.raw = raw;
    mode.fixed = (raw & 0x08U) != 0U;
    mode.size_code = static_cast<std::uint8_t>(raw & 0x07U);
    mode.unit_code = static_cast<std::uint8_t>((raw & 0xF0U) >> 4);
    return mode;
}

constexpr DaqKeyByte ParseDaqKeyByte(std::uint8_t raw) noexcept {
    // 位掩码来源：thirdparty/XCPlite/src/xcp.h:367-369
    DaqKeyByte key;
    key.raw = raw;
    key.optimisation_type = static_cast<std::uint8_t>(raw & 0x0FU);
    key.address_extension_mode = static_cast<std::uint8_t>((raw & 0x30U) >> 4);
    key.identification_field_type =
        static_cast<std::uint8_t>((raw & 0xC0U) >> 6);
    return key;
}

/**
 * @brief XCP on Ethernet 的 PACKET_ALIGNMENT（TL V1.5 §4.3.2，PDF 第 9-10 页）
 *
 * @details 规范原文："PACKET_ALIGNMENT_x (x = 8, 16, 32) indicates the
 *          alignment that has to be used for XCP packets." 且
 *          "The alignment indicated by PACKET_ALIGNMENT_x must be fulfilled
 *          for each XCP packet."
 *
 *          本枚举用 **位宽**（8/16/32 bit）而非字节数命名，与 A2L 关键字
 *          `PACKET_ALIGNMENT_8/16/32` 及规范文字一一对应，避免调用方在
 *          「位」与「字节」之间自行换算出错。字节数请用 AlignmentBytes()。
 *
 * @note 规范的 `Unknown` 表示「没有任何来源可证明对齐」——此时**不得**
 *       假定 8-bit。方案 §1.9A.4 明确：无 A2L/配置证据时 Alignment 必须
 *       保持 Unknown，并阻断依赖多 Frame/Tail 的能力（见 T-06）。
 */
enum class PacketAlignment : std::uint8_t {
    /// @brief 对齐未知：既无 A2L 声明也无显式配置，不得推断
    Unknown = 0,

    /// @brief PACKET_ALIGNMENT_8：1 字节对齐（等价于不填充）
    Bit8 = 8,

    /// @brief PACKET_ALIGNMENT_16：2 字节对齐
    Bit16 = 16,

    /// @brief PACKET_ALIGNMENT_32：4 字节对齐
    Bit32 = 32,
};

/**
 * @brief 取 PACKET_ALIGNMENT 对应的**字节**数
 * @param alignment 对齐位宽
 * @return 8→1、16→2、32→4；`Unknown` 返回 0（调用方须先判 Unknown）
 *
 * @note `Unknown` 返回 0 而非 1：0 无法作为模数参与对齐计算，会迫使调用方
 *       显式处理「未知」分支，而不是悄悄退化成一个看似合理的 1 字节对齐。
 */
[[nodiscard]] constexpr std::uint8_t AlignmentBytes(
    PacketAlignment alignment) noexcept {
    switch (alignment) {
        case PacketAlignment::Bit8:
            return 1;
        case PacketAlignment::Bit16:
            return 2;
        case PacketAlignment::Bit32:
            return 4;
        case PacketAlignment::Unknown:
            break;
    }
    return 0;
}

/**
 * @brief 解析 A2L `PACKET_ALIGNMENT_x` 的**原始位宽**为强类型
 * @param raw_bits 原始位宽（8/16/32），0 或其它值表示未知
 * @return 对应枚举；非 8/16/32 一律返回 `Unknown`
 *
 * @details 上游 A2L 桥接层（第三方 A2L SDK 的 DTO 映射）提供的
 *          `packet_alignment` 字段已是位宽（8/16/32，0 表示未知）。这里做
 *          防御性收敛：任何非 {8,16,32} 的取值都归为 `Unknown`，**不猜测**成 8。
 */
[[nodiscard]] constexpr PacketAlignment ParsePacketAlignment(
    std::uint8_t raw_bits) noexcept {
    switch (raw_bits) {
        case 8:
            return PacketAlignment::Bit8;
        case 16:
            return PacketAlignment::Bit16;
        case 32:
            return PacketAlignment::Bit32;
        default:
            break;
    }
    return PacketAlignment::Unknown;
}

/**
 * @brief 计算把后续 XCP Packet 对齐到 `alignment` 所需的 Tail 字节数
 * @param alignment 对齐位宽（`Unknown` 时返回 0）
 * @param frame_without_tail_bytes 当前 Frame 的「Header + XCP Packet」长度
 * @return 0..3 字节的 FILL 数量
 *
 * @details 依据 TL V1.5 §4.3.2.2（PDF 第 10 页）：
 *          "The tail of a preceding XCP message is built in such a way that
 *          the required alignment of the XCP packet of the subsequent message
 *          is fulfilled. Depending on the length of its XCP packet and
 *          depending on the required alignment, the tail can consist of 0, 1,
 *          2 or 3 FILL bytes."
 *
 *          公式与方案 §1.9A.3 冻结式一致：
 *          `tail = (a - (node_len mod a)) mod a`，其中 `a = alignment/8`。
 *          该式与「后继 Packet 的 Datagram 绝对偏移须为 a 的整数倍」等价——
 *          因为 Header 恒为 4 字节、后继 Packet 紧跟在当前 Frame 的 Tail 之后。
 *
 * @note FILL 字节内容规范定为 "do not care"（§4.3.2.2），本库固定填 0x00，
 *       便于测试与日志比对。
 */
[[nodiscard]] constexpr std::uint8_t TailBytesForAlignment(
    PacketAlignment alignment, std::size_t frame_without_tail_bytes) noexcept {
    const std::uint8_t a = AlignmentBytes(alignment);
    if (a == 0U) {
        // Unknown：无对齐依据，不填充（调用方须自行决定是否允许该能力）
        return 0;
    }
    const std::size_t rem = frame_without_tail_bytes % a;
    return static_cast<std::uint8_t>((a - rem) % a);
}

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_PROTOCOL_TYPES_HPP_
