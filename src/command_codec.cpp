/**
 * @file command_codec.cpp
 * @brief CommandCodec 的实现：8 条命令的 CTO 编码与字节序处理。
 *
 * 报文布局依据 docs/XCP_1.3.0_document.md 第 7.5.1 节与
 * code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 9.1 / 17.1 节的字节表。
 */

#include "libxcp/command_codec.hpp"

#include "libxcp/log.hpp"

#include <string>

namespace calmcar::xcp {

namespace {

/// @brief UPLOAD / SHORT_UPLOAD 的元素数字段为单字节，有效取值 1..255
constexpr ElementCount kMaxElementsPerField = 0xFFU;

/// @brief Level-1 命令族前缀（原始 PDF 第 106 页 §7.4：以 0xC0 起始的双字节码）
constexpr std::uint8_t kLevel1CommandPrefix = 0xC0U;

/// @brief GET_VERSION 的 Level-1 子命令码（原始 PDF 第 114 页 Table 44）
constexpr std::uint8_t kGetVersionSubCommand = 0x00U;
/// @brief Level-1 子命令：SET_DAQ_PACKED_MODE（V1.5 §7.5.4.7）
constexpr std::uint8_t kSetDaqPackedModeSubCommand = 0x01U;
/// @brief Level-1 子命令：GET_DAQ_PACKED_MODE（V1.5 §7.5.4.8）
constexpr std::uint8_t kGetDaqPackedModeSubCommand = 0x02U;

}  // namespace

CommandCodec::CommandCodec(ByteOrder byte_order,
                           AddressGranularity ag) noexcept
    : m_byte_order_(byte_order), m_ag_(ag) {}

ByteOrder CommandCodec::GetByteOrder() const noexcept { return m_byte_order_; }

AddressGranularity CommandCodec::GetAddressGranularity() const noexcept {
    return m_ag_;
}

std::size_t CommandCodec::DownloadAlignmentBytes() const noexcept {
    // 规则本体在 protocol_types.hpp 的 DownloadAlignmentBytes(ag)，
    // 与 MemoryAccess 的容量计算共用同一定义，避免两处口径不一致。
    return calmcar::xcp::DownloadAlignmentBytes(m_ag_);
}

void CommandCodec::WriteU16(Bytes& buf, std::uint16_t val) const {
    if (m_byte_order_ == ByteOrder::Intel) {
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
    } else {
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
    }
}

void CommandCodec::WriteU32(Bytes& buf, std::uint32_t val) const {
    if (m_byte_order_ == ByteOrder::Intel) {
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 16) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 24) & 0xFFU));
    } else {
        buf.push_back(static_cast<std::uint8_t>((val >> 24) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 16) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>((val >> 8) & 0xFFU));
        buf.push_back(static_cast<std::uint8_t>(val & 0xFFU));
    }
}

Bytes CommandCodec::EncodeConnect(std::uint8_t mode) const {
    // CONNECT: [FF][mode]，mode 0x00=普通 / 0x01=用户自定义
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Connect));
    cto.push_back(mode);
    return cto;
}

Bytes CommandCodec::EncodeDisconnect() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Disconnect));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::EncodeGetStatus() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetStatus));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::EncodeGetVersion() const {
    // 原始 PDF 第 114 页 Table 44：Level-1 双字节命令码 [0xC0][0x00]。
    // 第二字节是子命令码，非参数——不得写入长度或保留字节。
    Bytes cto;
    cto.reserve(2);
    cto.push_back(kLevel1CommandPrefix);
    cto.push_back(kGetVersionSubCommand);
    return cto;
}

Bytes CommandCodec::EncodeSynch() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Synch));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::EncodeGetCommModeInfo() const {
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetCommModeInfo));
    cto.push_back(0x00U);  // reserved
    return cto;
}

Bytes CommandCodec::EncodeSetMta(AddressExtension extension,
                                 Address address) const {
    // SET_MTA CRO（XCP 1.3 §7.5.1.10，Table CRO 布局）：
    //   byte0=0xF6  byte1=MODE(本库恒 0=normal)  byte2=reserved
    //   byte3=Address Extension  byte4..7=Address(32bit)
    // 与 SHORT_UPLOAD 的地址域同构（ext@3/addr@4-7）。批次协议调试（XCPlite
    // 对手端）核证：旧实现把 ext 放 byte2 且总长 7，真实 Slave 按规范回
    // ERR_CMD_SYNTAX（XCPlite xcp.h CRO_SET_MTA_LEN=8, CRO_SET_MTA_EXT=byte3,
    // CRO_SET_MTA_ADDR=dw[1]=byte4-7）。
    Bytes cto;
    cto.reserve(8);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::SetMta));
    cto.push_back(0x00U);  // MODE：0 = normal（Functional Mode 属后续里程碑）
    cto.push_back(0x00U);  // reserved
    cto.push_back(extension);
    WriteU32(cto, address);
    return cto;
}

