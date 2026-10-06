/**
 * @file response_parser.hpp
 * @brief XCP 响应解析器：Packet 分类与字段提取。
 *
 * 依据 code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 10 节实现。
 * 多字节字段按 Session Byte Order 解析；XCP on Ethernet Transport Header 的
 * LEN/CTR 固定小端，由 udp_header_codec 处理，不经过本解析器。
 */

#ifndef CALMCAR_XCP_RESPONSE_PARSER_HPP_
#define CALMCAR_XCP_RESPONSE_PARSER_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/// @brief Positive Response 内容（按命令区分）
struct PositiveResponse {
    CommandCode command{};  ///< 对应的命令码（由调用方传入的期望命令）
    Bytes data;             ///< RES 后的数据（不含 0xFF 前缀）
};

/// @brief Negative Response 内容
struct NegativeResponse {
    /// @brief 错误码原始字节（保留未知值，不丢失 Slave 返回的原码）
    std::uint8_t raw_error_code{0};
    /// @brief 已识别的错误码；未知厂商码为 std::nullopt
    std::optional<ErrorCode> error_code;
    /// @brief 可选附加信息（Byte 2..）
    Bytes additional_info;
};

/// @brief Event Packet 内容
struct EventPacket {
    /// @brief 事件码原始字节（保留未知值）
    std::uint8_t raw_event_code{0};
    /// @brief 已识别的事件码；未知值为 std::nullopt
    std::optional<EventCode> event_code;
    /// @brief 可选 Event 信息（Byte 2..）
    Bytes info;
};

/// @brief Service Request Packet 内容
struct ServicePacket {
    std::uint8_t service_code{0};  ///< SERV Packet 的 Byte 1
    Bytes data;                    ///< 可选 Service 数据
};

/**
 * @brief DTO Packet（批次14 起：识别 + 整帧交付，**帧边界契约已定稿**）
 *
 * @details R12 定稿：`data` 为**完整 DTO 帧**，`data[0] == pid`，其后依次是
 *          envelope 可变字段（CTR / Timestamp / 识别字段扩展等）与净荷。
 *          定稿理由：下游解码器 `IDaqLayout::Decode(frame_layout, dto)` 的入参
 *          契约就是"含 envelope 头的完整帧"，它按 `dto[0]` 取 EPK；若这里像
 *          RES/ERR/EV/SERV 那样剥掉首字节，接线时就会把**净荷首字节误当 PID**
 *          （批次11 已警告过的风险，见 code-plan 批次13-14 计划 R12 行）。
 * @note 四类同步/异步 Packet（RES/ERR/EV/SERV）仍按原语义剥掉首字节；
 *       只有 DTO 保留，`pid` 字段仅作便捷读数，与 `data[0]` 恒等。
 */
struct DtoPacket {
    std::uint8_t pid{0};  ///< 原始 PID（0x00..0xFB；与 data[0] 恒等）
    Bytes data;           ///< 完整 DTO 帧（含 PID，data[0] == pid）
};

/// @brief GET_SEED 响应解析结果（Seed&Key，批次 7）
struct GetSeedResponse {
    /// @brief Length 字段原值（保留原始语义，不翻译）
    /// @details Mode=First 时为 Seed 总长度；Mode=Remainder 时为发送本帧前的
    ///          剩余长度；0 表示资源未保护、无需 UNLOCK（规范 §7.5.1.8）。
    std::uint8_t length{0};
    /// @brief 本帧携带的 Seed 分段字节（多段由编排层拼接）
    Bytes seed;
};

/// @brief UNLOCK 响应解析结果（Seed&Key，批次 7）
struct UnlockResponse {
    /// @brief Current Resource Protection Status；
    ///        每帧 UNLOCK 均返回（末帧为解锁后的最终掩码）
    ResourceMask resource_protection{};
};

// ---------------------------------------------------------------------------
// 批次14：DAQ 命令响应结构（字段语义取 docs/XCP_1.3.0_document.md，
// 字节偏移交叉参考只读 thirdparty/XCPlite/src/xcp.h）
// ---------------------------------------------------------------------------

