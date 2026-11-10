/**
 * @file calibration_block_test.cpp
 * @brief B1 标定闭环测试：块传输写入、写后校验、持久化请求。
 *
 * 覆盖计划文档 §3 B1 批次的三个交付点：
 *   1. DOWNLOAD 块序列（首帧报整块元素数、续帧报剩余数、只有末帧应答）
 *   2. MemoryAccess::VerifyBytes（Slave 校验和优先、读回后备、不可复现转读回）
 *   3. MemoryAccess::RequestCalibrationStore（Completed / Pending / Unknown /
 *      不支持时抛 UnsupportedFeature）
 *
 * 用 MockTransport + 脚本化 FakeSlave，不依赖真实 Socket。
 */

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/checksum.hpp"
#include "libxcp/command_codec.hpp"
#include "libxcp/command_executor.hpp"
#include "libxcp/memory_access.hpp"
#include "libxcp/response_parser.hpp"
#include "libxcp/session.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 组装 CONNECT 响应（RES + 7 字节负载）
/// @param block_mode 是否声明 SLAVE_BLOCK_MODE（COMM_MODE_BASIC bit6）
Bytes ConnectResponse(AddressGranularity ag, ByteOrder order,
                      std::uint8_t max_cto, std::uint16_t max_dto,
                      bool block_mode) {
    std::uint8_t comm_mode = 0x00U;
    if (order == ByteOrder::Motorola) {
        comm_mode |= 0x01U;
    }
    comm_mode |= static_cast<std::uint8_t>(AgToCommModeBasicField(ag) << 1);
    if (block_mode) {
        comm_mode |= 0x40U;  // SLAVE_BLOCK_MODE
    }
    comm_mode |= 0x80U;  // OPTIONAL：允许后续可选命令
    Bytes out{static_cast<std::uint8_t>(PacketType::Res), 0x15U, comm_mode,
              max_cto};
    if (order == ByteOrder::Intel) {
        out.push_back(static_cast<std::uint8_t>(max_dto & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((max_dto >> 8) & 0xFFU));
    } else {
        out.push_back(static_cast<std::uint8_t>((max_dto >> 8) & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(max_dto & 0xFFU));
    }
    out.push_back(0x10U);  // Protocol Layer Version
    out.push_back(0x10U);  // Transport Layer Version
    return out;
}

/// @brief GET_COMM_MODE_INFO 响应：MAX_BS 位于报文 byte4（解析器读 res_data[3]）
/// @param max_bs 从站可接受的最多块元素数
/// @param master_block_mode true 表示 COMM_MODE_OPTIONAL(bit0) 置位，即从站
///        能在 DAQ/STIM 之外接受 Master→Slave 的 DOWNLOAD 块序列
/// @note COMM_MODE_OPTIONAL 解析为报告的 byte2（res_data[1]）；其值 0x0E
///       表示 bit1/bit2/bit3 置位，若执行 `| 0x01` 会得到 0x0F，不再是 0x0E，
///       所以这里按位**重建**而非在 0x0E 上叠加，保证两种取值的位含义都正确。
Bytes CommModeInfoResponse(std::uint8_t max_bs,
                           bool master_block_mode = true) {
    const std::uint8_t optional = master_block_mode ? 0x0FU : 0x0EU;
    return Bytes{static_cast<std::uint8_t>(PacketType::Res), 0x00U, optional,
                 0x00U, max_bs, 0x02U, 0x08U, 0x13U};
}

/// @brief GET_STATUS 响应，可控制 STORE_CAL_REQ 位
/// @details 解析器把 res_data[0]（即报文 byte1）当 CURRENT_SESSION_STATUS，
///          STORE_CAL_REQ 在 bit0，故请求位必须写在 RES 之后的第一个字节。
Bytes GetStatusResponse(bool store_cal_req) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Res),
                 static_cast<std::uint8_t>(store_cal_req ? 0x01U : 0x00U),
                 0x00U, 0x00U, 0x01U, 0x00U};
}

