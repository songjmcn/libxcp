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
#include "libxcp/protocol_version.hpp"
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
 * @brief GET_DAQ_LIST_MODE 响应解析结果（规范 Table 150）
 * @details RES 数据（去掉 0xFF 后）为 7 字节：
 *          [Current Mode][reserved WORD][Event Channel WORD][Prescaler]
 *          [Priority]。注意 `mode` 是**Slave 当前状态位**（SELECTED/
 *          DIRECTION/DTO_CTR/TIMESTAMP/PID_OFF/RUNNING/RESUME），与
 *          SET_DAQ_LIST_MODE 的请求 MODE 位语义不同，
 *          不能把 `mode` 原样回填给写命令。
 */
/**
 * @brief GET_DAQ_PACKED_MODE 响应解析结果（V1.5 §7.5.4.8 Table 128/129）
 * @details RES 数据（去掉 0xFF 后）：`mode = 0` 时只有 `[Reserved][Mode]`
 *          两个字节；`mode = 1/2` 时追加 `[DPM_TIMESTAMP_MODE]` 与
 *          `[DPM_SAMPLE_COUNT WORD]`。**不能按固定长度解析**。
 */
struct DaqPackedModeResponse {
    DaqPackedMode mode{DaqPackedMode::NotPacked};  ///< 当前生效的打包方式
    DaqPackedTimestampMode timestamp_mode{
        DaqPackedTimestampMode::LastSample};  ///< 时间戳语义（mode=0 时无意义）
    std::uint16_t sample_count{0};  ///< 一帧打包采样数（mode=0 时为 0）
};

struct GetDaqListModeResponse {
    std::uint8_t mode{0};           ///< Current Mode 原始位
    std::uint16_t reserved{0};      ///< 保留字（Slave 回填，调用方不应依赖）
    std::uint16_t event_channel{0};  ///< 当前绑定的 Event Channel
    std::uint8_t prescaler{0};      ///< 事件分频（0 = 未使用）
    std::uint8_t priority{0};       ///< 优先级（越大优先级越高）
};

/**
 * @brief BUILD_CHECKSUM 响应解析结果（原始 PDF 第 148 页 Table 74）
 * @details RES 数据（去掉 0xFF 后）为 7 字节：
 *          [Checksum type][Reserved(WORD)][Checksum(DWORD)]。
 *          规范保证校验结果**恒为 DWORD**，与算法位宽无关；本结构保留原始
 *          类型字节的枚举值，未知/厂商自定义类型时 `checksum_type` 为
 *          `std::nullopt`，调用方不得据此判定校验一致。
 * @note 负响应（Table 76）携带 `MTA_BLOCK_SIZE_ALIGN`(WORD) 与
 *       `Maximum block size`(DWORD)，由错误路径的附加信息保留，不进入本结构。
 */
struct BuildChecksumResponse {
    std::optional<CalibrationChecksumType> checksum_type;  ///< 算法类型
    std::uint8_t raw_checksum_type{0};                    ///< 原始类型字节
    std::uint16_t reserved{0};                            ///< 保留字
    std::uint32_t checksum{0};                            ///< 校验结果（恒 DWORD）
};

/**
 * @brief DTO_CTR_PROPERTIES 响应解析结果（docs §7.5.4.15 Table 164）
 * @details RES 数据（去掉 0xFF 后）为 5 字节：
 *          [PROPERTIES][Current Related Event Channel(WORD)][MODE]。
 *          `properties` 保留原始位（语义见 `DtoCtrPropertiesBit`），
 *          `mode` 保留原始位（bit0 DAQ_MODE、bit1 STIM_MODE）。
 * @note 规范未在响应里回显被查询的事件通道号，调用方需自行保存入参。
 */
struct DtoCtrPropertiesResponse {
    std::uint8_t properties{0};          ///< PROPERTIES 原始位
    std::uint16_t related_event_channel{0};  ///< 当前相关事件通道号
    std::uint8_t mode{0};                ///< MODE 原始位（DAQ/STIM 各一位）
};