Bytes CommandCodec::EncodeUpload(ElementCount number_of_elements) const {
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "UPLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    // UPLOAD: [F5][n]
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Upload));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    return cto;
}

Bytes CommandCodec::EncodeShortUpload(ElementCount number_of_elements,
                                      AddressExtension extension,
                                      Address address) const {
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "SHORT_UPLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    Bytes cto;
    cto.reserve(8);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::ShortUpload));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    cto.push_back(0x00U);  // reserved
    cto.push_back(extension);
    WriteU32(cto, address);
    return cto;
}

Bytes CommandCodec::EncodeGetId(std::uint8_t identification_type) const {
    // GET_ID（XCPlite 实然）：[FA][IDT]，2 字节（xcp.h:519-520）；
    // 单字节字段与 Session Byte Order 无关。
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetId));
    cto.push_back(identification_type);
    return cto;
}

Bytes CommandCodec::EncodeGetSeed(Resource resource, SeedMode mode) const {
    // GET_SEED: [F8][mode][resource]。Mode 与 Resource 均为单字节字段，
    // 与 Session Byte Order 无关；resource 的单资源位合法性由 Slave 判定
    // （ERR_OUT_OF_RANGE），本地预检在 XcpMaster::Unlock 完成。
    Bytes cto;
    cto.reserve(3);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetSeed));
    cto.push_back(static_cast<std::uint8_t>(mode));
    cto.push_back(static_cast<std::uint8_t>(resource));
    return cto;
}

Bytes CommandCodec::EncodeUnlock(std::uint8_t length_field,
                                 BytesView key_segment) const {
    // 长度字段声明的剩余 Key 量不可能小于本帧携带量（首帧=总长，后续帧=剩余）
    if (static_cast<std::size_t>(length_field) < key_segment.size()) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "UNLOCK Length 字段 " + std::to_string(length_field) +
            " 小于本帧 Key 字节数 " + std::to_string(key_segment.size()));
    }
    // UNLOCK: [F7][length][key...]，单字节字段与 Session Byte Order 无关
    Bytes cto;
    cto.reserve(2 + key_segment.size());
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Unlock));
    cto.push_back(length_field);
    cto.insert(cto.end(), key_segment.begin(), key_segment.end());
    return cto;
}

Bytes CommandCodec::EncodeClearDaqList(std::uint16_t daq_list) const {
    // CLEAR_DAQ_LIST: [E3][reserved][daq_lo][daq_hi]（xcp.h:684-686）
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::ClearDaqList));
    cto.push_back(0x00U);  // reserved
    WriteU16(cto, daq_list);
    return cto;
}

Bytes CommandCodec::EncodeSetDaqPtr(std::uint16_t daq_list,
                                    std::uint8_t odt_number,
                                    std::uint8_t odt_entry_number) const {
    // SET_DAQ_PTR: [E2][reserved][daq(WORD)][odt][entry]（xcp.h:689-693）
    Bytes cto;
    cto.reserve(6);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::SetDaqPtr));
    cto.push_back(0x00U);  // reserved
    WriteU16(cto, daq_list);
    cto.push_back(odt_number);
    cto.push_back(odt_entry_number);
    return cto;
}

Bytes CommandCodec::EncodeWriteDaq(std::uint8_t bit_offset, std::uint8_t size,
                                   AddressExtension extension,
                                   Address address) const {
    // WRITE_DAQ: [E1][bit_offset][size][extension][addr(DWORD)]
    // （xcp.h:696-701）；size 以 AG 为单位，0 = 无效条目（docs L2161-2166）
    if (size == 0U) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "WRITE_DAQ Size 字段不得为 0（以 AG 为单位的元素数）");
    }
    Bytes cto;
    cto.reserve(8);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::WriteDaq));
    cto.push_back(bit_offset);  // 0xFF = 无位偏（kDaqBitOffsetNone）
    cto.push_back(size);
    cto.push_back(extension);
    WriteU32(cto, address);
    return cto;
}