Bytes Err(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

/// @brief 从请求报文中按 Session 字节序解出 32 位地址（偏移 4）
Address ParseAddressAt4(BytesView packet, ByteOrder order) {
    if (order == ByteOrder::Intel) {
        return static_cast<Address>(packet[4]) |
               (static_cast<Address>(packet[5]) << 8) |
               (static_cast<Address>(packet[6]) << 16) |
               (static_cast<Address>(packet[7]) << 24);
    }
    return static_cast<Address>(packet[7]) |
           (static_cast<Address>(packet[6]) << 8) |
           (static_cast<Address>(packet[5]) << 16) |
           (static_cast<Address>(packet[4]) << 24);
}

/// @brief 从请求报文中解出 32 位字段（偏移 4，BUILD_CHECKSUM 的 Block size）
std::uint32_t ParseU32At4(BytesView packet, ByteOrder order) {
    if (order == ByteOrder::Intel) {
        return static_cast<std::uint32_t>(packet[4]) |
               (static_cast<std::uint32_t>(packet[5]) << 8) |
               (static_cast<std::uint32_t>(packet[6]) << 16) |
               (static_cast<std::uint32_t>(packet[7]) << 24);
    }
    return static_cast<std::uint32_t>(packet[7]) |
           (static_cast<std::uint32_t>(packet[6]) << 8) |
           (static_cast<std::uint32_t>(packet[5]) << 16) |
           (static_cast<std::uint32_t>(packet[4]) << 24);
}

/**
 * @brief 支持块传输 / BUILD_CHECKSUM / SET_REQUEST 的脚本化 Slave
 */
class BlockSlave {
public:
    BlockSlave(AddressGranularity ag, ByteOrder order, std::uint8_t max_cto,
               std::uint8_t max_bs, bool block_mode,
               bool master_block_mode = true)
        : m_ag_(ag),
          m_order_(order),
          m_max_cto_(max_cto),
          m_max_bs_(max_bs),
          m_block_mode_(block_mode),
          m_master_block_mode_(master_block_mode) {}

    void SetMemory(Address address, const Bytes& data) {
        for (std::size_t k = 0; k < data.size(); ++k) {
            m_memory_[address + static_cast<Address>(k)] = data[k];
        }
    }

    Bytes Peek(Address address, std::size_t bytes) const {
        Bytes out(bytes, 0x00U);
        for (std::size_t k = 0; k < bytes; ++k) {
            const auto it = m_memory_.find(address + static_cast<Address>(k));
            if (it != m_memory_.end()) {
                out[k] = it->second;
            }
        }
        return out;
    }

    /// @brief 令 BUILD_CHECKSUM 回 ERR_CMD_UNKNOWN
    void RejectBuildChecksum() { m_reject_build_checksum_ = true; }
    /// @brief 令 BUILD_CHECKSUM 回报指定类型，但校验和值可覆盖
    void SetChecksum(std::optional<std::uint8_t> type_override,
                     std::optional<std::uint32_t> value_override) {
        m_checksum_type_override_ = type_override;
        m_checksum_value_override_ = value_override;
    }
    /// @brief 令 SET_REQUEST 回 ERR_CMD_UNKNOWN
    void RejectSetRequest() { m_reject_set_request_ = true; }
    /// @brief 收到 SET_REQUEST 后，第 n 次 GET_STATUS 起 STORE_CAL_REQ 清除
    ///        （n=0 表示立即清除；n<0 表示永不清除）
    void SetStoreCalReqClearsAfter(int polls) { m_store_clears_after_ = polls; }

    /// @brief 块内中间帧若不应答，这里能观察到的"是否回了包"
    [[nodiscard]] int count(CommandCode code) const {
        const auto it = m_counts_.find(code);
        return it == m_counts_.end() ? 0 : it->second;
    }

    /// @brief 最近一次收到命令的 size 字段（DOWNLOAD / DOWNLOAD_NEXT）
    [[nodiscard]] std::vector<std::uint8_t> SizeFields() const {
        return m_size_fields_;
    }

    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];

        switch (cmd) {
            case CommandCode::Connect:
                return ConnectResponse(m_ag_, m_order_, m_max_cto_, 0x0010U,
                                       m_block_mode_);
            case CommandCode::GetCommModeInfo:
                return CommModeInfoResponse(m_max_bs_, m_master_block_mode_);
            case CommandCode::GetStatus:
                return handleGetStatus();
            case CommandCode::Synch:
                return Err(ErrorCode::CmdSynch);
            case CommandCode::SetMta:
                m_mta_ = ParseAddressAt4(packet, m_order_);
                return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
            case CommandCode::Download:
                return handleDownload(packet);
            case CommandCode::DownloadNext:
                return handleDownloadNext(packet);
            case CommandCode::ShortDownload:
                // 本测试用不到：一律拒绝，确保写入一定走块/分块通路
                return Err(ErrorCode::CmdUnknown);
            case CommandCode::ShortUpload:
                return handleShortUpload(packet);
            case CommandCode::Upload:
                return handleUpload(packet);
            case CommandCode::BuildChecksum:
                return handleBuildChecksum(packet);
            case CommandCode::SetRequest:
                return handleSetRequest(packet);
            default:
                return Err(ErrorCode::CmdUnknown);
        }
    }