/**
 * @brief GET_DAQ_CLOCK 响应解析结果（docs §7.5.4.10 Table 133/134）
 * @details RES 数据（去掉 0xFF 后）至少 6 字节：
 *          [reserved][TRIGGER_INFO][PAYLOAD_FMT][时钟时间戳…]。
 *          legacy 时时间戳固定为 DWORD（格式由 GET_DAQ_RESOLUTION_INFO 决定，
 *          本结构只保留原值）；extended 时时间戳依次为
 *          XCP slave（按 FMT_XCP_SLV 为 DWORD 或 DLONG）、
 *          grandmaster（若 FMT_GRANDM != 0）、ECU（若 FMT_ECU != 0），
 *          末尾在“存在可同步/锁频时钟”时追加 1 字节 SYNC_STATE。
 * @note 规范禁止 master 使用 TRIGGER_INFO 里的 TRIGGER_INITIATOR 判定时钟
 *       来源，这里仍保留原字节供诊断，但调用方不应据此分支。
 * @note 各时钟时间戳为**原值**，不做单位换算与回绕延展；换算属上层模型。
 */
struct GetDaqClockResponse {
    std::uint8_t trigger_info{0};   ///< TRIGGER_INFO 原始字节
    std::uint8_t payload_fmt{0};    ///< PAYLOAD_FMT 原始字节
    std::uint64_t slave_clock{0};   ///< XCP slave 时钟时间戳原值
    std::size_t slave_clock_bytes{0};  ///< slave 时间戳宽度（4 或 8）
    std::optional<std::uint64_t> grandmaster_clock;  ///< grandmaster 时钟原值
    std::optional<std::uint64_t> ecu_clock;          ///< ECU 时钟原值
    std::optional<std::uint8_t> sync_state;          ///< SYNC_STATE 原始字节
    bool extended{false};  ///< 是否按 extended 布局解析
};

/**
 * @brief TIME_CORRELATION_PROPERTIES 响应解析结果（docs §7.5.6.1 Table 212）
 * @details RES 数据（去掉 0xFF 后）为 8 字节：
 *          [SLAVE_CONFIG][OBSERVABLE_CLOCKS][SYNC_STATE][CLOCK_INFO]
 *          [RESERVED][CLUSTER_ID(WORD)]。
 */
struct TimeCorrelationPropertiesResponse {
    std::uint8_t slave_config{0};        ///< SLAVE_CONFIG 原始位
    std::uint8_t observable_clocks{0};   ///< OBSERVABLE_CLOCKS 原始位
    std::uint8_t sync_state{0};          ///< SYNC_STATE 原始位
    std::uint8_t clock_info{0};          ///< CLOCK_INFO 原始位（指示数据块组成）
    std::uint8_t reserved{0};            ///< 保留字节
    std::uint16_t cluster_id{0};         ///< CLUSTER_ID
};

/**
 * @brief EV_TIME_SYNC 事件载荷解析结果（docs Table 242 / 247 / 248）
 * @details legacy（MAX_CTO = 8 或未启用高级特性）：
 *          [reserved][reserved][DWORD 时间戳]。
 *          extended 且 MAX_CTO = 8：[TRIGGER_INFO][counter 或 0][DWORD]。
 *          extended 且 MAX_CTO != 8：[TRIGGER_INFO][PAYLOAD_FMT]
 *          [可选 slave ts][可选 grandm ts][可选 ecu ts]
 *          [可选 WORD cluster id][可选 BYTE counter][可选 SYNC_STATE]。
 * @note 本结构只保留原始字节与已解析的时间戳，不推断时间同步质量。
 */
/**
 * @brief EV_TIME_SYNC 事件的载荷布局（docs Table 242 / 247 / 248）
 * @details 规范里三种布局的差异不只是“长短”，第二字节的语义都不同：
 *          - Legacy：`[reserved][reserved][DWORD]`，没有 TRIGGER_INFO；
 *          - ExtendedMaxCto8：`[TRIGGER_INFO][counter 或 0][DWORD]`，
 *            **没有 PAYLOAD_FMT**（MAX_CTO=8 时协议无法塞下更多）；
 *          - ExtendedPayloadFormat：`[TRIGGER_INFO][PAYLOAD_FMT][…]`。
 *          用枚举而不是 bool 是为了让调用方必须显式选择，不能靠长度猜。
 */
enum class TimeSyncLayout : std::uint8_t {
    Legacy,                ///< legacy：无 TRIGGER_INFO/PAYLOAD_FMT
    ExtendedMaxCto8,       ///< extended 且 MAX_CTO = 8：无 PAYLOAD_FMT
    ExtendedPayloadFormat  ///< extended：含 PAYLOAD_FMT，可按需带多时钟
};

