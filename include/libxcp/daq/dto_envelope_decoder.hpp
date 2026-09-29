/**
 * @file dto_envelope_decoder.hpp
 * @brief 统一 DTO Envelope Decoder：按运行时布局把原始 DTO 帧切出信封。
 *
 * 依据 code-plan/libxcp_测量子系统代码增长计划.md §5.1 / §6（v0.2）。
 * 这是测量子系统的**统一入口**：把 Absolute / RelativeByte / RelativeWord /
 * RelativeWordAligned 四种识别模式收敛为一个 A2L-free 内核解码器，输出统一的
 * DtoEnvelope{identity, counter?, raw_timestamp?, payload}。
 *
 * 解码器是纯函数式：不取证、不访问 XcpMaster，布局由调用方经 DtoFrameLayout 传入。
 * 它与既有手工拆包（tests/xcplite_daq_test.cpp 的 SplitEnvelope）等价且是其后继，
 * 后者在 v0.9 删除。
 */

#ifndef CALMCAR_XCP_DAQ_DTO_ENVELOPE_DECODER_HPP_
#define CALMCAR_XCP_DAQ_DTO_ENVELOPE_DECODER_HPP_

#include <cstdint>

#include "libxcp/daq/dto_envelope_types.hpp"  // DtoEnvelope / DtoFrameLayout / IdentificationFieldType
#include "libxcp/protocol_types.hpp"          // BytesView / ByteOrder

namespace calmcar::xcp {

/**
 * @brief 统一 DTO 信封解码器
 *
 * 用法：
 * @code
 * DtoEnvelopeDecoder decoder;
 * DtoEnvelope env = decoder.Decode(dto_frame, frame_layout);
 * // env.identity.daq_list / odt 定位到账本；env.payload 是净荷段
 * @endcode
 *
 * 解析语义（对照 XCPlite 实然 + XCP 1.3.0 docs L2452… 区域）：
 *  - Absolute：byte0 = 绝对 ODT 号（PID），净荷自 byte1；
 *  - RelativeByte：byte0 = 相对 ODT 号，byte1 = DAQ 扩展字节，净荷自 byte2；
 *  - RelativeWord / RelativeWordAligned：低字节 DAQ 号、高字节相对 ODT（Intel），
 *    净荷自 byte2；Aligned 模式要求帧长相对扩展表 4 字节对齐。
 *
 * 身份换算：
 *  - 相对模式把（相对 ODT + DtoFrameLayout::first_odt）还原为绝对 ODT 号存入
 *    identity.odt；Absolute 模式 byte0 即绝对 ODT 号。
 *  - identity.daq_list：Relative* 模式取自扩展字节/低字节；Absolute 模式由调用方
 *    （运行态哪条 DAQ List 在跑）在路由阶段补充，本解码器置 0。
 *
 * 可选尾段（按 DtoFrameLayout 使能位）：
 *  - counter_enabled：识别字段后紧跟 1 字节 DTO 计数器；
 *  - timestamp_enabled：再紧跟 timestamp_size_bits/8 字节原始时间戳（小端）；
 *  - pid_off：禁止（B-7），抛 XcpException(MalformedPacket)。
 *
 * 任何字段越界/不足 → 抛 XcpException(MalformedPacket)；不允许部分解释。
 * 结构性异常由调用方构造并传入（见 DtoFrameLayout 注释），本类不取证。
 */
class DtoEnvelopeDecoder {
public:
    /**
     * @brief 构造解码器
     * @param byte_order 会话字节序（absolute 之外的 WORD 识别读对齐用；
     *                    XCPlite 为 Intel）
     */
    explicit DtoEnvelopeDecoder(ByteOrder byte_order = ByteOrder::Intel) noexcept;

    /**
     * @brief 按运行时布局解码一个 DTO 帧
     * @param dto 完整 DTO 帧（含识别字段头与可选尾段；帧内容只读）
     * @param frame_layout 运行时信封布局（调用方按会话取证构造）
     * @return 统一信封；payload 指向 dto 内部，生命周期不超过 dto
     * @throws XcpException(MalformedPacket) 帧过短 / pid_off / 识别字段不合法
     * @throws XcpException(UnsupportedFeature) timestamp_enabled 且
     *         timestamp_size_bits 非 8/16/32 的倍数整字节（无法切分）
     */
    [[nodiscard]] DtoEnvelope Decode(BytesView dto,
                                     const DtoFrameLayout& frame_layout) const;

private:
    ByteOrder m_byte_order_;  ///< 会话字节序
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_DAQ_DTO_ENVELOPE_DECODER_HPP_