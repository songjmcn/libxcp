/**
 * @file tcp_stream_decoder.cpp
 * @brief TCP 字节流 XCP Frame 解码器实现（双 Wire Profile）。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 4.2 节实现。
 *
 * @par 核心设计：绝对游标 + 已交付区间，保证 Frame 视图稳定
 *   `buffer_` 是接收缓冲，`cursor_` 是解析读写游标（相对 `buffer_` 起点的
 *   偏移），`delivered_begin_` 是本轮已交付数据的起点。交付的 Frame 视图
 *   指向 `buffer_` 中**已写入且不再改写**的区间，因此只要不触发 vector
 *   重新分配或 CompactBuffer()，视图就一直有效。
 *
 *   只有三种情况会搬动字节：CompactBuffer()（仅在调用方已消费完上一轮、
 *   且下一轮 Push 开始时执行）与 `Reset()`。这与头文件中 @warning 的约定
 *   一致：调用方在收到结果后、下一次投喂前消费完视图。
 */

#include "libxcp/tcp_stream_decoder.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace calmcar::xcp {

TcpStreamDecoder::TcpStreamDecoder(TcpWireProfile profile,
                                   PacketAlignment alignment,
                                   std::size_t max_packet_size) {
    // Profile 必须显式选定：禁止 Unknown 进入 decoder，杜绝自动推断
    if (!IsTcpWireProfileResolved(profile)) {
        throw detail::MakeInvalidArgument(
            "TcpStreamDecoder 需要显式的 Wire Profile："
            "Unknown 被拒绝，本库不做任何自动推断");
    }
    // 对齐必须已知：Unknown 无法推出 AsamV15 的 Tail 长度
    if (alignment == PacketAlignment::Unknown) {
        throw detail::MakeInvalidArgument(
            "TcpStreamDecoder 需要已知的 PACKET_ALIGNMENT："
            "Unknown 被拒绝");
    }
    // 上限必须落在 LEN 可表示范围内且非零
    if (max_packet_size == 0U) {
        throw detail::MakeInvalidArgument(
            "TcpStreamDecoder 的 max_packet_size 不得为 0");
    }
    if (max_packet_size > kEthernetMaxXcpPacket) {
        throw detail::MakeInvalidArgument(
            "TcpStreamDecoder 的 max_packet_size 超出 LEN 字段可表示范围 " +
            std::to_string(kEthernetMaxXcpPacket) +
            " 字节: " + std::to_string(max_packet_size));
    }

    profile_ = profile;
    alignment_ = alignment;
    max_packet_size_ = max_packet_size;

    // 预留一个普通帧的空间，减少稳态下的重新分配（重新分配会搬动 buffer_，
    // 但那只发生在没有未消费视图的时刻，见文件头说明）。
    buffer_.reserve(kEthernetHeaderSize + max_packet_size_);
}

std::uint8_t TcpStreamDecoder::InterFramePaddingFor(
    std::size_t payload_len) const noexcept {
    // AsamV14：每帧严格 [LEN][CTR][LEN bytes]，**绝不存在** LEN 外 Tail。
    // 即使 alignment 为 32 也返回 0——这正是该 Profile 的 wire 契约。
    if (profile_ == TcpWireProfile::AsamV14) {
        return 0;
    }
    // AsamV15：Tail 不计入 LEN，长度由 "Header + LEN" 与 alignment 推出
    return TailBytesForAlignment(alignment_,
                                 kEthernetHeaderSize + payload_len);
}

void TcpStreamDecoder::Fail(std::string reason) {
    malformed_ = true;
    stage_ = Stage::Failed;
    if (malformed_reason_.empty()) {
        malformed_reason_ = std::move(reason);
    }
}

void TcpStreamDecoder::Reset() {
    buffer_.clear();
    cursor_ = 0;
    delivered_begin_ = 0;
    stage_ = Stage::NeedHeader;
    malformed_ = false;
    malformed_reason_.clear();
    current_len_ = 0;
    current_ctr_ = 0;
    // 注意：delivered_frames_ 是累计诊断计数，Reset 不清零——它用于验证
    // "整个 decoder 生命周期内不丢帧/不重复"，跨重连累计更有意义。
}