struct TimeSyncEvent {
    std::uint8_t trigger_info{0};  ///< TRIGGER_INFO 原始字节
    std::uint8_t payload_fmt{0};   ///< PAYLOAD_FMT 原始字节（legacy 恒 0）
    std::optional<std::uint64_t> slave_clock;         ///< slave 时钟原值
    std::optional<std::uint64_t> grandmaster_clock;   ///< grandmaster 时钟原值
    std::optional<std::uint64_t> ecu_clock;           ///< ECU 时钟原值
    std::optional<std::uint16_t> cluster_id;          ///< 附加 cluster id
    std::optional<std::uint8_t> counter;              ///< 附加 counter
    std::optional<std::uint8_t> sync_state;           ///< SYNC_STATE 原始字节
    bool extended{false};  ///< 是否按 extended 布局解析
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
 * @brief GET_VERSION 响应解析结果（批次 B1）
 *
 * @details 布局依据（原始 PDF 第 114 页 Table 45「GET_VERSION response
 *          structure」，逐字）：
 * @verbatim
 *          Position Type Description
 *          0        BYTE Packet ID = 0xFF
 *          1        BYTE Reserved
 *          2        BYTE Major version of protocol layer
 *          3        BYTE Minor version of protocol layer
 *          4        BYTE Major version of currently active transport layer
 *          5        BYTE Minor version of currently active transport layer
 * @endverbatim
 *          **全部为单字节字段，字节序无影响**；`res_data` 已剥离首字节
 *          `0xFF`，故索引全体 −1：`reserved@0`、`major_pl@1`、`minor_pl@2`、
 *          `major_tl@3`、`minor_tl@4`，共 **5 字节**。
 *
 * @par 字段顺序
 *      **Protocol 在前（1/2），Transport 在后（3/4）**。二者必须写入两个
 *      独立快照，**禁止**用一方推导另一方（方案 §4 第 3 条）。
 *
 * @note `position 1` 是 **Reserved**，**不是长度字段**，也不是子命令回显。
 *       本命令响应**不含任何长度字段**（对照 GET_ID 的 Table 60 有 DWORD
 *       Length）。历史上曾有注释声称「GET_VERSION 的 LENGTH 为 WORD」，
 *       属臆造，已由 §2A 更正。
 */
struct GetVersionResponse {
    /// @brief Reserved 字节（原始 PDF 第 114 页 Table 45 position 1）
    /// @details 规范未规定其取值语义。保留原值供诊断，**不参与裁决**。
    std::uint8_t reserved{0};

    /// @brief 协议层主版本（Table 45 position 2）
    std::uint8_t protocol_major{0};

    /// @brief 协议层次版本（Table 45 position 3）
    std::uint8_t protocol_minor{0};

    /// @brief 当前活动传输层主版本（Table 45 position 4）
    std::uint8_t transport_major{0};

    /// @brief 当前活动传输层次版本（Table 45 position 5）
    std::uint8_t transport_minor{0};

    /// @brief 协议层版本视图
    [[nodiscard]] XcpProtocolVersion ProtocolVersion() const noexcept {
        return XcpProtocolVersion{protocol_major, protocol_minor};
    }

    /// @brief 传输层版本视图
    [[nodiscard]] XcpProtocolVersion TransportVersion() const noexcept {
        return XcpProtocolVersion{transport_major, transport_minor};
    }
};

/**
 * @brief GET_ID 响应解析结果（F-03a/F-03b）
 *
 * @details 布局依据（原始 PDF 第 123 页 Table 60「GET ID positive response
 *          structure」，逐字）：
 * @verbatim
 *          Position     Type    Description
 *          0            BYTE    Packet ID: 0xFF
 *          1            BYTE    Mode
 *          2            WORD    Reserved
 *          4            DWORD   Length [BYTE]
 *          8            BYTE 1  first byte of Identification (if mode = 1)
 *          8+Length-1   BYTE n  nth byte of identification
 * @endverbatim
 *          `res_data` 已剥离首字节 `0xFF`，故索引全体 -1：
 *          `mode@0`、`reserved@1..2`（WORD）、`length@3..6`（DWORD）、
 *          `data@7..`。
 *
 * @warning 本结构此前的 Doxygen 曾声称「规范 §7.5.1.6 的 LENGTH 为 WORD 且
 *          偏移不同——实然优先」，该说法**与官方规范冲突且不成立**：规范
 *          Table 60 明确 LENGTH 为 **DWORD@4**，与实现一致（无任何偏离）。
 *          该错误注释已实际导致规范审计报告做出错误的「固化 XCPlite 方言」
 *          高风险判断，故一并修正（F-03a）。另需注意 GET_VERSION 的 Table 45
 *          为固定 6 字节、**根本不含 LENGTH 字段**，故「LENGTH 为 WORD」的
 *          说法既不适用于 GET_ID 也不适用于 GET_VERSION，属臆造。
 */
struct GetIdResponse {
    /// @brief 原始 Mode 字节（保留供诊断与日志）
    /// @details Table 61 规定 bit 7..2 为保留位（x）、bit 1 为
    ///          COMPRESSED_ENCRYPTED、bit 0 为 TRANSFER_MODE。保留原始字节
    ///          以便上层在被要求时完整回显 Slave 的应答，不做有损改写。
    std::uint8_t raw_mode{0};