private:
    std::size_t AgBytes() const { return AgToBytes(m_ag_); }
    std::size_t FrameCapacity() const {
        const std::size_t alignment =
            static_cast<std::size_t>(DownloadAlignmentBytes(m_ag_));
        return (static_cast<std::size_t>(m_max_cto_) - 2U - alignment) /
               AgBytes();
    }

    Bytes handleGetStatus() {
        const bool request_bit = m_store_requested_ &&
                                 (m_store_clears_after_ < 0 ||
                                  m_status_polls_ >= m_store_clears_after_);
        ++m_status_polls_;
        return GetStatusResponse(request_bit);
    }

    /// @brief 把从 packet 指定偏移起的若干元素写入镜像并推进 MTA
    void WriteElements(BytesView packet, std::size_t header,
                       std::size_t elements) {
        const auto ag_bytes = AgBytes();
        for (std::size_t k = 0; k < elements; ++k) {
            m_memory_[m_mta_ + static_cast<Address>(k)] =
                packet[header + k * ag_bytes];
        }
        m_mta_ += static_cast<Address>(elements);
    }

    Bytes handleDownload(BytesView packet) {
        if (packet.size() < 2U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto block_elements = static_cast<std::size_t>(packet[1]);
        m_size_fields_.push_back(packet[1]);
        const std::size_t alignment =
            static_cast<std::size_t>(DownloadAlignmentBytes(m_ag_));
        const std::size_t header = 2U + alignment;
        const std::size_t capacity = FrameCapacity();
        if (block_elements == 0U) {
            return Err(ErrorCode::CmdSyntax);
        }
        if (!m_master_block_mode_ && block_elements > capacity) {
            // 未声明 MASTER_BLOCK_MODE(下载块) 却报超帧 size：
            // 按普通 DOWNLOAD 语义这是非法帧。
            // 这里**必须**看下载方向能力 bit0，而不是 CONNECT 的
            // SLAVE_BLOCK_MODE(bit6)：后者描述 Slave→Master 上传块。
            return Err(ErrorCode::CmdSyntax);
        }
        // 本帧实际承载的元素数 = min(块总元素数, 帧容量)
        const std::size_t in_frame =
            (block_elements < capacity) ? block_elements : capacity;
        const auto ag_bytes = AgBytes();
        if (header + in_frame * ag_bytes > packet.size()) {
            return Err(ErrorCode::CmdSyntax);
        }
        WriteElements(packet, header, in_frame);
        const std::size_t remaining = block_elements - in_frame;
        if (remaining == 0U) {
            return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
        }
        m_block_remaining_ = remaining;
        // 块未结束：中间帧不应答
        return {};
    }

    Bytes handleDownloadNext(BytesView packet) {
        if (packet.size() < 2U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto announced = static_cast<std::size_t>(packet[1]);
        m_size_fields_.push_back(packet[1]);
        if (m_block_remaining_ == 0U) {
            // 没有进行中的块就收到续帧：序号错
            return Err(ErrorCode::Sequence);
        }
        if (announced != m_block_remaining_) {
            // 规范：ERR_SEQUENCE 附期望元素数
            return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                         static_cast<std::uint8_t>(ErrorCode::Sequence),
                         static_cast<std::uint8_t>(m_block_remaining_ &
                                                   0xFFU)};
        }
        const std::size_t alignment =
            static_cast<std::size_t>(DownloadAlignmentBytes(m_ag_));
        const std::size_t header = 2U + alignment;
        const std::size_t capacity = FrameCapacity();
        const std::size_t in_frame =
            (announced < capacity) ? announced : capacity;
        const auto ag_bytes = AgBytes();
        if (header + in_frame * ag_bytes > packet.size()) {
            return Err(ErrorCode::CmdSyntax);
        }
        WriteElements(packet, header, in_frame);
        const std::size_t remaining = announced - in_frame;
        m_block_remaining_ = remaining;
        if (remaining == 0U) {
            return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
        }
        return {};
    }

    Bytes handleShortUpload(BytesView packet) {
        if (packet.size() < 8U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto elements = static_cast<std::size_t>(packet[1]);
        const Address address = ParseAddressAt4(packet, m_order_);
        Bytes res{static_cast<std::uint8_t>(PacketType::Res)};
        for (std::size_t k = 0; k < elements; ++k) {
            const auto it = m_memory_.find(address + static_cast<Address>(k));
            res.push_back(it == m_memory_.end() ? 0x00U : it->second);
        }
        m_mta_ = address + static_cast<Address>(elements);
        return res;
    }

    Bytes handleUpload(BytesView packet) {
        if (packet.size() < 2U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto elements = static_cast<std::size_t>(packet[1]);
        Bytes res{static_cast<std::uint8_t>(PacketType::Res)};
        for (std::size_t k = 0; k < elements; ++k) {
            const auto it = m_memory_.find(m_mta_ + static_cast<Address>(k));
            res.push_back(it == m_memory_.end() ? 0x00U : it->second);
        }
        m_mta_ += static_cast<Address>(elements);
        return res;
    }

    Bytes handleBuildChecksum(BytesView packet) {
        if (m_reject_build_checksum_) {
            return Err(ErrorCode::CmdUnknown);
        }
        if (packet.size() < 8U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto block_size = ParseU32At4(packet, m_order_);
        if (block_size == 0U) {
            return Err(ErrorCode::CmdSyntax);
        }
        // 采集参与校验的字节（连续字节视图，不区分元素）
        Bytes region;
        region.reserve(static_cast<std::size_t>(block_size) * AgBytes());
        for (std::size_t k = 0; k < static_cast<std::size_t>(block_size); ++k) {
            for (std::size_t b = 0; b < AgBytes(); ++b) {
                const auto it = m_memory_.find(
                    m_mta_ + static_cast<Address>(k));
                region.push_back(it == m_memory_.end() ? 0x00U : it->second);
            }
        }
        const auto type = m_checksum_type_override_.value_or(
            static_cast<std::uint8_t>(CalibrationChecksumType::Add11));
        std::uint32_t value = 0U;
        if (m_checksum_value_override_.has_value()) {
            value = *m_checksum_value_override_;
        } else {
            const auto computed = ComputeXcpChecksum(
                ToChecksumType(type).value_or(CalibrationChecksumType::Add11),
                region);
            value = computed.value_or(0U);
        }
        // RES: [FF][type][reserved WORD][checksum DWORD]
        Bytes res{static_cast<std::uint8_t>(PacketType::Res), type};
        AppendU16(res, 0x0000U);
        AppendU32(res, value);
        m_mta_ += static_cast<Address>(block_size);
        return res;
    }

    Bytes handleSetRequest(BytesView packet) {
        if (m_reject_set_request_) {
            return Err(ErrorCode::CmdUnknown);
        }
        if (packet.size() < 4U) {
            return Err(ErrorCode::CmdSyntax);
        }
        if ((packet[1] & 0x01U) != 0U) {  // STORE_CAL_REQ
            m_store_requested_ = true;
            m_status_polls_ = 0;
        }
        return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
    }

    void AppendU16(Bytes& out, std::uint16_t value) const {
        if (m_order_ == ByteOrder::Intel) {
            out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
        } else {
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        }
    }
    void AppendU32(Bytes& out, std::uint32_t value) const {
        if (m_order_ == ByteOrder::Intel) {
            for (int i = 0; i < 4; ++i) {
                out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) &
                                                        0xFFU));
            }
        } else {
            for (int i = 3; i >= 0; --i) {
                out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) &
                                                        0xFFU));
            }
        }
    }

    AddressGranularity m_ag_;
    ByteOrder m_order_;
    std::uint8_t m_max_cto_;
    std::uint8_t m_max_bs_;
    bool m_block_mode_;         ///< COMM_MODE_BASIC bit6：Slave→Master 上传块
    bool m_master_block_mode_;  ///< COMM_MODE_OPTIONAL bit0：Master→Slave 下载块
    std::map<Address, std::uint8_t> m_memory_;
    Address m_mta_{0};
    std::size_t m_block_remaining_{0};
    std::vector<std::uint8_t> m_size_fields_;
    bool m_reject_build_checksum_{false};
    std::optional<std::uint8_t> m_checksum_type_override_;
    std::optional<std::uint32_t> m_checksum_value_override_;
    bool m_reject_set_request_{false};
    bool m_store_requested_{false};
    int m_store_clears_after_{0};
    int m_status_polls_{0};
    std::map<CommandCode, int> m_counts_;
};

