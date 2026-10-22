/**
 * @file dto_envelope_decoder.cpp
 * @brief DtoEnvelopeDecoder：按运行时布局把原始 DTO 帧切出统一信封。
 *
 * 依据 code-plan/libxcp_测量子系统代码增长计划.md §5.1 / §6（v0.2）。
 * 解码器是纯函数式：不取证、不触网，布局由调用方经 DtoFrameLayout 传入。
 *
 * 帧结构（识别字段头长 = DtoFrameLayout::header_bytes，调用方按会话取证构造）：
 *   [识别字段 | counter? | timestamp?] | payload
 *   - Absolute：byte0 = 绝对 ODT 号；header_bytes=1。
 *   - RelativeByte：[PID(relODT)][DAQ(BYTE)]；header_bytes=2。
 *   - RelativeWord：[PID(relODT)][DAQ(WORD)]；header_bytes=3。
 *   - RelativeWordAligned：[PID(relODT)][FILL][DAQ(WORD)]；header_bytes=4。
 *     FILL 取值由规范留白，**不参与判定**。
 *   以上布局依据原始 PDF 第 95-96 页 Figure 50-53；相对 ODT 恒在 PID（byte0），
 *   DAQ 字段在其后。
 *
 * counter_enabled：DTO 计数器。`RelativeWordAligned` 时 CTR **取代 FILL 字节**
 *   （原始 PDF 第 97-98 页 Figure 56），不额外占长度；其余模式紧跟识别字段之后。
 * timestamp_enabled：再紧跟 timestamp_size_bits/8 字节原始时间戳，按会话字节序。
 * pid_off：禁止（B-7）→ MalformedPacket。
 *
 * 任何越界/不足 → MalformedPacket，不允许部分解释。
 */

#include "libxcp/daq/dto_envelope_decoder.hpp"

#include <cstdint>
#include <utility>  // std::move

#include "libxcp/protocol_types.hpp"  // ByteOrder / BytesView
#include "libxcp/xcp_error.hpp"       // detail::MakeMalformedPacket 等