    /// @brief 传输模式（`Mode` 的 **bit 0**）：1=标识符内联于响应剩余字节；
    ///        0=Slave 设置 MTA，Master 用 UPLOAD 读取
    /// @details 按 Table 61 的位掩码 `Mode & 0x01` 提取，**不与 bit 1 混淆**。
    std::uint8_t transfer_mode{0};

    /// @brief 压缩/加密标志（`Mode` 的 **bit 1**）
    /// @details Table 61：COMPRESSED_ENCRYPTED 仅对 identification type 4
    ///          （ASAM-MC2 file to upload）合法。置位时数据经压缩/加密，
    ///          须由外部算法解压解密——**本库未实现该能力**，调用方必须把
    ///          该情形当作 `UnsupportedFeature` 处理，不得把密文当作 A2L 文本。
    bool compressed_encrypted{false};

    /// @brief Mode 字节中除 bit 0/bit 1 外的保留位是否为 0（Table 61 的 bit 7..2）
    /// @details 规范将 bit 7..2 标为 `x`（保留）。非零时按畸形响应处理，以
    ///          避免把未来协议的扩展位静默当作当前语义。
    bool reserved_bits_clear{true};

    /// @brief LENGTH：标识数据/文件的字节长（`Length [BYTE]`）
    /// @details Table 60 下方原文：`Length` 指定标识符的字节数；若为 0 表示
    ///          所请求的标识类型不可用。另有约束 `Length mod AG = 0`（F-03c）。
    std::uint32_t length{0};

    /// @brief LENGTH 是否满足 `Length mod AG = 0`（F-03c）
    /// @details 由解析器按当前会话 AG 判定；不满足时上层须按畸形响应拒绝，
    ///          不得进入 A2L 解析或后续 UPLOAD 编排。
    bool length_aligned_to_ag{true};

    /// @brief MODE 的 TRANSFER_MODE=1 时的响应内数据（TRANSFER_MODE=0 为空）
    Bytes identification_data;
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
     * @param ag Session 地址粒度（AG）
     * @details AG 参与长度类判据：GET_ID 的 `Length mod AG = 0`
     *          （原始 PDF 第 123 页 Table 60，F-03c）必须按会话实际 AG 判定，
     *          不能假定 BYTE。
     */
    explicit ResponseParser(ByteOrder byte_order,
                            AddressGranularity ag = AddressGranularity::Byte) noexcept;

    /// @brief 当前使用的字节序
    [[nodiscard]] ByteOrder GetByteOrder() const noexcept;