/// @brief Master 脚手架
struct Harness {
    explicit Harness(AddressGranularity ag = AddressGranularity::Byte,
                     ByteOrder order = ByteOrder::Intel,
                     std::uint8_t max_cto = 0x10U, std::uint8_t max_bs = 0x04U,
                     bool block_mode = true, bool master_block_mode = true)
        : slave(ag, order, max_cto, max_bs, block_mode, master_block_mode),
          executor(transport, session,
                   CommandTimeouts{std::chrono::milliseconds(150),
                                   std::chrono::milliseconds(150), 2}) {
        transport.SetResponse([this](BytesView p) { return slave(p); });
        transport.Open(executor);
        (void)executor.ExecuteConnect(0x00);
    }

    /// @brief 让 Session 取到 MAX_BS（块模式判定的必要条件）
    void FetchCommModeInfo() { (void)executor.ExecuteGetCommModeInfo(); }

    struct Env {
        test::MockTransport transport;
        BlockSlave slave;
        Session session;
        CommandExecutor executor;
    };

    test::MockTransport transport;
    BlockSlave slave;
    Session session;
    CommandExecutor executor;
};

// --------------------------------------------------------------------------
// 1. Codec：块序列三帧的线格式
// --------------------------------------------------------------------------

TEST(CalibrationBlockCodec, DownloadNextLayoutIntel) {
    CommandCodec codec(ByteOrder::Intel, AddressGranularity::Byte);
    const auto cto = codec.EncodeDownloadNext(6U, BytesOf({0xAA, 0xBB}));
    // [EF][n=06][无对齐][data]
    EXPECT_EQ(cto, BytesOf({0xEF, 0x06, 0xAA, 0xBB}));
}