/**
 * @brief START_STOP_DAQ_LIST 响应解析结果（docs L2222；xcp.h:734-735）
 * @details FIRST_PID 只在本命令响应中出现；Absolute ODT Number 模式下
 *          `Absolute_ODT_Number = FIRST_PID + Relative_ODT_Number`（docs
 *          L2225）。Relative 模式可忽略本字段（docs L2228）。
 */
struct StartStopDaqListResponse {
    std::uint8_t first_pid{0};  ///< FIRST_PID（该 DAQ List 首个 ODT 的 PID）
};

/**
 * @brief GET_DAQ_LIST_INFO 响应解析结果（docs L2452-2457；xcp.h:810-814）
 * @details **本响应不含 FIRST_PID**（计划文档 T14-04 原文误记，已按 docs
 *          L2452-2457 与 L2222 纠正——FIRST_PID 出自 START_STOP_DAQ_LIST）。
 */
struct GetDaqListInfoResponse {
    DaqListPropertyBit properties{
        DaqListPropertyBit::kNone};   ///< DAQ_LIST_PROPERTIES 原始位
    std::uint8_t max_odt{0};          ///< MAX_ODT（Static 结构给出固定值）
    std::uint8_t max_odt_entries{0};  ///< MAX_ODT_ENTRIES
    std::uint16_t fixed_event{0};     ///< FIXED_EVENT（事件通道固定值）
};

/**
 * @brief GET_DAQ_EVENT_INFO 响应解析结果（v0.3；xCPlite 实然布局
 *        xcp.h:819-825，CRO_GET_DAQ_EVENT_INFO_LEN=4 / CRM_LEN=7）
 * @details RES 数据（去掉 0xFF 后）为 6 字节，**事件通道号不在响应中回显**
 *          （调用方已持有入参 event_channel）：
 *          [PROPERTIES][MAX_DAQ_LISTS][NAME_LENGTH][TIME_CYCLE][TIME_UNIT]
 *          [PRIORITY]。
 *          - NAME_LENGTH：事件名长度（XCP 1.3.0 §7.5.4.11 可选字段；事件名主体
 *            经 UPLOAD 通路按 MTA 取自本地缓冲区，本解析器只取长度）。
 *          - time_cycle / time_unit：周期与单位码；time_unit 的实然含义（XCPlite
 *            xcplite.c:2543-：1ns=0,10ns=1,100ns=2,1us=3,…,1ms=6）
 *            写入调用方语义层，本结构只保留原码。
 *          可选优先级仅在事件信息存在 EXTENDED 属性时有效（XCPlite 恒给）。
 */
struct GetDaqEventInfoResponse {
    std::uint8_t properties{0};  ///< EVENT_PROPERTIES 原始位
    std::uint8_t max_daq_lists{0};  ///< 该事件可用 DAQ List 数（XCPlite 恒 0xFF）
    std::uint8_t name_length{0};  ///< 事件名长度
    std::uint8_t time_cycle{0};   ///< 事件周期（缩放单位），原码
    std::uint8_t time_unit{0};    ///< 周期单位码，原码
    std::uint8_t priority{0};     ///< 事件优先级（0xFF = 最高）
};

/**
 * @brief GET_DAQ_PROCESSOR_INFO 响应解析结果（docs L2292-2311；xcp.h:790-795）
 * @details B-16 的 DAQ 侧比对（identification_field_type /
 *          address_extension_mode）唯一的运行时真值来源就是本响应的
 *          DAQ_KEY_BYTE（docs L1445），因此字段一律按原码保留、不做翻译。
 */
struct GetDaqProcessorInfoResponse {
    std::uint8_t properties{0};          ///< DAQ_PROPERTIES 原始位
    std::uint16_t max_daq{0};            ///< MAX_DAQ
    std::uint16_t max_event_channel{0};  ///< MAX_EVENT_CHANNEL
    std::uint8_t min_daq{0};             ///< MIN_DAQ（PREDEFINED 列表数）
    DaqKeyByte key_byte{};               ///< DAQ_KEY_BYTE 拆解结果
};

/**
 * @brief GET_DAQ_RESOLUTION_INFO 响应解析结果（docs L2346-2356；xcp.h:799-805）
 * @details granularity/max_size 均为**字节数**原值；timestamp 只拆位不换算
 *          （时间单位码表缺失，R13 外部阻塞）。
 */