Bytes CommandCodec::EncodeReadDaq() const {
    // READ_DAQ: [DB]（无参；读隐含 DAQ 指针处，xcp.h:781）
    Bytes cto;
    cto.reserve(1);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::ReadDaq));
    return cto;
}

Bytes CommandCodec::EncodeWriteDaqMultiple(
    std::span<const WriteDaqElement> elements) const {
    // WRITE_DAQ_MULTIPLE: [C7][n][每条目 8 字节]
    // 条目内序：bit_offset、size、address(DWORD)、extension、dummy
    // （docs §7.5.4.6 Table 122 的 Position 2..9 / 10..17 / …）
    if (elements.empty()) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "WRITE_DAQ_MULTIPLE 至少需要一个 Entry");
    }
    if (elements.size() > 0xFFU) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "WRITE_DAQ_MULTIPLE 的 Entry 数超出 n 字段可表达范围（最大 255）");
    }
    Bytes cto;
    cto.reserve(2U + (elements.size() * 8U));
    cto.push_back(static_cast<std::uint8_t>(CommandCode::WriteDaqMultiple));
    cto.push_back(static_cast<std::uint8_t>(elements.size()));
    for (const auto& element : elements) {
        // size 为 0 的条目在 WRITE_DAQ 里也是非法值（docs L2161-2166），
        // 这里提前拒绝，避免整份配置因单条无效而在 Slave 侧整体作废
        if (element.size == 0U) {
            LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
            throw detail::MakeInvalidArgument(
                "WRITE_DAQ_MULTIPLE 的 Size 字段不得为 0（以 AG 为单位的元素数）");
        }
        cto.push_back(element.bit_offset);
        cto.push_back(element.size);
        WriteU32(cto, element.address);
        cto.push_back(element.extension);
        // 规范要求每个条目（含最后一个）都留 dummy 对齐字节，
        // 少了这一个字节整帧就会短 1 字节
        cto.push_back(0x00U);
    }
    return cto;
}

Bytes CommandCodec::EncodeDtoCtrProperties(
    DtoCtrModifierBit modifier, std::uint16_t event_channel,
    std::uint16_t related_event_channel, DaqCounterMode daq_mode,
    bool stim_mode) const {
    // DTO_CTR_PROPERTIES: [C5][MODIFIER][EVENT(WORD)][RELATED(WORD)][MODE]
    // （docs §7.5.4.15 Table 159）
    std::uint8_t mode = 0x00U;
    if (HasDtoCtrModifier(modifier, DtoCtrModifierBit::kModifyDaqMode)) {
        mode = static_cast<std::uint8_t>(
            mode | static_cast<std::uint8_t>(daq_mode));
    } else if (daq_mode != DaqCounterMode::InsertCounter) {
        // 未声明修改 DAQ_MODE 却传了非默认值：这属于调用方语义自相矛盾，
        // 若默默忽略会让上层以为改成功了，故直接拒绝
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "DTO_CTR_PROPERTIES 未置 MODIFY_DAQ_MODE 时不得指定非默认 DAQ_MODE");
    }
    if (HasDtoCtrModifier(modifier, DtoCtrModifierBit::kModifyStimMode)) {
        if (stim_mode) {
            mode = static_cast<std::uint8_t>(mode | 0x02U);  // bit1 STIM_MODE
        }
    } else if (stim_mode) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "DTO_CTR_PROPERTIES 未置 MODIFY_STIM_MODE 时不得指定 STIM_MODE");
    }
    if (!HasDtoCtrModifier(modifier, DtoCtrModifierBit::kModifyRelatedEvent) &&
        related_event_channel != 0U) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "DTO_CTR_PROPERTIES 未置 MODIFY_RELATED_EVENT 时相关事件通道号应写 0");
    }
    Bytes cto;
    cto.reserve(7);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::DtoCtrProperties));
    cto.push_back(static_cast<std::uint8_t>(modifier));
    WriteU16(cto, event_channel);
    WriteU16(cto, related_event_channel);
    cto.push_back(mode);
    return cto;
}

Bytes CommandCodec::EncodeGetDaqClock() const {
    // GET_DAQ_CLOCK: 单字节 [DC]（docs §7.5.4.10 Table 132）
    return Bytes{static_cast<std::uint8_t>(CommandCode::GetDaqClock)};
}