TEST(CalibrationBlockCodec, DownloadNextAddsAlignmentOnlyForDword) {
    CommandCodec dword(ByteOrder::Intel, AddressGranularity::DWord);
    const auto cto = dword.EncodeDownloadNext(3U, BytesOf({0x01, 0x02, 0x03,
                                                           0x04}));
    // DOWNLOAD 同构：AG>2 时 [EF][n] 之后插 2 字节对齐
    EXPECT_EQ(cto, BytesOf({0xEF, 0x03, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04}));
}

TEST(CalibrationBlockCodec, DownloadMaxUsesAgMinusOneAlignment) {
    // MAX_CTO=16、AG=DWORD：对齐 3 字节，数据 = 16-4 = 12 字节
    CommandCodec codec(ByteOrder::Intel, AddressGranularity::DWord);
    const Bytes data(12U, 0x5AU);
    const auto cto = codec.EncodeDownloadMax(data, 16U);
    ASSERT_EQ(cto.size(), 16U);
    EXPECT_EQ(cto[0], 0xEE);
    EXPECT_EQ(cto[1], 0x00);
    EXPECT_EQ(cto[2], 0x00);
    EXPECT_EQ(cto[3], 0x00);
    EXPECT_EQ(cto[4], 0x5A);
}

TEST(CalibrationBlockCodec, DownloadMaxRejectsWrongLength) {
    CommandCodec codec(ByteOrder::Intel, AddressGranularity::Byte);
    // AG=BYTE、MAX_CTO=16 → 数据必须正好 15 字节
    EXPECT_THROW((void)codec.EncodeDownloadMax(Bytes(14U, 0x00), 16U),
                 XcpException);
}

TEST(CalibrationBlockCodec, SetRequestLayout) {
    CommandCodec codec(ByteOrder::Intel, AddressGranularity::Byte);
    const auto cto =
        codec.EncodeSetRequest(SetRequestModeBit::kStoreCalReq, 0x1234U);
    EXPECT_EQ(cto, BytesOf({0xF9, 0x01, 0x34, 0x12}));
}

TEST(CalibrationBlockCodec, SetRequestRejectsZeroMode) {
    CommandCodec codec(ByteOrder::Intel, AddressGranularity::Byte);
    EXPECT_THROW(
        (void)codec.EncodeSetRequest(SetRequestModeBit::kNone, 0U),
        XcpException);
}

TEST(CalibrationBlockCodec, BuildChecksumLayout) {
    CommandCodec codec(ByteOrder::Intel, AddressGranularity::Byte);
    const auto cto = codec.EncodeBuildChecksum(0x00000100U);
    // [F3][reserved][reserved WORD][Block size DWORD]
    EXPECT_EQ(cto,
              BytesOf({0xF3, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}));
}

TEST(CalibrationBlockCodec, BuildChecksumRejectsZeroBlock) {
    CommandCodec codec(ByteOrder::Intel, AddressGranularity::Byte);
    EXPECT_THROW((void)codec.EncodeBuildChecksum(0U), XcpException);
}

// --------------------------------------------------------------------------
// 2. Parser：BUILD_CHECKSUM 响应
// --------------------------------------------------------------------------

TEST(CalibrationBlockParser, ParsesKnownChecksumType) {
    ResponseParser parser(ByteOrder::Intel);
    // res_data 已剥离 0xFF：[type=0x01][reserved WORD][checksum DWORD]
    const auto res = parser.ParseBuildChecksum(
        BytesOf({0x01, 0x00, 0x00, 0xDD, 0x01, 0x00, 0x00}));
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->raw_checksum_type, 0x01U);
    ASSERT_TRUE(res->checksum_type.has_value());
    EXPECT_EQ(*res->checksum_type, CalibrationChecksumType::Add11);
    EXPECT_EQ(res->checksum, 0x000001DDU);
}

