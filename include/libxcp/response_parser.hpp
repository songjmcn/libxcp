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

/// @brief DTO Packet（本阶段仅识别，不解析内容）
struct DtoPacket {
    std::uint8_t pid{0};  ///< 原始 PID（0x00..0xFB）
    Bytes data;           ///< DTO 数据（PID 之后的全部字节）
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

private:
    /// @brief CONNECT 协商出的 Session 字节序，决定多字节字段的读取方向
    ByteOrder m_byte_order_;

    /// @brief 按 Session 字节序读取 16 位字段
    /// @param data 响应数据
    /// @param offset 字段起始偏移
    /// @return 取值；越界时返回 std::nullopt（由调用方判为畸形包）
    [[nodiscard]] std::optional<std::uint16_t> ReadU16(
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