Bytes CommandCodec::EncodeTimeCorrelationProperties(
    std::uint8_t set_properties, std::uint8_t get_properties_request,
    std::uint16_t cluster_id) const {
    // TIME_CORRELATION_PROPERTIES:
    //   [C6][SET_PROPERTIES][GET_PROPERTIES_REQUEST][RESERVED][CLUSTER_ID(WORD)]
    // （docs §7.5.6.1 Table 207，共 6 字节；RESERVED 规范要求写 0）
    Bytes cto;
    cto.reserve(6);
    cto.push_back(
        static_cast<std::uint8_t>(CommandCode::TimeCorrelationProperties));
    cto.push_back(set_properties);
    cto.push_back(get_properties_request);
    cto.push_back(0x00U);
    WriteU16(cto, cluster_id);
    return cto;
}

Bytes CommandCodec::EncodeSetDaqPackedMode(
    std::uint16_t daq_list, DaqPackedMode mode,
    DaqPackedTimestampMode timestamp_mode,
    std::uint16_t sample_count) const {
    // SET_DAQ_PACKED_MODE（原始 PDF Table 123/124）：
    //   [C0][01][daq_list WORD][DAQ_PACKED_MODE][DPM_TIMESTAMP_MODE]
    //   [DPM_SAMPLE_COUNT WORD]
    const bool packed = (mode != DaqPackedMode::NotPacked);
    if (packed) {
        if (sample_count < kMinDaqPackedSampleCount ||
            sample_count > kMaxDaqPackedSampleCount) {
            throw detail::MakeInvalidArgument(
                "SET_DAQ_PACKED_MODE: DPM_SAMPLE_COUNT 必须在 [2, 0xFFFF] 内，"
                "实际 " +
                std::to_string(sample_count));
        }
        if (ToDaqPackedTimestampMode(
                static_cast<std::uint8_t>(timestamp_mode)) != timestamp_mode) {
            throw detail::MakeInvalidArgument(
                "SET_DAQ_PACKED_MODE: DPM_TIMESTAMP_MODE 只允许 0（末采样）"
                "或 1（首采样）");
        }
    } else if (timestamp_mode != DaqPackedTimestampMode::LastSample ||
               sample_count != 0U) {
        // 规范规定 NotPacked 下第 5..7 字节被忽略；这里拒绝“给了又不用”的调用，
        // 免得调用方以为设置的采样数生效了。
        throw detail::MakeInvalidArgument(
            "SET_DAQ_PACKED_MODE: DAQ_PACKED_MODE=0 时时间戳语义与采样数无意义，"
            "必须传 LastSample 与 0");
    }

    Bytes cto;
    cto.reserve(8);
    cto.push_back(kLevel1CommandPrefix);
    cto.push_back(kSetDaqPackedModeSubCommand);
    WriteU16(cto, daq_list);
    cto.push_back(static_cast<std::uint8_t>(mode));
    if (packed) {
        cto.push_back(static_cast<std::uint8_t>(timestamp_mode));
        WriteU16(cto, sample_count);
    } else {
        // 规范：mode = 0 时格式与内容被忽略，统一写 0 保持报文长度一致
        cto.push_back(0x00U);
        WriteU16(cto, 0x0000U);
    }
    return cto;
}

Bytes CommandCodec::EncodeGetDaqPackedMode(std::uint16_t daq_list) const {
    // GET_DAQ_PACKED_MODE（原始 PDF Table 127）：[C0][02][daq_list WORD]
    Bytes cto;
    cto.reserve(4);
    cto.push_back(kLevel1CommandPrefix);
    cto.push_back(kGetDaqPackedModeSubCommand);
    WriteU16(cto, daq_list);
    return cto;
}

Bytes CommandCodec::EncodeSetDaqListMode(DaqListModeBit mode,
                                         std::uint16_t daq_list,
                                         std::uint16_t event_channel,
                                         std::uint8_t prescaler,
                                         std::uint8_t priority) const {
    // SET_DAQ_LIST_MODE: [E0][mode][daq(WORD)][event(WORD)][prescaler]
    //                    [priority]（xcp.h:713-719；docs L2180-2186 字段序）
    Bytes cto;
    cto.reserve(8);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::SetDaqListMode));
    cto.push_back(static_cast<std::uint8_t>(mode));
    WriteU16(cto, daq_list);
    WriteU16(cto, event_channel);
    cto.push_back(prescaler);
    cto.push_back(priority);
    return cto;
}

