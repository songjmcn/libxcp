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
 *   - RelativeByte（XCPlite 实然）：[relODT][0xAA][DAQ16 LE]，header_bytes=4；
 *     相对 ODT 复原 + DAQ16 取 == 账本 daq_list。
 *   - RelativeWord：低字节 DAQ 号、高字节相对 ODT（Intel 文档口径）；
 *     header_bytes=2。
 *   - RelativeWordAligned：WORD 但扩展表按 4 字节 DWORD 对齐（header_bytes=4，
 *     payload 起点天然对 4）。
 *
 * counter_enabled：识别字段后紧跟 1 字节 DTO 计数器。
 * timestamp_enabled：再紧跟 timestamp_size_bits/8 字节原始时间戳（小端）。
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

/// @brief XCPlite 相对枚举 0xAA 标志（识别字段分隔标记，见 RelativeByte 注释）
constexpr std::uint8_t kXcpliteRelativeMarker = 0xAAU;

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

/// @brief 小端读 [0,8) 字节 → uint64（Intel；raw 段永远按小端，符合会话取证）
std::uint64_t ReadLe(BytesView view, std::size_t offset, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v |= static_cast<std::uint64_t>(view[offset + i]) << (8U * i);
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

    // header_bytes 即识别字段头长；帧至少要有完整识别字段。
    const std::size_t field = frame_layout.header_bytes;
    if (dto.size() < field) {
        ThrowMalformed("DTO 帧短于识别字段头");
    }

    DtoIdentity identity;
    switch (frame_layout.identification_field_type) {
        case IdentificationFieldType::Absolute: {
            // byte0 = 绝对 ODT 号；daq_list 由调用方（运行态哪条在跑）路由补充。
            identity.odt = dto[0];
            identity.daq_list = 0;
            break;
        }
        case IdentificationFieldType::RelativeByte: {
            // XCPlite：[relODT][0xAA][DAQ16 LE]，header_bytes=4。
            if (field < 4U) {
                ThrowMalformed("RelativeByte 识别字段不足 4 字节");
            }
            if (dto[1] != kXcpliteRelativeMarker) {
                ThrowMalformed("RelativeByte 识别字段缺少 0xAA 标志");
            }
            identity.odt =
                static_cast<std::uint8_t>(frame_layout.first_odt + dto[0]);
            identity.daq_list = static_cast<std::uint16_t>(ReadLe(dto, 2, 2));
            break;
        }
        case IdentificationFieldType::RelativeWord:
        case IdentificationFieldType::RelativeWordAligned: {
            // WORD：低字节 DAQ 号、高字节相对 ODT（Intel 文档口径）。
            if (field < 2U) {
                ThrowMalformed("RelativeWord 识别字段不足 2 字节");
            }
            identity.odt =
                static_cast<std::uint8_t>(frame_layout.first_odt + dto[1]);
            identity.daq_list = dto[0];
            break;
        }
    }

    std::size_t off = field;  // 当前指到 counter/timestamp 段起点

    // counter：使能时紧跟 1 字节。
    std::optional<std::uint8_t> counter;
    if (frame_layout.counter_enabled) {
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
        raw_timestamp = ReadLe(dto, off, ts_bytes);
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