void TcpStreamDecoder::CompactBuffer() {
    // 丢弃已交付的字节，把**尚未消费**的字节（cursor_ 之后的）前移。
    //
    // 关键：不能因为 delivered_begin_ >= cursor_ 就清空整个缓冲！
    // delivered_begin_ 只标记"已交付数据的结束"，而 cursor_ 之后可能仍有
    // 属于**后续帧**却尚未被消费的字节（例如 AsamV15 的待消费 Tail，或
    // 已到达但还不够组成下一帧 Header 的字节）。清空缓冲会把这些字节一并
    // 丢掉，造成静默失步——这是本类必须避免的最严重缺陷。
    if (delivered_begin_ == 0U) {
        // 本轮没有交付任何字节，无需整理
        return;
    }

    // 把 [cursor_, end) 的未消费字节整体前移。这些字节尚未交付给调用方，
    // 因此不持有指向它们的视图，搬动安全。
    const auto remaining_begin =
        buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_);
    buffer_.erase(buffer_.begin(), remaining_begin);

    // 游标与已交付起点同步左移到新的缓冲原点。已交付区间被丢弃后，
    // 游标即指向"已消费数据的末尾 = 未消费数据的起点"，两者归零。
    cursor_ -= delivered_begin_;
    delivered_begin_ = 0;
}

std::vector<EthernetFrameView> TcpStreamDecoder::DrainAvailableFrames() {
    std::vector<EthernetFrameView> produced;

    while (true) {
        // 已停机：不再推进任何字节
        if (stage_ == Stage::Failed) {
            return produced;
        }
        const std::size_t available = buffer_.size() - cursor_;

        switch (stage_) {
            case Stage::NeedHeader: {
                // 不足 4 字节 Header：等下一段数据（TCP 分片的正常情形）
                if (available < kEthernetHeaderSize) {
                    return produced;
                }
                const auto* p = buffer_.data() + cursor_;
                const auto len =
                    static_cast<std::uint16_t>(p[0]) |
                    (static_cast<std::uint16_t>(p[1]) << 8);
                const auto ctr =
                    static_cast<std::uint16_t>(p[2]) |
                    (static_cast<std::uint16_t>(p[3]) << 8);

                // LEN == 0：不构成合法 XCP 消息（每个 Frame 至少 1 字节 PID）。
                // 这属于流已失步/对端违规，无法确定下一边界 -> 不可恢复畸形。
                if (len == 0U) {
                    Fail("TCP 流中出现 LEN=0 的 Frame：每个 Frame 至少携带 "
                         "1 字节 PID");
                    return produced;
                }
                // LEN 超过配置上限：先校验再扩容，防止失步导致无界增长
                if (static_cast<std::size_t>(len) > max_packet_size_) {
                    Fail("TCP 流中 Frame 的 LEN " + std::to_string(len) +
                         " 超过配置上限 " +
                         std::to_string(max_packet_size_));
                    return produced;
                }

                current_len_ = static_cast<std::size_t>(len);
                current_ctr_ = static_cast<DatagramCtr>(ctr);
                // Header 已确认合法，推进并转入 Payload 累计
                cursor_ += kEthernetHeaderSize;
                stage_ = Stage::NeedPayload;
                break;
            }

            case Stage::NeedPayload: {
                // Payload 未满：等下一段数据
                if (available < current_len_) {
                    return produced;
                }

                // Payload 已完整，先记录视图（指向稳定区间）
                EthernetFrameView view;
                view.header.len = static_cast<DatagramLen>(current_len_);
                view.header.ctr = current_ctr_;
                const std::size_t payload_offset = cursor_;
                view.xcp_packet =
                    BytesView{buffer_}.subspan(payload_offset, current_len_);

                const std::uint8_t tail = InterFramePaddingFor(current_len_);
                view.tail_bytes = tail;
                produced.push_back(view);

                cursor_ += current_len_;
                delivered_begin_ = cursor_;

                if (tail == 0U) {
                    // 无 Tail（AsamV14 恒如此；AsamV15 的 8-bit 对齐亦然）
                    stage_ = Stage::NeedHeader;
                    break;
                }

                // AsamV15 且需要 Tail：转入 Tail 消费阶段
                stage_ = Stage::NeedInterFramePadding;
                // 注意：此处**不**立即交付，DAV 视图已入 produced；
                // Tail 的消费在下一轮循环/下次 Push 中推进。
                // 若当前恰好没有更多数据，本帧先交付，Tail 留待后续或 FIN
                // 判定为"合法省略"（TL V1.5 §4.3.2.1 "The tail is optional"）。
                if (buffer_.size() - cursor_ == 0U) {
                    return produced;
                }
                break;
            }

            case Stage::NeedInterFramePadding: {
                const std::uint8_t tail = InterFramePaddingFor(current_len_);
                if (available < tail) {
                    // Tail 只到一半且流还在继续：无法确定后续边界，
                    // 只能在 Finish() 时判定。此时先返回已产出的帧。
                    return produced;
                }
                // 完整消费 Tail：内容不解释（禁止以"值为 0"识别 Tail）
                cursor_ += tail;
                delivered_begin_ = cursor_;
                stage_ = Stage::NeedHeader;
                break;
            }

            case Stage::Failed:
                return produced;
        }
    }
}