Bytes CommandCodec::EncodeStartStopDaqList(DaqListAction action,
                                           std::uint16_t daq_list) const {
    // START_STOP_DAQ_LIST: [DE][mode][daq(WORD)]（xcp.h:731-733）
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::StartStopDaqList));
    cto.push_back(static_cast<std::uint8_t>(action));
    WriteU16(cto, daq_list);
    return cto;
}

Bytes CommandCodec::EncodeStartStopSynch(DaqSynchAction action) const {
    // START_STOP_SYNCH: [DD][mode]（xcp.h:738-739）
    Bytes cto;
    cto.reserve(2);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::StartStopSynch));
    cto.push_back(static_cast<std::uint8_t>(action));
    return cto;
}

Bytes CommandCodec::EncodeGetDaqListInfo(std::uint16_t daq_list) const {
    // GET_DAQ_LIST_INFO: [D8][reserved][daq(WORD)]（xcp.h:808-810）
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetDaqListInfo));
    cto.push_back(0x00U);  // reserved
    WriteU16(cto, daq_list);
    return cto;
}

Bytes CommandCodec::EncodeGetDaqListMode(std::uint16_t daq_list) const {
    // GET_DAQ_LIST_MODE: [DF][reserved][daq(WORD)]（规范 Table 150）
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetDaqListMode));
    cto.push_back(0x00U);  // reserved
    WriteU16(cto, daq_list);
    return cto;
}

Bytes CommandCodec::EncodeGetDaqEventInfo(std::uint16_t event_channel) const {
    // GET_DAQ_EVENT_INFO: [D7][reserved][event_channel(WORD)]；
    // xcp.h:817-818 declares CRO_LEN=4 and CRO_WORD(1), i.e. bytes 2..3.
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetDaqEventInfo));
    cto.push_back(0x00U);
    WriteU16(cto, event_channel);
    return cto;
}

Bytes CommandCodec::EncodeGetDaqProcessorInfo() const {
    // GET_DAQ_PROCESSOR_INFO: [DA]（1 字节无参；xcp.h:789）
    Bytes cto;
    cto.reserve(1);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetDaqProcessorInfo));
    return cto;
}

Bytes CommandCodec::EncodeGetDaqResolutionInfo() const {
    // GET_DAQ_RESOLUTION_INFO: [D9]（1 字节无参；xcp.h:798
    // CRO_GET_DAQ_RESOLUTION_INFO_LEN=1）
    Bytes cto;
    cto.reserve(1);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetDaqResolutionInfo));
    return cto;
}

Bytes CommandCodec::EncodeFreeDaq() const {
    // FREE_DAQ: [D6]（1 字节无参；xcp.h:832 CRO_FREE_DAQ_LEN=1，
    // xcplite.c:2562）。释放全部动态表，运行中发→CRC_DAQ_ACTIVE（D9）
    Bytes cto;
    cto.reserve(1);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::FreeDaq));
    return cto;
}

Bytes CommandCodec::EncodeAllocDaq(std::uint16_t count) const {
    // ALLOC_DAQ: [D5][reserved][count(WORD)]
    // （xcp.h:836-838：LEN4、COUNT=CRO_WORD(1)→字节2..3；xcplite.c:2567）。
    // 时序硬门（D9/XcpAllocDaq xcplite.c:1148）：odt/entry 计数非零时再发
    // →CRC_SEQUENCE，故编排层必须"一次 alloc 全部 list 数"
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::AllocDaq));
    cto.push_back(0x00U);  // reserved
    WriteU16(cto, count);
    return cto;
}

Bytes CommandCodec::EncodeAllocOdt(std::uint16_t daq_list,
                                   std::uint8_t count) const {
    // ALLOC_ODT: [D4][reserved][daq(WORD)][count]（xcp.h:841-844：LEN5、
    // DAQ=CRO_WORD(1)→2..3、COUNT=CRO_BYTE(4)；xcplite.c:2574）。
    // 同一 list 的多个 ODT 必须连续分配后再动 entry（游标语义）
    Bytes cto;
    cto.reserve(5);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::AllocOdt));
    cto.push_back(0x00U);  // reserved
    WriteU16(cto, daq_list);
    cto.push_back(count);
    return cto;
}