namespace calmcar::xcp {

namespace {

/// @brief 抛 MalformedPacket（短帧/pid_off/识别字段非法），message 仅展示
[[noreturn]] void ThrowMalformed(std::string msg) {
    throw detail::MakeMalformedPacket(std::move(msg));
}

/// @brief 抛 UnsupportedFeature：时间戳使能但位宽无法整字节切分
[[noreturn]] void ThrowUnsupportedTs(std::uint8_t bits) {
    throw detail::MakeUnsupportedFeature(
        "DTO timestamp_enabled 但 timestamp_size_bits=" +
        std::to_string(bits) + " 非法（须为 8/16/32/64 整字节）");
}

/// @brief 按会话字节序读 [0,8) 字节 → uint64
/// @param order 会话字节序；Motorola 为高字节在前（F-07）
/// @param view 帧缓冲
/// @param offset 起始偏移
/// @param n 字节数
std::uint64_t ReadOrdered(BytesView view, std::size_t offset, std::size_t n,
                          ByteOrder order) {
    std::uint64_t v = 0;
    if (order == ByteOrder::Intel) {
        for (std::size_t i = 0; i < n; ++i) {
            v |= static_cast<std::uint64_t>(view[offset + i]) << (8U * i);
        }
    } else {
        for (std::size_t i = 0; i < n; ++i) {
            v = (v << 8U) | static_cast<std::uint64_t>(view[offset + i]);
        }
    }
    return v;
}

}  // namespace

DtoEnvelopeDecoder::DtoEnvelopeDecoder(ByteOrder byte_order) noexcept
    : m_byte_order_(byte_order) {}

DtoEnvelope DtoEnvelopeDecoder::Decode(BytesView dto,
                                       const DtoFrameLayout& frame_layout) const {
    // pid_off：无识别字段无法按 EPK 路由，B-7 解码侧必拒。
    if (frame_layout.pid_off) {
        ThrowMalformed("pid_off DTO 无识别字段，无法解码信封");
    }

    // header_bytes 即识别字段头长（不含时间戳）；帧至少要有完整识别字段。
    const std::size_t field = frame_layout.header_bytes;
    if (dto.size() < field) {
        ThrowMalformed("DTO 帧短于识别字段头");
    }

    // 识别字段内 CTR 的字节偏移（仅 RelativeWordAligned 有意义，= FILL 槽位）。
    std::optional<std::size_t> merged_counter_offset;

    DtoIdentity identity;
    switch (frame_layout.identification_field_type) {
        case IdentificationFieldType::Absolute: {
            // 识别字段仅 PID；first_odt 为 START 返回的 FIRST_PID，
            // 映射成当前单 List 内的相对 ODT 编号。first_odt=0 保持旧纯解码语义。
            if (dto[0] < frame_layout.first_odt) {
                ThrowMalformed("Absolute PID 小于 FIRST_PID");
            }
            identity.odt = static_cast<std::uint8_t>(
                dto[0] - frame_layout.first_odt);
            identity.daq_list = 0;
            break;
        }
        case IdentificationFieldType::RelativeByte: {
            // Figure 51：PID（相对 ODT）+ DAQ(BYTE)，共 2 字节。
            if (field < 2U) {
                ThrowMalformed("RelativeByte 识别字段不足 2 字节");
            }
            identity.odt =
                static_cast<std::uint8_t>(frame_layout.first_odt + dto[0]);
            identity.daq_list = dto[1];
            break;
        }
        case IdentificationFieldType::RelativeWord: {
            // Figure 52：PID（相对 ODT）+ DAQ(WORD)，共 3 字节。
            if (field < 3U) {
                ThrowMalformed("RelativeWord 识别字段不足 3 字节");
            }
            identity.odt =
                static_cast<std::uint8_t>(frame_layout.first_odt + dto[0]);
            identity.daq_list = static_cast<std::uint16_t>(
                ReadOrdered(dto, 1, 2, m_byte_order_));
            break;
        }
        case IdentificationFieldType::RelativeWordAligned: {
            // Figure 53：PID（相对 ODT）+ FILL + DAQ(WORD)，共 4 字节。
            // FILL 用于对齐，规范不约束其取值（"an extra FILL byte"），
            // 故此处**不得**校验任何固定标志值。
            if (field < 4U) {
                ThrowMalformed("RelativeWordAligned 识别字段不足 4 字节");
            }
            identity.odt =
                static_cast<std::uint8_t>(frame_layout.first_odt + dto[0]);
            identity.daq_list = static_cast<std::uint16_t>(
                ReadOrdered(dto, 2, 2, m_byte_order_));
            // Figure 56：CTR 取代 FILL 字节（偏移 1），不额外占用帧长。
            if (frame_layout.counter_enabled) {
                merged_counter_offset = 1U;
            }
            break;
        }
        default:
            ThrowMalformed("未知 DTO identification field 模式");
    }

    std::size_t off = field;  // 当前指到 counter/timestamp 段起点

    // counter：
    //  - RelativeWordAligned：CTR 已在识别字段内（取代 FILL），此处不消费字节；
    //  - 其余模式：使能时紧跟识别字段之后，占 1 字节。
    std::optional<std::uint8_t> counter;
    if (merged_counter_offset.has_value()) {
        counter = dto[*merged_counter_offset];
    } else if (frame_layout.counter_enabled) {
        if (off + 1 > dto.size()) {
            ThrowMalformed("DTO 计数器越界");
        }
        counter = dto[off];
        ++off;
    }

    // timestamp：使能时先做位宽→整字节切分合法性校验（8/16/32/64 整字节）。
    // first_only（XCPlite D13）：相对模式下只有本事件首 ODT 帧带时间戳，
    // 其余帧整段跳过——不消费字节、不产生 raw_timestamp（不猜，B-3）。
    std::optional<std::uint64_t> raw_timestamp;
    const bool ts_present =
        frame_layout.timestamp_enabled &&
        !(frame_layout.timestamp_relative_first_only &&
          frame_layout.identification_field_type !=
              IdentificationFieldType::Absolute &&
          identity.odt != frame_layout.first_odt);
    if (ts_present) {
        const std::size_t ts_bytes = frame_layout.timestamp_size_bits / 8U;
        if (const auto rem = frame_layout.timestamp_size_bits % 8U; rem != 0 ||
            ts_bytes == 0 || ts_bytes > sizeof(std::uint64_t)) {
            ThrowUnsupportedTs(frame_layout.timestamp_size_bits);
        }
        if (off + ts_bytes > dto.size()) {
            ThrowMalformed("DTO 原始时间戳越界");
        }
        // F-07：按会话字节序解码，不再固定小端。
        raw_timestamp = ReadOrdered(dto, off, ts_bytes, m_byte_order_);
        off += ts_bytes;
    }

    // payload：去掉识别字段 + counter + timestamp 后的净荷段。
    BytesView payload;
    if (off <= dto.size()) {
        payload = dto.subspan(off);
    }

    DtoEnvelope envelope;
    envelope.identity = identity;
    envelope.counter = counter;
    envelope.raw_timestamp = raw_timestamp;
    envelope.payload = payload;
    return envelope;
}

}  // namespace calmcar::xcp