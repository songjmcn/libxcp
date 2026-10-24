/**
 * @file tcp_stream_decoder.hpp
 * @brief TCP 字节流上的 XCP Frame 流式解码器（双 Wire Profile）。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 4.2 节实现（B-T1 §4.2）。
 *
 * @par 与 UDP 解码的本质差异
 *   UDP 是**数据报**：一个 Datagram 即一次完整的 Frame 集合，半截报文必然是
 *   畸形包，故 `DecodeEthernetDatagramAligned()` 对任何不完整输入**整体拒绝**。
 *
 *   TCP 是**字节流**：`recv()` 的边界与 Frame 边界**毫无关系**。一次 recv 可能
 *   只拿到半个 Header，也可能一次拿到三帧半。因此 TCP 必须用**跨调用保存状态**
 *   的状态机，绝不能沿用 UDP 的"整体拒绝"策略（那会把正常分片误判为畸形）。
 *
 * @par 双 Profile 状态机
 *   - AsamV15：`NeedHeader -> NeedPayload -> NeedInterFramePadding -> NeedHeader`
 *     多出的 `NeedInterFramePadding` 用于消费 PACKET_ALIGNMENT 的 LEN 外 FILL。
 *   - AsamV14：`NeedHeader -> NeedPayload -> NeedHeader`
 *     **绝不进入** Tail 状态：每取得 `4 + LEN` 字节立即回到 `NeedHeader`。
 *
 *   Profile 在构造后**不可变**，且不允许 `Unknown`：无显式配置时构造即失败，
 *   避免任何形式的自动推断。
 *
 * @par 存储与生命周期（关键设计约束）
 *   一次 `Push()` 可返回**零到多个** Frame，且这些 Frame 的存储必须在调用方
 *   持有期间**保持稳定**。因此本类**不**在交付后立即 `erase()` 内部缓冲——
 *   那样会让先前返回的 `BytesView` 悬空。采用「已交付区间前移 + 周期性整理」
 *   策略：
 *   - 交付的 Frame 从内部缓冲的 `[delivered_begin_, total_received_)` 区间
 *     指向**稳定地址**（`std::vector` 只在重新分配时搬家，而整理只在没有
 *     未交付数据时进行）；
 *   - `Reset()` 或新的一次 `Finish()` 之后的 Push 会整理缓冲，此时调用方
 *     **必须**已经不再使用上一轮的 Frame 视图（见各方法 @warning）。
 *
 * @note 本类只做 **wire framing**：不按内容猜 CRM/DTO，不裁剪 AsamV14 的
 *       LEN 内扩长，也不解释 Tail 内容（禁止以"值为 0"识别 Tail）。
 */

#ifndef CALMCAR_XCP_TCP_STREAM_DECODER_HPP_
#define CALMCAR_XCP_TCP_STREAM_DECODER_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "libxcp/ethernet_header_codec.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_wire_profile.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/**
 * @brief 一次 `Push()` / `Finish()` 的解码结果
 *
 * @details 与 UDP 的 `std::optional<std::vector<...>>` 不同：TCP 流上"本次收到的
 *          数据不足以构成完整 Frame"是**完全正常**的中间状态，不是错误。因此
 *          结果用「是否畸形 + 已完整解码的 Frame 列表」表达，而不是用
 *          `nullopt` 表示失败。
 */
struct TcpDecodeResult {
    /// @brief 本次新增的完整 Frame（可能为空：数据还不足以构成完整帧）
    /// @warning 视图指向 decoder 内部缓冲，生命周期见类注释；
    ///          调用方在下一次 `Push()`/`Finish()`/`Reset()` 之前使用有效。
    std::vector<EthernetFrameView> frames;

    /// @brief 是否检测到**不可恢复**的 wire 畸形
    /// @details 为 true 时本次及后续解码已停止，调用方应关闭连接重建。
    ///          这是"流已失步，无法确定下一边界"的表达，绝不做启发式恢复。
    bool malformed{false};

    /// @brief 畸形原因（仅 `malformed == true` 时有值；中文，供日志与异常文本）
    std::string malformed_reason{};

    /// @brief 是否已遇到流结束（`Finish()` 调用后置位）
    bool end_of_stream{false};
};

