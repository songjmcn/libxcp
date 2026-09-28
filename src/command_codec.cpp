/**
 * @file command_codec.cpp
 * @brief CommandCodec 的实现：8 条命令的 CTO 编码与字节序处理。
 *
 * 报文布局依据 docs/XCP_1.3.0_document.md 第 7.5.1 节与
 * code-plan/XCP_1.3.0_详细设计_类接口与头文件.md 第 9.1 / 17.1 节的字节表。
 */

#include "libxcp/command_codec.hpp"

#include <string>

namespace calmcar::xcp {

namespace {

/// @brief UPLOAD / SHORT_UPLOAD 的元素数字段为单字节，有效取值 1..255
constexpr ElementCount kMaxElementsPerField = 0xFFU;

}  // namespace

CommandCodec::CommandCodec(ByteOrder byte_order) noexcept
    : m_byte_order_(byte_order) {}

ByteOrder CommandCodec::GetByteOrder() const noexcept { return m_byte_order_; }

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
    // DOWNLOAD: [F0][size][data...]（xcp.h:578-582）；size 是**元素数**
    // （以 AG 为单位），与 Session Byte Order 无关
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        throw detail::MakeInvalidArgument(
            "DOWNLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    if (data.empty()) {
        throw detail::MakeInvalidArgument("DOWNLOAD 数据段为空");
    }
    Bytes cto;
    cto.reserve(2 + data.size());
    cto.push_back(static_cast<std::uint8_t>(CommandCode::Download));
    cto.push_back(static_cast<std::uint8_t>(number_of_elements));
    cto.insert(cto.end(), data.begin(), data.end());
    return cto;
}

Bytes CommandCodec::EncodeShortDownload(ElementCount number_of_elements,
                                        AddressExtension extension,
                                        Address address, BytesView data) const {
    // SHORT_DOWNLOAD: [ED][size][reserved][extension][addr(DWORD)][data...]
    // （xcp.h:597-602）——固定头 8 字节，MAX_CTO=8 时无处放数据（docs L2026）
    if (number_of_elements == 0U || number_of_elements > kMaxElementsPerField) {
        throw detail::MakeInvalidArgument(
            "SHORT_DOWNLOAD NumberOfElements 超出单字节字段范围 1..255: " +
            std::to_string(number_of_elements));
    }
    if (data.empty()) {
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

}  // namespace calmcar::xcp