struct GetDaqResolutionInfoResponse {
    std::uint8_t granularity_daq{0};         ///< GRANULARITY_ODT_ENTRY_SIZE_DAQ
    std::uint8_t max_odt_entry_size_daq{0};  ///< MAX_ODT_ENTRY_SIZE_DAQ
    std::uint8_t granularity_stim{0};  ///< GRANULARITY_ODT_ENTRY_SIZE_STIM
    std::uint8_t max_odt_entry_size_stim{0};  ///< MAX_ODT_ENTRY_SIZE_STIM
    DaqTimestampMode timestamp_mode{};        ///< TIMESTAMP_MODE 拆解
    std::uint16_t timestamp_ticks{0};  ///< TIMESTAMP_TICKS（每单位 tick 数）
};

/**
 * @brief READ_DAQ 响应解析结果（docs L2257-2261；xcp.h:782-786）
 * @details 读当前隐含 DAQ 指针处的 ODT Entry；READ_DAQ 对 PREDEFINED 与
 *          configurable List 都可用（docs L2261），是 B-6「实际账本」在
 *          PREDEFINED 侧的唯一取证通路。
 */
struct ReadDaqResponse {
    std::uint8_t bit_offset{0};             ///< BIT_OFFSET（0xFF = 无位偏）
    std::uint8_t size{0};                   ///< SIZE（以 AG 为单位的元素数）
    AddressExtension address_extension{0};  ///< 地址扩展
    Address address{0};                     ///< 32 位地址
};

/**
 * @brief GET_ID 响应解析结果（批次 21 21-2；XCPlite 实然布局 xcp.h:521-525）
 * @details CRM：MODE=b1（0x00=走 UPLOAD、0x01=响应内含数据）、
 *          LENGTH=DWORD@b4..7、DATA=b8..。规范 §7.5.1.6 的 LENGTH 为 WORD
 *          且偏移不同——实然优先。res_data 已剥 0xFF，索引全体 -1。
 */
struct GetIdResponse {
    std::uint8_t transfer_mode{0};  ///< 传输模式（b1 原码）
    std::uint32_t length{0};        ///< LENGTH：标识数据/文件的字节长
    Bytes identification_data;  ///< MODE=0x01 时的响应内数据（MODE=0 为空）
};

/// @brief 解析后的 Packet 联合类型
using ParsedPacket = std::variant<PositiveResponse, NegativeResponse,
                                  EventPacket, ServicePacket, DtoPacket>;

/**
 * @brief XCP 响应解析器
 *
 * 解析收到的完整 XCP Packet，按首字节分类并提取字段。使用 Session Byte Order
 * 解析多字节字段。所有读取均带边界检查，畸形包返回 std::nullopt，绝不越界读。
 */
class ResponseParser {
public:
    /**
     * @brief 构造解析器
     * @param byte_order Session 字节序
     */
    explicit ResponseParser(ByteOrder byte_order) noexcept;

    /// @brief 当前使用的字节序
    [[nodiscard]] ByteOrder GetByteOrder() const noexcept;

    /**
     * @brief 解析一个完整的 XCP Packet
     * @param packet 完整 XCP Packet 字节（不含 Transport Header）
     * @param expected_command 调用方期望的命令码（用于
     * PositiveResponse.command）
     * @return 解析结果；空包等畸形输入返回 std::nullopt
     */
    [[nodiscard]] std::optional<ParsedPacket> Parse(
        BytesView packet, CommandCode expected_command) const;

    // ---- 专用解析方法（入参均为去掉 0xFF 前缀后的 RES 数据）----