/**
 * @brief TCP 字节流上的 XCP Frame 解码器
 *
 * @par 线程安全
 *   本类**不是**线程安全的：TCP Transport 的接收线程是唯一所有者。
 *
 * @par 不变量
 *   - 构造后 `profile_` 与 `alignment_` 永不改变；
 *   - 任何扩容前必先校验 LEN 与 `max_packet_size_`，防止对端失步时缓冲无界增长；
 *   - 一旦 `malformed_` 置位，所有后续 `Push()` 都只返回同一个失败结果，不再推进。
 */
class TcpStreamDecoder final {
public:
    /**
     * @brief 构造解码器
     * @param profile Wire Profile（**不得为 `Unknown`**）
     * @param alignment PACKET_ALIGNMENT（AsamV14 下仅记录不参与 framing）
     * @param max_packet_size 单个 XCP Packet 的配置上限（字节）
     * @throws XcpException(InvalidArgument) `profile == Unknown`、
     *         `alignment == Unknown`、`max_packet_size == 0` 或
     *         `max_packet_size > kEthernetMaxXcpPacket`
     *
     * @details 参数在构造期一次性校验并冻结：把配置错误挡在连接建立之前，
     *          而不是等到解析第一帧时才失败。
     *
     * @note `max_packet_size` 允许小于 65535：调用方可用它表达"我的接收缓冲
     *       只准备承受这么大"，超过该值的 LEN 视为**配置性畸形**（fail
     *       closed），而不是静默扩容。
     */
    TcpStreamDecoder(TcpWireProfile profile, PacketAlignment alignment,
                     std::size_t max_packet_size);

    /// @brief 析构（内部缓冲随对象释放）
    ~TcpStreamDecoder() = default;

    /// @brief 禁止拷贝：解码器持有流式状态与稳定存储，不构成值语义
    TcpStreamDecoder(const TcpStreamDecoder&) = delete;

    /// @brief 禁止拷贝赋值
    TcpStreamDecoder& operator=(const TcpStreamDecoder&) = delete;

    /// @brief 允许移动：便于把已配置好的 decoder 移交接收线程
    TcpStreamDecoder(TcpStreamDecoder&&) noexcept = default;

    /// @brief 允许移动赋值
    TcpStreamDecoder& operator=(TcpStreamDecoder&&) noexcept = default;

    /**
     * @brief 投喂一段新收到的字节，推进状态机
     * @param bytes 本次 `recv()` 得到的字节（可为空，返回空结果）
     * @return 本次完整解出的 Frame 列表与畸形标志
     *
     * @details 一次调用可返回**零到多个** Frame：
     *   - 零个：数据仍不足以构成完整帧（TCP 分片的正常情形）；
     *   - 多个：本次收到的字节包含多个粘包帧。
     *
     * @par AsamV15 的末尾 Tail 处理
     *   状态机在 Payload 完整后进入 `NeedInterFramePadding`。若此时**没有**
     *   后续数据，则本帧可以先交付、把 Tail 留到下次（或由 `Finish()`
     *   判定为合法省略）——这对应规范 "The tail is optional"。
     *   一旦下一个 Header 已经到达，则 Tail 必须完整存在，否则畸形。
     *
     * @warning 返回的 `frames` 引用内部缓冲：在下一次 `Push()`、`Finish()`
     *          或 `Reset()` 之前使用有效。调用方必须先消费完再继续投喂。
     */
    [[nodiscard]] TcpDecodeResult Push(BytesView bytes);

    /**
     * @brief 流结束（FIN）时的收尾校验
     * @return 剩余可交付的 Frame，以及是否因截断而畸形
     *
     * @details EOF 合法性判定（计划 §1.3 第 4 条）：
     *   - Payload 完整、且尚未收到任何可选末尾 Tail -> 合法；
     *   - 收到 `1..N-1` 字节部分 Tail -> truncated frame，畸形；
     *   - 半 Header 或半 Payload -> truncated frame，畸形。
     *
     * @note 本方法**不**重置状态：若需复用请随后调用 `Reset()`。
     * @warning 返回的 `frames` 引用内部缓冲，`Reset()` 后失效。
     */
    [[nodiscard]] TcpDecodeResult Finish();

