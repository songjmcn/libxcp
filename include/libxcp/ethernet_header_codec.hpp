/**
 * @file ethernet_header_codec.hpp
 * @brief XCP on Ethernet 通用 Transport Header（4 字节 LEN/CTR）与 Tail
 * （PACKET_ALIGNMENT FILL）纯函数编解码，供 UDP 与 TCP 共享。
 *
 * 依据 code-plan/TCP传输_实施计划_B-T1至B-T5.md 第 4.1 节实现。
 *
 * @par 为什么 UDP 与 TCP 共用本文件
 *   依据 TL V1.5 §4.3.1：TCP 与 UDP 的 Frame 布局完全一致
 *   （LEN + CTR + XCP Packet [+ Tail FILL]），差别只在**承载方式**
 *   （数据报 vs 字节流）。因此 Header / LEN / CTR / Tail 算法只有一份实现，
 *   两个 Transport 共用，避免两份近似代码各自漂移。
 *
 * @par 本文件**不承载任何 Stream/Profile 状态**
 *   Wire Profile（AsamV15 / AsamV14）的流式状态机在
 *   `tcp_stream_decoder.hpp` 的 `TcpStreamDecoder` 中。本文件只做无状态的
 *   Header、长度与 Tail 运算，从而可以纯函数单测。
 *
 * @par 规范基线（TL V1.5 原始 PDF：
 *      `docs/ASAM-AE-MCD-1-XCP-AS-Ethernet-Transport-Layer-V1-5-0.pdf`）
 *   - §4.3.1（PDF 第 9 页）：Header 由 LEN 与 CTR 组成，二者恒为 Intel
 *     （小端）WORD；"LEN is the number of bytes in the original XCP Packet."
 *     —— **LEN 不含 Tail**。
 *   - §4.3.2（PDF 第 9 页）："the tail consists of a control field containing
 *     0,1,2 or 3 FILL bytes."
 *   - §4.3.2.1：单个 XCP 消息独占 Frame 时 "The tail is optional."。
 *   - §4.3.2.2（PDF 第 10 页）：多消息串联时，前一消息的 Tail 用于让后继
 *     XCP Packet 起始地址满足 PACKET_ALIGNMENT；FILL 内容 "do not care"。
 *
 * @note 旧基线（XCP 1.1 Part 3）称 "XCP on Ethernet 无 Tail"。该说法在
 *       PACKET_ALIGNMENT_8 下与 V1.5 一致（tail 恒为 0），故
 *       `EncodeUdpFrame()` / `DecodeUdpDatagram()` 保留为 **8-bit 语义**；
 *       需要 16/32-bit 对齐的路径必须显式调用带 `Aligned` 后缀的入口，
 *       不得依赖任何默认推断（方案 §1.9A.4）。
 */

#ifndef CALMCAR_XCP_ETHERNET_HEADER_CODEC_HPP_
#define CALMCAR_XCP_ETHERNET_HEADER_CODEC_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"