Bytes CommandCodec::EncodeAllocOdtEntry(std::uint16_t daq_list,
                                        std::uint8_t odt_number,
                                        std::uint8_t count) const {
    // ALLOC_ODT_ENTRY: [D3][reserved][daq(WORD)][odt][count]
    // （xcp.h:847-851：LEN6、DAQ=CRO_WORD(1)→2..3、ODT=CRO_BYTE(4)、
    // COUNT=CRO_BYTE(5)；xcplite.c:2583）。odt 为该 list 内相对号
    Bytes cto;
    cto.reserve(6);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::AllocOdtEntry));
    cto.push_back(0x00U);  // reserved
    WriteU16(cto, daq_list);
    cto.push_back(odt_number);
    cto.push_back(count);
    return cto;
}

Bytes CommandCodec::EncodeDownload(ElementCount number_of_elements,
                                   BytesView data) const {
    // DOWNLOAD: [F0][size][pad...][data...]（xcp.h:578-582）；size 是**元素数**
    // （以 AG 为单位），与 Session Byte Order 无关。
    // ASAM XCP V1.5.0 原始 PDF 第 142 页 Table 82：对齐字节 "only if AG >2"，
    // 首元素位置 AG=1 时为 2、AG>1 时为 AG；"If AG = DWORD, 2 alignment bytes
    // must be used"。故 AG=DWord 时在 [F0][size] 之后插 2 字节对齐（填 0），
    // 使数据段落在 4 字节边界。AG=1(Byte)/2(Word) 时行为与修正前逐字节一致。
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "DOWNLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    if (data.empty()) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument("DOWNLOAD 数据段为空");
    }
    const std::size_t alignment = DownloadAlignmentBytes();
    Bytes cto;
    cto.reserve(2 + alignment + data.size());
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Download));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    // 对齐字节取值不受规范约束（原文只称 "Used for alignment"），统一填 0
    cto.insert(cto.end(), alignment, 0x00U);
    cto.insert(cto.end(), data.begin(), data.end());
    return cto;
}

Bytes CommandCodec::EncodeDownloadNext(ElementCount number_of_elements,
                                       BytesView data) const {
    // DOWNLOAD_NEXT（原始 PDF 第 144 页 Table 83）：[EF][n][对齐][data...]
    // 与 DOWNLOAD 结构同构，但 n 的语义是"剩余元素数（含本帧）"，
    // Slave 据此检测丢包；序号不符时回 ERR_SEQUENCE 并附期望元素数。
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "DOWNLOAD_NEXT NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    if (data.empty()) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument("DOWNLOAD_NEXT 数据段为空");
    }
    // 对齐规则与 DOWNLOAD 一致（Table 83 引用了同一段说明）：仅 AG>2 插 2 字节
    const std::size_t alignment = DownloadAlignmentBytes();
    Bytes cto;
    cto.reserve(2 + alignment + data.size());
    cto.push_back(static_cast<std::uint8_t>(CommandCode::DownloadNext));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    cto.insert(cto.end(), alignment, 0x00U);
    cto.insert(cto.end(), data.begin(), data.end());
    return cto;
}

Bytes CommandCodec::EncodeDownloadMax(BytesView data,
                                     std::size_t max_cto) const {
    // DOWNLOAD_MAX（原始 PDF 第 145 页 Table 85）：[EE][对齐][data...]
    // 对齐规则与 DOWNLOAD **不同**：Table 85 为 "only if AG > 1"，
    // 元素 1 位于偏移 AG，故对齐字节数 = AG-1（AG=BYTE 时 0，AG=WORD 时 1）。
    const std::size_t ag_bytes = AgToBytes(m_ag_);
    if (ag_bytes == 0U || max_cto == 0U || (max_cto % ag_bytes) != 0U) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "DOWNLOAD_MAX 要求 MAX_CTO 为 AG 的整数倍: MAX_CTO=" +
            std::to_string(max_cto) + ", AG=" + std::to_string(ag_bytes));
    }
    const std::size_t alignment = ag_bytes - 1U;
    const std::size_t expected_data = max_cto - ag_bytes;
    if (data.size() != expected_data) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "DOWNLOAD_MAX 数据长度必须为 MAX_CTO-AG = " +
            std::to_string(expected_data) + " 字节，实际 " +
            std::to_string(data.size()));
    }
    if (data.empty()) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "DOWNLOAD_MAX 无处承载数据元素（MAX_CTO 仅够一帧头部）");
    }
    Bytes cto;
    cto.reserve(1 + alignment + data.size());
    cto.push_back(static_cast<std::uint8_t>(CommandCode::DownloadMax));
    cto.insert(cto.end(), alignment, 0x00U);
    cto.insert(cto.end(), data.begin(), data.end());
    return cto;
}