    /**
     * @brief 丢弃全部流式状态与缓冲，回到初始 `NeedHeader`
     *
     * @details 用于 TCP 重连：新连接的首个 Header 重新成为 alignment origin。
     *          **不**改变 Profile/alignment/上限（它们是构造期冻结的）。
     *
     * @warning 调用后，先前的所有 Frame 视图**立即失效**。
     */
    void Reset();

    /// @brief 读取构造时冻结的 Wire Profile
    [[nodiscard]] TcpWireProfile Profile() const noexcept { return profile_; }

    /// @brief 读取构造时冻结的 PACKET_ALIGNMENT
    [[nodiscard]] PacketAlignment Alignment() const noexcept {
        return alignment_;
    }

    /// @brief 读取构造时冻结的单 Packet 上限
    [[nodiscard]] std::size_t MaxPacketSize() const noexcept {
        return max_packet_size_;
    }

    /// @brief 是否已进入不可恢复的畸形状态
    [[nodiscard]] bool IsMalformed() const noexcept { return malformed_; }

    /// @brief 累计已交付的 Frame 数量（诊断用，用于验证不丢帧/不重复）
    [[nodiscard]] std::size_t DeliveredFrameCount() const noexcept {
        return delivered_frames_;
    }

private:
    /// @brief AsamV15 状态机的当前阶段
    enum class Stage : std::uint8_t {
        /// @brief 正在累计 4 字节 Transport Header
        NeedHeader,

        /// @brief 已解出 LEN，正在累计 LEN 字节 Payload
        NeedPayload,

        /// @brief AsamV15：Payload 已满，正在等待/消费 LEN 外 Tail
        NeedInterFramePadding,

        /// @brief 已因不可恢复畸形而停机
        Failed,
    };

    /// @brief 把当前缓冲中可确定的内容尽量推进，产出完整 Frame
    /// @return 本次新增的 Frame 视图
    std::vector<EthernetFrameView> DrainAvailableFrames();

    /// @brief 依据当前 Profile 与 `4 + LEN` 推出生效的 Tail 字节数
    /// @param payload_len 当前帧的 XCP Packet 长度
    /// @return AsamV14 恒为 0；AsamV15 按 alignment 计算
    [[nodiscard]] std::uint8_t InterFramePaddingFor(
        std::size_t payload_len) const noexcept;

    /// @brief 置位不可恢复畸形状态并记录原因
    /// @param reason 中文原因描述
    void Fail(std::string reason);

    /// @brief 紧凑化缓冲：丢弃已交付区间并前移未交付字节
    /// @warning 仅在调用方已不再持有旧 Frame 视图时调用
    void CompactBuffer();

    /// @brief 构造期冻结的 Wire Profile
    TcpWireProfile profile_{TcpWireProfile::Unknown};

    /// @brief 构造期冻结的 PACKET_ALIGNMENT
    PacketAlignment alignment_{PacketAlignment::Unknown};

    /// @brief 构造期冻结的单 XCP Packet 上限（字节）
    std::size_t max_packet_size_{0};

    /// @brief 当前状态机阶段
    Stage stage_{Stage::NeedHeader};

    /// @brief 是否已进入不可恢复畸形状态
    bool malformed_{false};

    /// @brief 畸形原因（中文）
    std::string malformed_reason_{};

    /// @brief 累计已交付 Frame 数
    std::size_t delivered_frames_{0};

    /// @brief 接收缓冲（保存未消费的字节）
    Bytes buffer_{};

    /// @brief 解析游标：`buffer_` 中已经推进到的绝对读取位置
    std::size_t cursor_{0};

    /// @brief 已交付但尚未整理掉的数据起点（供 Frame 视图稳定指向）
    std::size_t delivered_begin_{0};

    /// @brief 当前帧已解出的 LEN（`NeedPayload`/`NeedInterFramePadding` 阶段有效）
    std::size_t current_len_{0};

    /// @brief 当前帧已解出的 CTR
    DatagramCtr current_ctr_{0};
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_TCP_STREAM_DECODER_HPP_