namespace calmcar::xcp {

/// @brief XCP on Ethernet Transport Header 固定长度（字节）：LEN(2) + CTR(2)
constexpr std::size_t kEthernetHeaderSize = 4;

/// @brief IPv4 UDP Payload 最大理论长度（65535 - 8 Byte UDP Header - 20 Byte IP
/// Header）
/// @note 该上界**只对 UDP** 成立（IPv4 分片限制）；TCP 是字节流，不受此限，
///       故 TCP 侧使用 `kEthernetMaxXcpPacket`。
constexpr std::size_t kUdpMaxDatagramSize = 65507;

/// @brief UDP 单 Datagram 承载时单个 XCP Packet 的最大理论长度
/// @details 单 Frame 独占 UDP Datagram 时：65507 - 4 Byte Header = 65503。
constexpr std::size_t kUdpMaxXcpPacket = kUdpMaxDatagramSize - kEthernetHeaderSize;

/// @brief 以太网通用层单个 XCP Packet 的长度上限（字节）
/// @details LEN 是 16 位字段，合法域 `1..0xFFFF`。**该上界属于以太网通用层**
///          且与承载介质无关：TCP 没有 IP 分片约束，允许到 65535。
/// @note 不要把它与 UDP 专用的 `kUdpMaxXcpPacket`(65503) 混用：后者是 IPv4
///       Datagram 的物理限制，属于 UDP 传输层而非通用层。
constexpr std::size_t kEthernetMaxXcpPacket = 0xFFFFU;

/// @brief PACKET_ALIGNMENT 的最大字节数（32 bit）
constexpr std::size_t kEthernetMaxAlignmentBytes = 4U;

/// @brief 单个 XCP Frame 的最大总长度（字节）：Header + 最大 Packet + 最大 Tail
/// @details `4 + 0xFFFF + 4 = 65539`。
///
/// @note **本库不把该值当作配置上限**：TCP 的 `max_frame_packet_size` 默认值
///       取 `kEthernetMaxXcpPacket`(65535)，因为那才是"单个 XCP Packet"的
///       语义上界。本常量仅用于接收缓冲的**绝对容灾上界**，防止对端失步时
///       缓冲无界增长。
constexpr std::size_t kEthernetMaxFrameSize =
    kEthernetHeaderSize + kEthernetMaxXcpPacket + kEthernetMaxAlignmentBytes;

/// @brief 编码后的单个 XCP on Ethernet Frame
/// （Header + 一个 XCP Packet [+ Tail FILL]）
struct EthernetFrame {
    Bytes data;  ///< LEN(u16le) + CTR(u16le) + XCP Packet + [FILL...]
    /// @brief 本 Frame 尾部 FILL 字节数（0..3）；8-bit 对齐时恒为 0
    std::uint8_t tail_bytes{0};
};

/// @brief 解码后的以太网 Transport Header 字段
struct EthernetHeader {
    DatagramLen len{};  ///< 原始 XCP Packet 字节数（**不含 Tail**）
    DatagramCtr ctr{};  ///< 该 XCP Frame 的独立计数器
};

/**
 * @brief 承载单元内单个 XCP Frame 的只读视图
 * @note xcp_packet 指向解码入口入参的底层存储，生命周期不超过该视图。
 */
struct EthernetFrameView {
    EthernetHeader header;  ///< Frame 的 Transport Header
    BytesView xcp_packet;   ///< Frame 内的 XCP Packet（不拥有存储）
    /// @brief 本 Frame 尾随的 FILL 字节数（0..3），见 TL V1.5 §4.3.2.2
    std::uint8_t tail_bytes{0};
};

/// @brief 校验一个 XCP Packet 长度是否可编入 16 位 LEN 字段
/// @param len 待检查的 XCP Packet 字节数
/// @throws XcpException(InvalidArgument) `len == 0` 或 `len > 0xFFFF`
/// @details LEN 合法域为 `1..0xFFFF`：0 无意义（每个 Frame 至少携带 1 字节
///          XCP Packet，即 PID），超过 0xFFFF 无法编入 16 位字段。
///
/// @note 用户裁决（m00560）：本函数按计划 §4.1 **拒绝空 Packet**。编码器与
///       解码器在 LEN=0 上的一致性由 B-T1 建立，取代了迁移前"编码宽进、
///       解码严出"的旧行为。
void CheckEthernetPacketLen(std::size_t len);

/**
 * @brief 编码一个 AsamV15 Profile 的 XCP on Ethernet Frame
 *        （按指定 PACKET_ALIGNMENT 补 Tail）
 * @param xcp_packet 原始 XCP Packet（不含 Transport Header）
 * @param ctr 该 Frame 的发送计数器值
 * @param alignment 会话协商到的对齐位宽
 * @return 编码后的 Frame（LEN + CTR + Packet + 0..3 字节 FILL）
 * @throws XcpException(InvalidArgument) `xcp_packet` 为空或
 *         `xcp_packet.size() > 0xFFFF`，或
 *         `alignment == PacketAlignment::Unknown`
 *
 * @details 依据 TL V1.5 §4.3.2.2：Tail 长度取
 *          `TailBytesForAlignment(alignment, kEthernetHeaderSize + packet.size())`，
 *          使**后继** XCP Packet 的起始偏移满足 alignment。FILL 内容规范定为
 *          "do not care"，本库固定填 0x00。
 *
 * @note `Unknown` 被拒绝而非按 8-bit 处理：无证据时不得假定对齐，
 *       否则会静默生成与对端不兼容的报文（方案 §1.9A.4）。
 */
[[nodiscard]] EthernetFrame EncodeEthernetFrameAligned(
    BytesView xcp_packet, DatagramCtr ctr, PacketAlignment alignment);

/**
 * @brief 编码一个 AsamV14 兼容 Profile 的 XCP on Ethernet Frame（**无 Tail**）
 * @param xcp_packet 原始 XCP Packet（不含 Transport Header）
 * @param ctr 该 Frame 的发送计数器值
 * @return 编码后的 Frame（**严格 Header + Packet**，`tail_bytes == 0`）
 * @throws XcpException(InvalidArgument) `xcp_packet` 为空或
 *         `xcp_packet.size() > 0xFFFF`
 *
 * @details AsamV14 契约（方案 §1.4）：Master TX 只发送 `Header + 真实 CRO`，
 *          **绝不附加 LEN 外 Tail**。即使 alignment 为 8（两种 wire 形式
 *          数值上等价），也不得改调带对齐的入口，以免将来 alignment 变化时
 *          静默开始补 Tail。
 */
[[nodiscard]] EthernetFrame EncodeEthernetFrameUnpadded(BytesView xcp_packet,
                                                        DatagramCtr ctr);

/**
 * @brief 以承载单元边界为准，解码一个承载单元中连续打包的全部 XCP Frame
 * @param datagram 完整承载单元 Payload（UDP Datagram）
 * @param max_tail 末帧允许的最大 Tail 字节数（0..3，默认 3）
 * @return 全部 Frame 视图；畸形输入整体返回 `std::nullopt`
 *
 * @details 用于**没有 PACKET_ALIGNMENT 证据**的接收路径：UDP 一个数据报就是
 *          一个承载单元，Frame 边界可直接观测，因此 Tail 长度不必由对齐推出，
 *          而由剩余字节判定：
 *          - 末帧（本数据报最后一个 Frame）：`LEN` 之后剩下的 0..max_tail 字节
 *            一律视为 FILL（规范定为 "do not care"，不读取其内容）；
 *          - 中间帧：要求 Tail 为 0（紧随其后必须能构成下一帧的 4 字节 Header）。
 *
 *          因为 `max_tail <= 3`，剩余字节少于 4 时不可能构成下一帧 Header，
 *          故"不超过 max_tail 的剩余字节"只能解释为 FILL，判定无歧义。
 *
 *          发送侧配套约定：主站发 CRO 时**不补 Tail**（TL V1.5 §4.3.2.1
 *          允许单个 XCP 消息独占 Frame 时省略 Tail），因此不依赖对端预期的
 *          对齐位宽。
 *
 * @par 整体拒绝（返回 std::nullopt）的畸形情形
 *   - `datagram` 为空或剩余字节不足以容纳完整 4 字节 Header；
 *   - `LEN == 0`；
 *   - `Header + LEN` 超出 Datagram 剩余长度（截断）；
 *   - 中间帧之后剩余字节多于 `max_tail`（即对端在中间帧上补了 FILL）。
 *     本函数**不做试探**：同时尝试多种 Tail 长度属于嗅探，会让畸形包被
 *     静默接受，故一律整体拒绝。
 *
 * @warning 返回视图指向 `datagram` 底层存储，生命周期不超过 `datagram`。
 */
[[nodiscard]] std::optional<std::vector<EthernetFrameView>>
DecodeEthernetDatagramByBoundary(BytesView datagram,
                                 std::uint8_t max_tail = 3) noexcept;

/**
 * @brief 解码一个承载单元中连续打包的全部 XCP Frame（AsamV15 语义）
 * @param datagram 完整承载单元 Payload（UDP Datagram）
 * @param alignment 会话协商到的对齐位宽
 * @return 全部 Frame 视图；畸形输入整体返回 `std::nullopt`
 *
 * @details 每个 Frame 在 XCP Packet 之后会跳过
 *          `TailBytesForAlignment(alignment, kEthernetHeaderSize + len)` 个
 *          FILL 字节，再继续解析后继 Frame。该长度完全由 LEN 与 alignment
 *          推出，无需读取 FILL 内容（规范定为 "do not care"）。
 *
 * @par 整体拒绝（返回 std::nullopt）的畸形情形
 *   - `datagram` 为空；
 *   - 剩余字节不足以容纳完整 4 字节 Header；
 *   - `LEN == 0`（规范要求 LEN 为原 XCP Packet 字节数，0 无意义）；
 *   - `Header + LEN + Tail` 超出 Datagram 剩余长度（截断）；
 *   - 跳过 Tail 后仍有无法构成完整 Frame 的残尾；
 *   - `alignment == PacketAlignment::Unknown`。
 *
 * @note 该「整体拒绝」语义只适用于**数据报**输入：Datagram 边界就是 Frame
 *       边界，半截报文必然是畸形包，绝不能对半截 Datagram 交付部分 Frame，
 *       否则上层会把截断报文误当作完整 DAQ 批次。TCP 字节流请改用
 *       `tcp_stream_decoder.hpp` 的 `TcpStreamDecoder`。
 *
 * @warning 返回视图指向 `datagram` 底层存储，生命周期不超过 `datagram`。
 */
[[nodiscard]] std::optional<std::vector<EthernetFrameView>>
DecodeEthernetDatagramAligned(BytesView datagram,
                              PacketAlignment alignment) noexcept;

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_ETHERNET_HEADER_CODEC_HPP_