    /**
     * @brief 解析 CONNECT 响应
     * @param res_data RES 后的数据（长度必须 >= 7）
     * @return 解析结果；长度不足或 AG 位域为保留值 11 时返回 std::nullopt
     */
    [[nodiscard]] std::optional<ConnectResponse> ParseConnectResponse(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_STATUS 响应
     * @param res_data RES 后的数据（长度必须 >= 5）
     */
    [[nodiscard]] std::optional<GetStatusResponse> ParseGetStatusResponse(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_COMM_MODE_INFO 响应
     * @param res_data RES 后的数据（长度必须 >= 7）
     */
    [[nodiscard]] std::optional<GetCommModeInfoResponse>
    ParseGetCommModeInfoResponse(BytesView res_data) const;

    /**
     * @brief 解析 GET_SEED 响应（Seed&Key，批次 7）
     * @param res_data RES 后的数据（布局 [length][seed...]，长度必须 >= 1）
     * @return 解析结果；长度不足返回 std::nullopt
     */
    [[nodiscard]] std::optional<GetSeedResponse> ParseGetSeedResponse(
        BytesView res_data) const;

    /**
     * @brief 解析 UNLOCK 响应（Seed&Key，批次 7）
     * @param res_data RES 后的数据（布局 [resource_protection]，长度必须 >= 1）
     * @return 解析结果；长度不足返回 std::nullopt
     */
    [[nodiscard]] std::optional<UnlockResponse> ParseUnlockResponse(
        BytesView res_data) const;

    // ---- 批次14：DAQ 命令响应（字段语义取 docs，偏移取 XCPlite 交叉参考）----

    /**
     * @brief 解析 START_STOP_DAQ_LIST 响应（docs L2222；xcp.h:734-735）
     * @param res_data RES 后的数据（长度必须 >= 1）
     * @note FIRST_PID **只在本命令响应里出现**（计划文档 T14-04 原文把它写在
     *       GET_DAQ_LIST_INFO 名下，属笔误，已按 docs L2452-2457 纠正）。
     */
    [[nodiscard]] std::optional<StartStopDaqListResponse>
    ParseStartStopDaqListResponse(BytesView res_data) const;

    /**
     * @brief 解析 GET_DAQ_LIST_INFO 响应（docs L2452-2457；xcp.h:810-814）
     * @param res_data RES 后的数据（长度必须 >= 5：PROPERTIES/MAX_ODT/
     *                 MAX_ODT_ENTRY/FIXED_EVENT(WORD)）
     */
    [[nodiscard]] std::optional<GetDaqListInfoResponse> ParseGetDaqListInfo(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_DAQ_EVENT_INFO 响应（v0.3；XCPlite 实然 xcp.h:819-825，
     *        CRO_GET_DAQ_EVENT_INFO_LEN=4 / CRM_LEN=7）
     * @param res_data RES 后的数据（去掉 0xFF 前缀，长度必须 >= 6：
     *                 PROPERTIES/MAX_DAQ_LISTS/NAME_LENGTH/TIME_CYCLE/
     *                 TIME_UNIT/PRIORITY；事件通道号不在响应中回显）
     */
    [[nodiscard]] std::optional<GetDaqEventInfoResponse>
    ParseGetDaqEventInfo(BytesView res_data) const;

    /**
     * @brief 解析 GET_DAQ_PROCESSOR_INFO 响应（docs L2292-2311；xcp.h:790-795）
     * @param res_data RES 后的数据（长度必须 >= 7：PROPERTIES/MAX_DAQ(WORD)/
     *                 MAX_EVENT_CHANNEL(WORD)/MIN_DAQ/DAQ_KEY_BYTE）
     */
    [[nodiscard]] std::optional<GetDaqProcessorInfoResponse>
    ParseGetDaqProcessorInfo(BytesView res_data) const;

    /**
     * @brief 解析 GET_DAQ_RESOLUTION_INFO 响应（docs
     * L2346-2356；xcp.h:799-805）
     * @param res_data RES 后的数据（长度必须 >= 7）
     */
    [[nodiscard]] std::optional<GetDaqResolutionInfoResponse>
    ParseGetDaqResolutionInfo(BytesView res_data) const;

    /**
     * @brief 解析 READ_DAQ 响应（docs L2257-2261；xcp.h:782-786）
     * @param res_data RES 后的数据（长度必须 >= 7：BITOFFSET/SIZE/EXT/ADDR）
     */
    [[nodiscard]] std::optional<ReadDaqResponse> ParseReadDaq(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_ID 响应（批次 21 21-2；XCPlite 实然布局，
     *        见 GetIdResponse）
     * @param res_data RES 后的数据（长度必须 >= 7：MODE + reserved×2 +
     *                 LENGTH(DWORD)）
     */
    [[nodiscard]] std::optional<GetIdResponse> ParseGetId(
        BytesView res_data) const;

    // ---- 变量标定批次：Calibration / Page Switching 响应（docs §7.5.2.5 /
    //      §7.5.3；CRM 布局对照 thirdparty/XCPlite/src/xcp.h:604-660）----

    /**
     * @brief 解析 GET_CAL_PAGE 响应（docs §7.5.3.2；xcp.h CRM_GET_CAL_PAGE_LEN=4）
     * @param res_data RES 后的数据（长度必须 >= 3：reserved×2 + PAGE_NUMBER）
     * @note 布局 [FF][reserved][reserved][page]，剥 FF 后 page 在偏移 2。
     */
    [[nodiscard]] std::optional<GetCalPageResponse> ParseGetCalPage(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_PAG_PROCESSOR_INFO 响应（docs §7.5.3.3；
     *        xcp.h CRM_GET_PAG_PROCESSOR_INFO_LEN=3）
     * @param res_data RES 后的数据（长度必须 >= 2：MAX_SEGMENT/PAG_PROPERTIES）
     */
    [[nodiscard]] std::optional<GetPagProcessorInfoResponse>
    ParseGetPagProcessorInfo(BytesView res_data) const;

    /**
     * @brief 解析 GET_SEGMENT_INFO 响应（docs §7.5.3.4；变长，按 Mode 分支）
     * @param res_data RES 后的数据
     * @param mode 请求时使用的 SegmentInfoMode（决定响应布局与最小长度）
     * @details Mode 0/2：[DWORD@0..3]（长度 >= 4）；
     *          Mode 1：[MAX_PAGES][ADDRESS_EXTENSION][MAX_MAPPING]
     *                   [COMPRESSION][ENCRYPTION]（长度 >= 5）。
     *          Slave 可能返回更长的兼容响应，多余字节安全忽略。
     */
    [[nodiscard]] std::optional<GetSegmentInfoResponse> ParseGetSegmentInfo(
        BytesView res_data, SegmentInfoMode mode) const;

    /**
     * @brief 解析 GET_PAGE_INFO 响应（docs §7.5.3.5；xcp.h CRM_GET_PAGE_INFO_LEN=3）
     * @param res_data RES 后的数据（长度必须 >= 2：PAGE_PROPERTIES/INIT_SEGMENT）
     */
    [[nodiscard]] std::optional<GetPageInfoResponse> ParseGetPageInfo(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_SEGMENT_MODE 响应（docs §7.5.3.7；
     *        xcp.h CRM_GET_SEGMENT_MODE_LEN=3）
     * @param res_data RES 后的数据（长度必须 >= 2：reserved + MODE）
     * @note 布局 [FF][reserved][mode]，剥 FF 后 mode 在偏移 1。
     */
    [[nodiscard]] std::optional<GetSegmentModeResponse> ParseGetSegmentMode(
        BytesView res_data) const;

private:
    /// @brief CONNECT 协商出的 Session 字节序，决定多字节字段的读取方向
    ByteOrder m_byte_order_;

    /// @brief 按 Session 字节序读取 16 位字段
    /// @param data 响应数据
    /// @param offset 字段起始偏移
    /// @return 取值；越界时返回 std::nullopt（由调用方判为畸形包）
    [[nodiscard]] std::optional<std::uint16_t> ReadU16(
        BytesView data, std::size_t offset) const;

    /// @brief 按 Session 字节序读取 32 位字段（批次14：READ_DAQ 地址）
    /// @param data 响应数据
    /// @param offset 字段起始偏移
    /// @return 取值；越界时返回 std::nullopt
    [[nodiscard]] std::optional<std::uint32_t> ReadU32(
        BytesView data, std::size_t offset) const;

    /// @brief 读取单字节字段（带越界保护）
    /// @param data 响应数据
    /// @param offset 字段偏移
    /// @return 取值；越界时返回 std::nullopt
    [[nodiscard]] static std::optional<std::uint8_t> ReadU8(
        BytesView data, std::size_t offset) noexcept;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_RESPONSE_PARSER_HPP_
