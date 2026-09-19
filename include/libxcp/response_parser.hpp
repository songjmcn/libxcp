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
    std::uint8_t rawErrorCode{0};
    /// @brief 已识别的错误码；未知厂商码为 std::nullopt
    std::optional<ErrorCode> errorCode;
    /// @brief 可选附加信息（Byte 2..）
    Bytes additionalInfo;
};

/// @brief Event Packet 内容
struct EventPacket {
    /// @brief 事件码原始字节（保留未知值）
    std::uint8_t rawEventCode{0};
    /// @brief 已识别的事件码；未知值为 std::nullopt
    std::optional<EventCode> eventCode;
    /// @brief 可选 Event 信息（Byte 2..）
    Bytes info;
};

/// @brief Service Request Packet 内容
struct ServicePacket {
    std::uint8_t serviceCode{0};  ///< SERV Packet 的 Byte 1
    Bytes data;                   ///< 可选 Service 数据
};

/// @brief DTO Packet（本阶段仅识别，不解析内容）
struct DtoPacket {
    std::uint8_t pid{0};  ///< 原始 PID（0x00..0xFB）
    Bytes data;           ///< DTO 数据（PID 之后的全部字节）
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
    [[nodiscard]] ByteOrder byteOrder() const noexcept;

    /**
     * @brief 解析一个完整的 XCP Packet
     * @param packet 完整 XCP Packet 字节（不含 Transport Header）
     * @param expected_command 调用方期望的命令码（用于
     * PositiveResponse.command）
     * @return 解析结果；空包等畸形输入返回 std::nullopt
     */
    [[nodiscard]] std::optional<ParsedPacket> parse(
        BytesView packet, CommandCode expected_command) const;

    // ---- 专用解析方法（入参均为去掉 0xFF 前缀后的 RES 数据）----

    /**
     * @brief 解析 CONNECT 响应
     * @param res_data RES 后的数据（长度必须 >= 7）
     * @return 解析结果；长度不足或 AG 位域为保留值 11 时返回 std::nullopt
     */
    [[nodiscard]] std::optional<ConnectResponse> parseConnectResponse(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_STATUS 响应
     * @param res_data RES 后的数据（长度必须 >= 5）
     */
    [[nodiscard]] std::optional<GetStatusResponse> parseGetStatusResponse(
        BytesView res_data) const;

    /**
     * @brief 解析 GET_COMM_MODE_INFO 响应
     * @param res_data RES 后的数据（长度必须 >= 7）
     */
    [[nodiscard]] std::optional<GetCommModeInfoResponse>
    parseGetCommModeInfoResponse(BytesView res_data) const;

private:
    ByteOrder byte_order_;  ///< Session 字节序

    /// @brief 按字节序从 data[offset] 起读取 16 位值；越界返回 std::nullopt
    [[nodiscard]] std::optional<std::uint16_t> readU16(
        BytesView data, std::size_t offset) const;

    /// @brief 按字节序从 data[offset] 起安全读取单字节；越界返回 std::nullopt
    [[nodiscard]] static std::optional<std::uint8_t> readU8(
        BytesView data, std::size_t offset) noexcept;
};

}  // namespace calmcar::xcp

#endif  // CALMCAR_XCP_RESPONSE_PARSER_HPP_