Bytes CommandCodec::EncodeSetRequest(
    SetRequestModeBit mode, std::uint16_t session_configuration_id) const {
    // SET_REQUEST（原始 PDF 第 128 页 Table 62）：
    //   [F9][Mode][Session configuration id(WORD)]
    const auto mode_bits = static_cast<std::uint8_t>(mode);
    if (mode_bits == 0U) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument("SET_REQUEST Mode 不能为 0（未请求任何动作）");
    }
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::SetRequest));
    cto.push_back(mode_bits);
    WriteU16(cto, session_configuration_id);
    return cto;
}

Bytes CommandCodec::EncodeBuildChecksum(std::uint32_t block_size) const {
    // BUILD_CHECKSUM（原始 PDF 第 148 页 Table 73）：
    //   [F3][reserved][reserved(WORD)][Block size [AG](DWORD)]
    if (block_size == 0U) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument("BUILD_CHECKSUM Block size 必须大于 0");
    }
    Bytes cto;
    cto.reserve(8);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::BuildChecksum));
    cto.push_back(0x00U);  // reserved
    WriteU16(cto, 0x0000U);  // reserved
    WriteU32(cto, block_size);
    return cto;
}

Bytes CommandCodec::EncodeShortDownload(ElementCount number_of_elements,
                                        AddressExtension extension,
                                        Address address, BytesView data) const {
    // SHORT_DOWNLOAD: [ED][size][reserved][extension][addr(DWORD)][data...]
    // （xcp.h:597-602）——固定头 8 字节，MAX_CTO=8 时无处放数据（docs L2026）
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "SHORT_DOWNLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    if (data.empty()) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument("SHORT_DOWNLOAD 数据段为空");
    }
    Bytes cto;
    cto.reserve(8 + data.size());
    cto.push_back(static_cast<std::uint8_t>(CommandCode::ShortDownload));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    cto.push_back(0x00U);  // reserved
    cto.push_back(extension);
    WriteU32(cto, address);
    cto.insert(cto.end(), data.begin(), data.end());
    return cto;
}

Bytes CommandCodec::EncodeModifyBits(std::uint8_t shift, std::uint16_t and_mask,
                                     std::uint16_t xor_mask) const {
    // MODIFY_BITS: [EC][shift][AND Mask(WORD)][XOR Mask(WORD)]
    // （xcp.h CRO_MODIFY_BITS_LEN=6：SHIFT=CRO_BYTE(1)、AND=CRO_WORD(2)、
    //  XOR=CRO_WORD(3)→字节4..5）；Shift 有效范围 0..16（docs §7.5.2.5，
    // 超出 Slave 回 ERR_OUT_OF_RANGE，本地预检直接拒绝）
    if (shift > kModifyBitsMaxShift) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "MODIFY_BITS Shift Value 超出范围 0..16: " + std::to_string(shift));
    }
    Bytes cto;
    cto.reserve(6);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::ModifyBits));
    cto.push_back(shift);
    WriteU16(cto, and_mask);
    WriteU16(cto, xor_mask);
    return cto;
}

Bytes CommandCodec::EncodeSetCalPage(CalPageModeBit mode, std::uint8_t segment,
                                     std::uint8_t page) const {
    // SET_CAL_PAGE: [EB][mode][segment][page]（xcp.h CRO_SET_CAL_PAGE_LEN=4）
    // mode 至少含 ECU/XCP 之一才有意义；仅 ALL 或全零组合 Slave 无法执行，
    // 本地预检拒绝（ERR_MODE_NOT_VALID 的语义前置到调用方）
    if (!HasCalPageMode(mode, CalPageModeBit::kEcu | CalPageModeBit::kXcp)) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "SET_CAL_PAGE Mode 必须包含 ECU(0x01) 或 XCP(0x02) 位: 0x" +
            std::to_string(static_cast<unsigned>(mode)));
    }
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::SetCalPage));
    cto.push_back(static_cast<std::uint8_t>(mode));
    cto.push_back(segment);
    cto.push_back(page);
    return cto;
}

