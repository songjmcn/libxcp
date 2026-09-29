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
    ModifyBits = 0xEC,         ///< 位修改
    // ---- 批次14：DAQ 命令组（0xE3..0xD8；docs §7.5.4 系列） ----
    ClearDaqList = 0xE3,      ///< 清除 DAQ List 的全部 ODT Entry
    SetDaqPtr = 0xE2,         ///< 设置 DAQ 指针（后续 WRITE/READ_DAQ 用）
    WriteDaq = 0xE1,          ///< 向 DAQ 指针处写一个 ODT Entry
    SetDaqListMode = 0xE0,    ///< 设置 DAQ List 工作模式
    StartStopDaqList = 0xDE,  ///< 启停/选择一个 DAQ List（响应含 FIRST_PID）
    StartStopSynch = 0xDD,    ///< 同步启停全部或选中的 DAQ List
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

/// @brief DAQ List 模式位合并（供 Master 组装 MODE 字节）
[[nodiscard]] constexpr DaqListModeBit operator|(DaqListModeBit lhs,
                                                 DaqListModeBit rhs) noexcept;

/// @brief 模式位与掩码的可组合判断（返回是否命中掩码内任一标志）
/// @note 报文字节可用 `static_cast<DaqListModeBit>(raw)` 安全转入本枚举
///       （固定底层类型，取值覆盖整个 uint8 范围）。
[[nodiscard]] constexpr bool HasDaqMode(DaqListModeBit mode,
                                        DaqListModeBit mask) noexcept;

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
};

/// @brief DAQ List 属性位合并
[[nodiscard]] constexpr DaqListPropertyBit operator|(
    DaqListPropertyBit lhs, DaqListPropertyBit rhs) noexcept;

/// @brief 属性位命中判断
[[nodiscard]] constexpr bool HasDaqProperty(DaqListPropertyBit props,
                                            DaqListPropertyBit mask) noexcept;

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
 * @brief GET_COMM_MODE_INFO 响应解析结果
 */
struct GetCommModeInfoResponse {
    std::uint8_t comm_mode_optional{0};    ///< COMM_MODE_OPTIONAL 原始字节
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
     * @brief 地址前进指定元素数（按 AG 换算为字节数）
     * @param elements 前进的元素数
     * @param ag 地址粒度
     * @return 前进后的新地址；32 位地址部分溢出时返回 std::nullopt
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
// constexpr 工具函数实现（声明见上方）
// ---------------------------------------------------------------------------

constexpr std::uint8_t AgToBytes(AddressGranularity ag) noexcept {
    return static_cast<std::uint8_t>(ag);
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

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_PROTOCOL_TYPES_HPP_