    /// @brief 当前使用的地址粒度（AG）
    [[nodiscard]] AddressGranularity GetAddressGranularity() const noexcept;

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
     * @brief 解析 GET_VERSION 响应（批次 B1）
     * @param res_data RES 后的数据（剥离 0xFF 后**长度必须 >= 5**）
     * @return 解析结果；长度不足返回 std::nullopt
     *
     * @details 布局见 `GetVersionResponse` 的 Doxygen（原始 PDF 第 114 页
     *          Table 45）。本方法不做版本合法性判断（如是否在支持范围内），
     *          只负责忠实解码——版本裁决由版本解析器负责。
     */
    [[nodiscard]] std::optional<GetVersionResponse> ParseGetVersionResponse(
        BytesView res_data) const;

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
     * @brief 解析 GET_DAQ_LIST_MODE 响应（规范 Table 150）
     * @param res_data RES 后的数据（去掉 0xFF 前缀，长度必须 >= 7：
     *                 Current Mode/reserved(WORD)/Event Channel(WORD)/
     *                 Prescaler/Priority）
     */
    [[nodiscard]] std::optional<GetDaqListModeResponse> ParseGetDaqListMode(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_DAQ_PACKED_MODE 正响应（V1.5 §7.5.4.8）
     * @param res_data 已剥离 0xFF 的 RES 数据
     * @return 解析结果；短帧、或 Slave 回了未定义的 DAQ_PACKED_MODE（3..255）、
     *         或 packed 模式下 DPM_TIMESTAMP_MODE 为保留值，一律返 `nullopt`
     * @note `mode = 0` 与 `mode = 1/2` 的载荷长度不同，按 mode 分支解析，
     *       不用长度猜。
     */
    [[nodiscard]] std::optional<DaqPackedModeResponse> ParseDaqPackedMode(
        BytesView res_data) const;

    /**
     * @brief 解析 BUILD_CHECKSUM 响应（原始 PDF 第 148 页 Table 74）
     * @param res_data RES 后的数据（去掉 0xFF 前缀，长度必须 >= 7：
     *                 Checksum type/Reserved(WORD)/Checksum(DWORD)）
     * @return 解析结果；长度不足返回 std::nullopt（不臆造校验值）
     * @note 未知校验类型不视为解析失败：保留 `raw_checksum_type` 并把
     *       `checksum_type` 置空，让上层按“无法验证”而非“验证通过”处理。
     */
    [[nodiscard]] std::optional<BuildChecksumResponse> ParseBuildChecksum(
        BytesView res_data) const;

    /**
     * @brief 解析 DTO_CTR_PROPERTIES 响应（docs §7.5.4.15 Table 164）
     * @param res_data RES 后的数据（去掉 0xFF 前缀，长度必须 >= 5：
     *                 PROPERTIES/Related Event Channel(WORD)/MODE）
     * @return 解析结果；长度不足返回 std::nullopt
     * @note 响应不回显被查询的事件通道号，调用方必须自己保留入参，
     *       否则无法把结果与查询对象对应起来。
     */
    [[nodiscard]] std::optional<DtoCtrPropertiesResponse> ParseDtoCtrProperties(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_DAQ_CLOCK 响应（docs §7.5.4.10 Table 133/134）
     * @param res_data RES 后的数据（去掉 0xFF 前缀）
     * @param extended true = 按 extended 布局（PAYLOAD_FMT 决定各时钟宽度），
     *        false = legacy（时间戳固定 DWORD）
     * @return 解析结果；长度不足或 PAYLOAD_FMT 含保留值（3）返回 std::nullopt
     * @note 时钟原值不做单位换算；SYNC_STATE 是否存在由布局长度决定，
     *       不猜测缺省值。
     */
    [[nodiscard]] std::optional<GetDaqClockResponse> ParseGetDaqClock(
        BytesView res_data, bool extended) const;

    /**
     * @brief 解析 TIME_CORRELATION_PROPERTIES 响应（docs §7.5.6.1 Table 212）
     * @param res_data RES 后的数据（去掉 0xFF 前缀，长度必须 >= 8）
     * @return 解析结果；长度不足返回 std::nullopt
     */
    [[nodiscard]] std::optional<TimeCorrelationPropertiesResponse>
    ParseTimeCorrelationProperties(BytesView res_data) const;

    /**
     * @brief 解析 EV_TIME_SYNC 事件载荷（docs Table 242 / 247 / 248）
     * @param res_data 事件包去掉 `[0xFD][0x08]` 之后的载荷
     * @param layout 载荷布局；由调用方按会话协商结果决定，本方法不猜
     * @return 解析结果；长度不足或 PAYLOAD_FMT 含保留值返回 std::nullopt
     * @note 三个布局的第二字节语义不同（见 `TimeSyncLayout`），因此不能用
     *       “先试长的再退回短的”来判定，必须由调用方显式给出布局。
     */
    [[nodiscard]] std::optional<TimeSyncEvent> ParseTimeSyncEvent(
        BytesView res_data, TimeSyncLayout layout) const;

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
    ByteOrder m_byte_order_;  ///< 会话字节序（Intel/Motorola）

    /// @brief 会话地址粒度（AG）
    /// @details 用于按 AG 判定长度类规范约束（如 GET_ID 的
    ///          `Length mod AG = 0`）。CONNECT 协商后由执行器重建解析器。
    AddressGranularity m_ag_;

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

    /// @brief 按 Session 字节序读取 64 位字段（B3：DLONG 时钟时间戳）
    /// @param data 响应数据
    /// @param offset 字段起始偏移
    /// @return 取值；越界时返回 std::nullopt
    [[nodiscard]] std::optional<std::uint64_t> ReadU64(
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