Bytes CommandCodec::EncodeGetCalPage(CalPageAccessMode access_mode,
                                     std::uint8_t segment) const {
    // GET_CAL_PAGE: [EA][access_mode][segment]（xcp.h CRO_GET_CAL_PAGE_LEN=3）
    // Access Mode 仅允许 0x01(ECU)/0x02(XCP)，其他 Slave 回
    // ERR_MODE_NOT_VALID（docs §7.5.3.2），本地预检拒绝
    if (access_mode != CalPageAccessMode::Ecu &&
        access_mode != CalPageAccessMode::Xcp) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "GET_CAL_PAGE Access Mode 仅允许 0x01(ECU)/0x02(XCP): 0x" +
            std::to_string(static_cast<unsigned>(access_mode)));
    }
    Bytes cto;
    cto.reserve(3);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetCalPage));
    cto.push_back(static_cast<std::uint8_t>(access_mode));
    cto.push_back(segment);
    return cto;
}

Bytes CommandCodec::EncodeGetPagProcessorInfo() const {
    // GET_PAG_PROCESSOR_INFO: [E9]（1 字节无参；xcp.h
    // CRO_GET_PAG_PROCESSOR_INFO_LEN=1）
    Bytes cto;
    cto.reserve(1);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetPagProcessorInfo));
    return cto;
}

Bytes CommandCodec::EncodeGetSegmentInfo(SegmentInfoMode mode,
                                         std::uint8_t segment,
                                         SegmentInfoSelector info,
                                         std::uint8_t mapping_index) const {
    // GET_SEGMENT_INFO: [E8][mode][segment][segment_info][mapping_index]
    // （xcp.h CRO_GET_SEGMENT_INFO_LEN=5）；mode/info 越界 Slave 回
    // ERR_OUT_OF_RANGE，本地预检拒绝
    const auto mode_raw = static_cast<std::uint8_t>(mode);
    const auto info_raw = static_cast<std::uint8_t>(info);
    if (mode_raw > 2U) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "GET_SEGMENT_INFO Mode 仅允许 0/1/2: " + std::to_string(mode_raw));
    }
    if (info_raw > 2U) {
        LIBXCP_LOG_ERROR("CommandCodec rejected invalid parameters");
        throw detail::MakeInvalidArgument(
            "GET_SEGMENT_INFO SegmentInfo 仅允许 0/1/2: " +
            std::to_string(info_raw));
    }
    Bytes cto;
    cto.reserve(5);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetSegmentInfo));
    cto.push_back(mode_raw);
    cto.push_back(segment);
    cto.push_back(info_raw);
    cto.push_back(mapping_index);
    return cto;
}

Bytes CommandCodec::EncodeGetPageInfo(std::uint8_t segment,
                                      std::uint8_t page) const {
    // GET_PAGE_INFO: [E7][reserved][segment][page]（xcp.h
    // CRO_GET_PAGE_INFO_LEN=4）
    Bytes cto;
    cto.reserve(4);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetPageInfo));
    cto.push_back(0x00U);  // reserved
    cto.push_back(segment);
    cto.push_back(page);
    return cto;
}

Bytes CommandCodec::EncodeSetSegmentMode(SegmentModeBit mode,
                                         std::uint8_t segment) const {
    // SET_SEGMENT_MODE: [E6][mode][segment]（xcp.h CRO_SET_SEGMENT_MODE_LEN=3）
    // mode 仅 bit0 FREEZE 有定义，其余位原样透传由 Slave 判定
    Bytes cto;
    cto.reserve(3);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::SetSegmentMode));
    cto.push_back(static_cast<std::uint8_t>(mode));
    cto.push_back(segment);
    return cto;
}

Bytes CommandCodec::EncodeGetSegmentMode(std::uint8_t segment) const {
    // GET_SEGMENT_MODE: [E5][reserved][segment]（xcp.h
    // CRO_GET_SEGMENT_MODE_LEN=3）
    Bytes cto;
    cto.reserve(3);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::GetSegmentMode));
    cto.push_back(0x00U);  // reserved
    cto.push_back(segment);
    return cto;
}

Bytes CommandCodec::EncodeCopyCalPage(const CopyCalPageRequest& request) const {
    // COPY_CAL_PAGE: [E4][src_segment][src_page][dst_segment][dst_page]
    // （xcp.h CRO_COPY_CAL_PAGE_LEN=5）
    Bytes cto;
    cto.reserve(5);
    cto.push_back(static_cast<std::uint8_t>(CommandCode::CopyCalPage));
    cto.push_back(request.src_segment);
    cto.push_back(request.src_page);
    cto.push_back(request.dst_segment);
    cto.push_back(request.dst_page);
    return cto;
}

}  // namespace calmcar::xcp