TcpDecodeResult TcpStreamDecoder::Push(BytesView bytes) {
    TcpDecodeResult result;

    // 已停机：恒定返回同一个失败结果，不再吞入任何数据
    if (malformed_) {
        result.malformed = true;
        result.malformed_reason = malformed_reason_;
        return result;
    }

    // 进入新一轮：上一轮交付的视图按约定已被调用方消费完，可以整理缓冲。
    // 这是 CompactBuffer() 的唯一调用点，保证不会搬动尚未消费的字节。
    if (delivered_begin_ > 0U && cursor_ >= delivered_begin_) {
        CompactBuffer();
    }

    if (!bytes.empty()) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }

    result.frames = DrainAvailableFrames();
    delivered_frames_ += result.frames.size();

    if (malformed_) {
        result.malformed = true;
        result.malformed_reason = malformed_reason_;
    }
    return result;
}

TcpDecodeResult TcpStreamDecoder::Finish() {
    TcpDecodeResult result;

    if (malformed_) {
        result.malformed = true;
        result.malformed_reason = malformed_reason_;
        result.end_of_stream = true;
        return result;
    }

    // 先把缓冲中能确定的帧尽量解出
    result.frames = DrainAvailableFrames();
    delivered_frames_ += result.frames.size();

    if (malformed_) {
        result.malformed = true;
        result.malformed_reason = malformed_reason_;
        result.end_of_stream = true;
        return result;
    }

    // 收尾校验：按计划 §1.3 第 4 条判定 EOF 合法性。
    // 此时 DrainAvailableFrames 已无法再推进，剩余字节即"未凑齐的部分"。
    const std::size_t leftover = buffer_.size() - cursor_;
    if (leftover != 0U) {
        switch (stage_) {
            case Stage::NeedHeader:
                // 半 Header：truncated frame
                Fail("TCP 流结束时残留 " + std::to_string(leftover) +
                     " 字节不完整 Header：截断帧");
                break;
            case Stage::NeedPayload:
                // 半 Payload：truncated frame
                Fail("TCP 流结束时 Payload 不完整：已收到 " +
                     std::to_string(leftover) + " 字节，期望 " +
                     std::to_string(current_len_) + " 字节");
                break;
            case Stage::NeedInterFramePadding: {
                // AsamV15：Payload 已完整，剩余的是可选末尾 Tail。
                // 收到 1..N-1 字节部分 Tail -> truncated frame；
                // 收到 0 字节 -> 合法（规范允许省略末尾 Tail）。
                const std::uint8_t tail = InterFramePaddingFor(current_len_);
                Fail("TCP 流结束时 Tail 不完整：已收到 " +
                     std::to_string(leftover) + " 字节，期望 " +
                     std::to_string(tail) + " 字节");
                break;
            }
            case Stage::Failed:
                break;
        }
    }
    // 注意：leftover == 0 且处于 NeedInterFramePadding 时，意味着 Tail 完全
    // 未被收到或恰好为 0——规范允许省略末尾 Tail，故视为合法。Payload 完整
    // 且尚未收到任何 Tail 的帧已在 NeedPayload 阶段交付，此处无需补交。

    if (malformed_) {
        result.malformed = true;
        result.malformed_reason = malformed_reason_;
    }
    result.end_of_stream = true;
    return result;
}

}  // namespace calmcar::xcp