TEST(CalibrationBlockParser, KeepsUnknownTypeAsRawByte) {
    ResponseParser parser(ByteOrder::Intel);
    const auto res = parser.ParseBuildChecksum(
        BytesOf({0x42, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00}));
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->raw_checksum_type, 0x42U);
    EXPECT_FALSE(res->checksum_type.has_value())
        << "未知类型不能编造成某个具体算法";
}

TEST(CalibrationBlockParser, ShortFrameReturnsNullopt) {
    ResponseParser parser(ByteOrder::Intel);
    EXPECT_FALSE(
        parser.ParseBuildChecksum(BytesOf({0x01, 0x00, 0x00, 0xDD})).has_value());
}

TEST(CalibrationBlockParser, MotorolaByteOrderForChecksum) {
    ResponseParser parser(ByteOrder::Motorola);
    // Motorola：checksum DWORD 大端，0x000001DD 编码为 00 00 01 DD
    const auto res = parser.ParseBuildChecksum(
        BytesOf({0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0xDD}));
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->checksum, 0x000001DDU);
}

// --------------------------------------------------------------------------
// 3. 块传输写入：首帧报整块、续帧报剩余、只有末帧应答
// --------------------------------------------------------------------------

TEST(CalibrationBlockWrite, UsesBlockSequenceWhenSupported) {
    // MAX_CTO=16、AG=BYTE → 单帧 14 元素；MAX_BS=4 → 块上限 56
    Harness h;
    h.FetchCommModeInfo();
    const Bytes payload(100U, 0x00U);
    Bytes data(100U);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i);
    }

    MemoryAccess access(h.executor, h.session);
    access.SetBlockModeEnabled(true);
    access.WriteBytes(0x0100, 0x00, data);

    // 100 = 56 + 44 → 两块，每块首帧 DOWNLOAD + 3 帧 DOWNLOAD_NEXT
    EXPECT_EQ(h.slave.count(CommandCode::Download), 2);
    EXPECT_EQ(h.slave.count(CommandCode::DownloadNext), 6);
    // 每块首帧的 size 字段是整块元素数
    const auto sizes = h.slave.SizeFields();
    ASSERT_EQ(sizes.size(), 8U);
    EXPECT_EQ(sizes[0], 56U);
    EXPECT_EQ(sizes[4], 44U);
    // 写入内容正确落进镜像
    EXPECT_EQ(h.slave.Peek(0x0100, data.size()), data);
}

TEST(CalibrationBlockWrite, LastFrameOfEachBlockIsIdenticalToRemaining) {
    Harness h;
    h.FetchCommModeInfo();
    Bytes data(100U);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(0xA0U + (i & 0x0FU));
    }
    MemoryAccess access(h.executor, h.session);
    access.SetBlockModeEnabled(true);
    access.WriteBytes(0x0200, 0x00, data);

    // 块1：56 → 首帧报 56，续帧依次报 42 / 28 / 14
    // 块2：44 → 首帧报 44，续帧依次报 30 / 16 / 2
    const auto sizes = h.slave.SizeFields();
    ASSERT_EQ(sizes.size(), 8U);
    EXPECT_EQ(sizes[1], 42U);
    EXPECT_EQ(sizes[2], 28U);
    EXPECT_EQ(sizes[3], 14U);
    EXPECT_EQ(sizes[5], 30U);
    EXPECT_EQ(sizes[6], 16U);
    EXPECT_EQ(sizes[7], 2U);
}

TEST(CalibrationBlockWrite, FallsBackToChunkedWhenMasterBlockModeMissing) {
    // 回归（PR#22 #6）：块下载能力必须看 COMM_MODE_OPTIONAL 的
    // MASTER_BLOCK_MODE(bit0)，而不是 CONNECT 的 SLAVE_BLOCK_MODE(bit6)。
    // 这里 Slave 声明了 SLAVE_BLOCK_MODE（上传块）但**未**声明
    // MASTER_BLOCK_MODE（下载块）→ 必须回退逐块 SET_MTA + DOWNLOAD。
    // 旧实现只看 bit6，会误判为可块传并对该 Slave 下发 DOWNLOAD_NEXT。
    Harness h(AddressGranularity::Byte, ByteOrder::Intel, 0x10U, 0x04U,
              /*block_mode=*/true, /*master_block_mode=*/false);
    h.FetchCommModeInfo();
    Bytes data(40U);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(0x10U + i);
    }
    MemoryAccess access(h.executor, h.session);
    access.SetBlockModeEnabled(true);  // 即使显式启用，协商不支持也不得块传
    access.WriteBytes(0x0300, 0x00, data);

    EXPECT_EQ(h.slave.count(CommandCode::DownloadNext), 0);
    // 40 / 14 → 3 块，每块前都有 SET_MTA
    EXPECT_EQ(h.slave.count(CommandCode::Download), 3);
    EXPECT_GE(h.slave.count(CommandCode::SetMta), 3);
    EXPECT_EQ(h.slave.Peek(0x0300, data.size()), data);
}

TEST(CalibrationBlockWrite, BlocksWhenOnlyMasterBlockModeIsDeclared) {
    // 反向回归（PR#22 #6）：只申报 MASTER_BLOCK_MODE(bit0)、未申报
    // SLAVE_BLOCK_MODE(bit6) 时，下载块**应当**启用。旧实现只看 bit6，
    // 会把这类 Slave 错误降级为逐块写入（多花往返但不报错，故更难发现）。
    Harness h(AddressGranularity::Byte, ByteOrder::Intel, 0x10U, 0x04U,
              /*block_mode=*/false, /*master_block_mode=*/true);
    h.FetchCommModeInfo();
    Bytes data(100U);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i);
    }
    MemoryAccess access(h.executor, h.session);
    access.SetBlockModeEnabled(true);
    access.WriteBytes(0x0400, 0x00, data);

    // 100 = 56 + 44 → 两块，每块 1 首帧 + 3 续帧
    EXPECT_EQ(h.slave.count(CommandCode::Download), 2);
    EXPECT_EQ(h.slave.count(CommandCode::DownloadNext), 6);
    EXPECT_EQ(h.slave.Peek(0x0400, data.size()), data);
}

TEST(CalibrationBlockWrite, DefaultsToChunkedEvenWhenBlockModeSupported) {
    // 未调用 SetBlockModeEnabled：即便 Slave 声称支持且 MAX_BS 已取得，
    // 也必须走逐块 SET_MTA + DOWNLOAD——块序列丢帧会静默写错位置，
    // 默认路径不能承担这个风险
    Harness h;
    h.FetchCommModeInfo();
    Bytes data(40U, 0x33U);

    MemoryAccess access(h.executor, h.session);
    access.WriteBytes(0x0350, 0x00, data);

    EXPECT_EQ(h.slave.count(CommandCode::DownloadNext), 0);
    EXPECT_EQ(h.slave.count(CommandCode::Download), 3);
    EXPECT_EQ(h.slave.Peek(0x0350, data.size()), data);
}

TEST(CalibrationBlockWrite, FallsBackWhenCommModeInfoMissing) {
    // 声明了块模式但从未查询 GET_COMM_MODE_INFO：无法确认 MAX_BS，必须保守
    Harness h;
    Bytes data(40U, 0x77U);
    MemoryAccess access(h.executor, h.session);
    access.SetBlockModeEnabled(true);  // 启用但没查 MAX_BS，仍须保守
    access.WriteBytes(0x0400, 0x00, data);

    EXPECT_EQ(h.slave.count(CommandCode::DownloadNext), 0);
    EXPECT_EQ(h.slave.Peek(0x0400, data.size()), data);
}

// --------------------------------------------------------------------------
// 4. VerifyBytes
// --------------------------------------------------------------------------

TEST(CalibrationVerify, UsesSlaveChecksumWhenAlgorithmIsReproducible) {
    Harness h;
    Bytes data(8U);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(0x20U + i);
    }
    h.slave.SetMemory(0x0500, data);

    MemoryAccess access(h.executor, h.session);
    const auto result = access.VerifyBytes(0x0500, 0x00, data);
    EXPECT_EQ(result.status, CalibrationVerificationStatus::Match);
    EXPECT_EQ(result.method, CalibrationVerificationMethod::SlaveChecksum);
    ASSERT_TRUE(result.checksum_type.has_value());
    EXPECT_EQ(*result.checksum_type, CalibrationChecksumType::Add11);
    EXPECT_EQ(result.slave_checksum, result.local_checksum);
    // 没有把整段数据搬回主机
    EXPECT_EQ(h.slave.count(CommandCode::Upload), 0);
    EXPECT_EQ(h.slave.count(CommandCode::ShortUpload), 0);
}

TEST(CalibrationVerify, ReportsMismatchWhenChecksumDiffers) {
    Harness h;
    const Bytes expected(8U, 0x11U);
    h.slave.SetMemory(0x0600, expected);
    // Slave 报一个与本地计算不同的值：内容被认为不一致
    h.slave.SetChecksum(static_cast<std::uint8_t>(CalibrationChecksumType::Add11),
                        0xDEADBEEFU);

    MemoryAccess access(h.executor, h.session);
    const auto result = access.VerifyBytes(0x0600, 0x00, expected);
    EXPECT_EQ(result.status, CalibrationVerificationStatus::Mismatch);
    EXPECT_EQ(result.method, CalibrationVerificationMethod::SlaveChecksum);
    EXPECT_EQ(result.slave_checksum, 0xDEADBEEFU);
}

TEST(CalibrationVerify, FallsBackToReadbackWhenSlaveLacksBuildChecksum) {
    Harness h;
    const Bytes data = BytesOf({0x01, 0x02, 0x03, 0x04});
    h.slave.SetMemory(0x0700, data);
    h.slave.RejectBuildChecksum();

    MemoryAccess access(h.executor, h.session);
    const auto result = access.VerifyBytes(0x0700, 0x00, data);
    EXPECT_EQ(result.status, CalibrationVerificationStatus::Match);
    EXPECT_EQ(result.method, CalibrationVerificationMethod::Readback);
    EXPECT_FALSE(result.checksum_type.has_value());
}

TEST(CalibrationVerify, FallsBackToReadbackWhenAlgorithmNotReproducible) {
    // Slave 报 CRC_32：Table 77 未给参数，本地算不出来 → 必须改读回，
    // 绝不能把"算不出来"当成不匹配
    Harness h;
    const Bytes data = BytesOf({0x0A, 0x0B, 0x0C, 0x0D});
    h.slave.SetMemory(0x0800, data);
    h.slave.SetChecksum(static_cast<std::uint8_t>(CalibrationChecksumType::Crc32),
                        std::nullopt);

    MemoryAccess access(h.executor, h.session);
    const auto result = access.VerifyBytes(0x0800, 0x00, data);
    EXPECT_EQ(result.method, CalibrationVerificationMethod::Readback);
    EXPECT_EQ(result.status, CalibrationVerificationStatus::Match);
}

TEST(CalibrationVerify, ReadbackDetectsMismatch) {
    Harness h;
    const Bytes expected = BytesOf({0x01, 0x02, 0x03, 0x04});
    h.slave.SetMemory(0x0900, BytesOf({0x01, 0x02, 0xFF, 0x04}));
    h.slave.RejectBuildChecksum();

    MemoryAccess access(h.executor, h.session);
    const auto result = access.VerifyBytes(0x0900, 0x00, expected);
    EXPECT_EQ(result.status, CalibrationVerificationStatus::Mismatch);
    EXPECT_EQ(result.method, CalibrationVerificationMethod::Readback);
}

TEST(CalibrationVerify, EmptyExpectedIsRejected) {
    Harness h;
    MemoryAccess access(h.executor, h.session);
    EXPECT_THROW((void)access.VerifyBytes(0x0A00, 0x00, Bytes{}), XcpException);
}

// --------------------------------------------------------------------------
// 5. RequestCalibrationStore
// --------------------------------------------------------------------------

TEST(CalibrationStore, CompletesWhenRequestBitCleared) {
    Harness h;
    h.slave.SetStoreCalReqClearsAfter(1);  // 第一次 GET_STATUS 时位已清除
    MemoryAccess access(h.executor, h.session);

    const auto result =
        access.RequestCalibrationStore(std::chrono::milliseconds(500));
    EXPECT_EQ(result.status, CalibrationStoreStatus::Completed);
    EXPECT_TRUE(result.request_accepted);
    EXPECT_GE(result.polls, 1U);
    EXPECT_EQ(h.slave.count(CommandCode::SetRequest), 1);
}

TEST(CalibrationStore, ReportsPendingWhenBitStaysSet) {
    Harness h;
    h.slave.SetStoreCalReqClearsAfter(-1);  // 永不清除
    MemoryAccess access(h.executor, h.session);

    const auto result =
        access.RequestCalibrationStore(std::chrono::milliseconds(60));
    EXPECT_EQ(result.status, CalibrationStoreStatus::Pending);
    EXPECT_TRUE(result.request_accepted);
    EXPECT_GE(result.polls, 1U);
}

TEST(CalibrationStore, UnsupportedSlaveRaisesUnsupportedFeature) {
    Harness h;
    h.slave.RejectSetRequest();
    MemoryAccess access(h.executor, h.session);

    try {
        (void)access.RequestCalibrationStore(std::chrono::milliseconds(100));
        FAIL() << "SET_REQUEST 不被支持时必须抛出";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
}

TEST(CalibrationStore, ZeroTimeoutIsRejected) {
    Harness h;
    MemoryAccess access(h.executor, h.session);
    try {
        (void)access.RequestCalibrationStore(std::chrono::milliseconds(0));
        FAIL() << "completion_timeout 必须大于 0";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

}  // namespace
}  // namespace calmcar::xcp