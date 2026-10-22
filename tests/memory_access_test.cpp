/**
 * @file memory_access_test.cpp
 * @brief MemoryAccess 测试：AG 参数化分块、降级路径、地址溢出与长度校验。
 *
 * 覆盖计划文档 §9.2（AG=1/2/4 参数化）与 §9.5（Mock 集成）中与读取相关的部分。
 * 使用 MockTransport 脚本化内存，不依赖真实 Socket。
 */

#include "libxcp/memory_access.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/command_executor.hpp"
#include "libxcp/session.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 模拟 ECU 内存：按**地址**存元素，每个地址存 AG 个字节
///
/// @note 依据原始 PDF 第 112 页 Table 42「The address granularity indicates
///       the size of an element contained at a single address」与第 113 页
///       Table 43（WORD 下 Address n = Byte 00 Byte 01、Address n+1 =
///       Byte 02 Byte 03）：XCP 地址以**元素**计数，一个地址承载 AG 字节。
///       修正前该镜像把地址当字节偏移（每地址 1 字节），与主站对地址语义
///       的理解不一致，导致 AG>1 的用例在错误假设下「一起错、于是通过」。
using MemoryImage = std::map<Address, Bytes>;

/// @brief 组装 CONNECT 响应报文（RES 前缀 + 7 字节负载）
Bytes ConnectResponseBytes(AddressGranularity ag, ByteOrder order,
                           std::uint8_t max_cto, std::uint16_t max_dto,
                           ResourceMask resource = 0x15U,
                           bool optional = true) {
    std::uint8_t comm_mode = 0x00U;
    if (order == ByteOrder::Motorola) {
        comm_mode |= 0x01U;
    }
    comm_mode |= static_cast<std::uint8_t>(AgToCommModeBasicField(ag) << 1);
    if (optional) {
        comm_mode |= 0x80U;
    }
    Bytes out{static_cast<std::uint8_t>(PacketType::Res), resource, comm_mode,
              max_cto};
    // MAX_DTO 按 Session Byte Order
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

/// @brief GET_STATUS 响应（RES + 5 字节负载）
Bytes GetStatusResponseBytes() {
    return Bytes{static_cast<std::uint8_t>(PacketType::Res),
                 0x00,
                 0x00,
                 0x01,
                 0x07,
                 0x00};
}

/// @brief 从 SHORT_UPLOAD 请求中解出 addr（按 Session Byte Order）
Address ParseShortUploadAddress(BytesView packet, ByteOrder order) {
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

/// @brief 从 SET_MTA 请求中解出 addr
/// @details XCP 1.3 布局 [F6][MODE][rsv][EXT@3][ADDR@4..7]（与 SHORT_UPLOAD
///          地址域同构；XCPlite 对手端协议调试核证）。
Address ParseSetMtaAddress(BytesView packet, ByteOrder order) {
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

/**
 * @brief 模拟 Slave：按 MockTransport 收到的命令生成响应
 *
 * 支持 CONNECT / GET_STATUS / GET_COMM_MODE_INFO / SET_MTA / UPLOAD /
 * SHORT_UPLOAD， 并可脚本化“SHORT_UPLOAD 返回 ERR_CMD_UNKNOWN”“UPLOAD 前 N
 * 次超时”等行为。
 */
class FakeSlave {
public:
    FakeSlave(AddressGranularity ag, ByteOrder order, std::uint8_t max_cto,
              std::uint16_t max_dto)
        : m_ag_(ag),
          m_order_(order),
          m_max_cto_(max_cto),
          m_max_dto_(max_dto) {}

    /// @brief 令 SHORT_UPLOAD 始终返回 ERR_CMD_UNKNOWN
    void RejectShortUpload() { m_reject_short_upload_ = true; }

    /// @brief 令 SHORT_DOWNLOAD 始终返回 ERR_CMD_UNKNOWN（批次14 回落通路）
    void RejectShortDownload() { m_reject_short_download_ = true; }

    /// @brief 令写回（DOWNLOAD / SHORT_DOWNLOAD）返回 ERR_WRITE_PROTECTED
    void SetWriteProtected(bool protected_write) {
        m_write_protected_ = protected_write;
    }

    /// @brief 判断本 mock 当前的地址粒度字节数
    [[nodiscard]] std::size_t AgBytes() const {
        return AgToBytes(m_ag_);
    }

    /// @brief 按元素装载模拟内存（自动使用本 mock 的 AG 作为元素尺寸）
    /// @param address 起始**元素**地址
    /// @param data 数据字节；长度必须是 AG 的整数倍
    /// @note 第 k 个元素位于地址 address + k，承载 AG 字节
    ///       （原始 PDF 第 112 页 Table 42、第 113 页 Table 43）
    void SetMemoryElements(Address address, const Bytes& data) {
        const auto ag_bytes = AgBytes();
        if (ag_bytes == 0U) {
            return;
        }
        const auto count = data.size() / ag_bytes;
        for (std::size_t k = 0; k < count; ++k) {
            Bytes element(data.begin() + k * ag_bytes,
                          data.begin() + (k + 1) * ag_bytes);
            m_memory_[address + static_cast<Address>(k)] = std::move(element);
        }
    }

    /// @brief 按元素装载模拟内存（显式指定元素尺寸）
    /// @param address 起始**元素**地址
    /// @param data 数据字节
    /// @param ag_bytes 每元素的字节数
    void SetMemoryElements(Address address, const Bytes& data,
                           std::size_t ag_bytes) {
        if (ag_bytes == 0U) {
            return;
        }
        const auto count = data.size() / ag_bytes;
        for (std::size_t k = 0; k < count; ++k) {
            Bytes element(data.begin() + k * ag_bytes,
                          data.begin() + (k + 1) * ag_bytes);
            m_memory_[address + static_cast<Address>(k)] = std::move(element);
        }
    }

    /// @brief 读回模拟内存（用于校验写回真的落进了 Slave 内存）
    /// @param address 起始**元素**地址
    /// @param bytes 自起始元素地址起的字节数
    /// @return 按元素顺序拼接的字节
    [[nodiscard]] Bytes Peek(Address address, std::size_t bytes) const {
        const auto ag_bytes = AgBytes();
        if (ag_bytes == 0U) {
            return {};
        }
        Bytes out;
        const auto elements = (bytes + ag_bytes - 1U) / ag_bytes;
        for (std::size_t k = 0; k < elements; ++k) {
            const auto it = m_memory_.find(address + static_cast<Address>(k));
            if (it == m_memory_.end()) {
                out.insert(out.end(), ag_bytes, 0x00U);
                continue;
            }
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
        out.resize(bytes);
        return out;
    }

    /// @brief 按字节视图装载（地址按字节推进，仅用于 AG=BYTE 的旧式用例）
    void SetMemory(Address address, const Bytes& data) {
        SetMemoryElements(address, data, AgBytes());
    }

    /// @brief 令第 n 次 UPLOAD（1 起）不响应（模拟超时）
    void SetTimeoutUploadAt(std::size_t n) { m_timeout_upload_n_ = n; }

    /// @brief 令所有 UPLOAD 都不响应（模拟持续丢包，用于验证重试耗尽）
    void SetTimeoutAllUploads() { m_timeout_all_uploads_ = true; }

    /// @brief 收到命令总数 / 各类命令计数
    [[nodiscard]] int count(CommandCode code) const {
        const auto it = m_counts_.find(code);
        return it == m_counts_.end() ? 0 : it->second;
    }
    [[nodiscard]] int totalCommands() const { return m_total_; }

    /// @brief MockTransport 的响应脚本
    Bytes operator()(BytesView packet) {
        ++m_total_;
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];

        switch (cmd) {
            case CommandCode::Connect:
                return ConnectResponseBytes(m_ag_, m_order_, m_max_cto_,
                                            m_max_dto_);
            case CommandCode::GetStatus:
                return GetStatusResponseBytes();
            case CommandCode::GetCommModeInfo:
                return Bytes{static_cast<std::uint8_t>(PacketType::Res),
                             0x00,
                             0x0E,
                             0x00,
                             0x04,
                             0x02,
                             0x08,
                             0x13};
            case CommandCode::Synch:
                // SYNCH 始终以 ERR_CMD_SYNCH 应答
                return Err(ErrorCode::CmdSynch);
            case CommandCode::Disconnect:
                return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
            case CommandCode::SetMta:
                m_mta_ = ParseSetMtaAddress(packet, m_order_);
                m_mta_ext_ = packet[3];  // EXT 在 byte3（XCP 1.3 布局）
                return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
            case CommandCode::Upload:
                return handleUpload(packet);
            case CommandCode::ShortUpload:
                return handleShortUpload(packet);
            case CommandCode::Download:
                return handleDownload(packet);
            case CommandCode::ShortDownload:
                return handleShortDownload(packet);
            default:
                return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                             static_cast<std::uint8_t>(ErrorCode::CmdUnknown)};
        }
    }

private:
    Bytes handleUpload(BytesView packet) {
        if (m_timeout_all_uploads_) {
            return {};  // 模拟持续丢包/超时
        }
        if (++m_upload_seen_ == m_timeout_upload_n_) {
            return {};  // 模拟丢包/超时
        }
        if (packet.size() < 2U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto elements = static_cast<ElementCount>(packet[1]);
        // 容量以字节计：elements * AG 不得超过 MAX_CTO
        // （UPLOAD 的 RES 为 [FF][data...]，不含元素计数字节，故 -1）
        const auto ag_bytes = AgToBytes(m_ag_);
        if (elements == 0U ||
            static_cast<std::size_t>(elements) * ag_bytes + 1U > m_max_cto_) {
            return Err(ErrorCode::OutOfRange);
        }
        auto data = read(m_mta_, elements);
        if (!data) {
            return Err(ErrorCode::AccessDenied);
        }
        // UPLOAD 的 RES 为 [FF][data...]，不含元素计数字节
        Bytes res{static_cast<std::uint8_t>(PacketType::Res)};
        res.insert(res.end(), data->begin(), data->end());
        // MTA 按元素数前移：XCP 地址以元素为单位（原始 PDF 第 113 页
        // Table 43：WORD 下 Address n+1 = Byte 02，地址 +1 即前进 1 个元素），
        // 故推进量为 elements 本身，AG 不得乘入地址。
        m_mta_ += static_cast<Address>(elements);
        return res;
    }

    Bytes handleShortUpload(BytesView packet) {
        if (m_reject_short_upload_) {
            return Err(ErrorCode::CmdUnknown);
        }
        if (packet.size() < 8U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto elements = static_cast<ElementCount>(packet[1]);
        // 容量以字节计：elements * AG 不得超过 MAX_CTO
        const auto ag_bytes = AgToBytes(m_ag_);
        if (elements == 0U ||
            static_cast<std::size_t>(elements) * ag_bytes > m_max_cto_) {
            return Err(ErrorCode::OutOfRange);
        }
        const Address address = ParseShortUploadAddress(packet, m_order_);
        auto data = read(address, elements);
        if (!data) {
            return Err(ErrorCode::AccessDenied);
        }
        // SHORT_UPLOAD 的 RES 同样为 [FF][data...]
        Bytes res{static_cast<std::uint8_t>(PacketType::Res)};
        res.insert(res.end(), data->begin(), data->end());
        // 同 handleUpload：地址按元素计数，AG 不参与地址推进
        m_mta_ = address + static_cast<Address>(elements);
        return res;
    }

    /// @brief 自元素地址读取若干元素
    /// @param address 起始元素地址
    /// @param elements 要读取的元素个数
    /// @return 共 elements * AG 字节；任一元素首字节未映射则返回 nullopt
    ///
    /// @note 地址以**元素**为单位（原始 PDF 第 113 页 Table 43：WORD 下
    ///       Address n = Byte 00、Address n+1 = Byte 02），第 k 个元素位于
    ///       address + k*AG。修正前误按 elements * AgToBytes(ag) 连续步进
    ///       地址，并把 AG 乘进地址推进，与主站「一起错」，导致 AG>1 用例
    ///       全部虚假通过。
    std::optional<Bytes> read(Address address, ElementCount elements) {
        Bytes out;
        const auto ag_bytes = AgBytes();
        const auto count = static_cast<std::size_t>(elements);
        out.reserve(count * ag_bytes);
        // 地址以元素计数：第 k 个元素位于 address + k，承载 AG 字节
        for (std::size_t k = 0; k < count; ++k) {
            const auto it = m_memory_.find(address + static_cast<Address>(k));
            if (it == m_memory_.end()) {
                return std::nullopt;
            }
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
        return out;
    }

    // ---- 批次14（T14-13）：写回通路 ----

    Bytes handleDownload(BytesView packet) {
        if (m_write_protected_) {
            return Err(ErrorCode::WriteProtected);
        }
        if (packet.size() < 2U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto elements = static_cast<ElementCount>(packet[1]);
        // AG>2（即 DWORD）时 [F0][SIZE] 之后有 2 字节对齐
        // （原始 PDF 第 142 页 Table 82："only if AG >2"；
        //   "If AG = DWORD, 2 alignment bytes must be used"）
        const auto alignment = static_cast<std::size_t>(
            DownloadAlignmentBytes(m_ag_));
        const auto header = 2U + alignment;
        const auto ag_bytes = AgToBytes(m_ag_);
        // 净荷长度 = 元素数 * AG
        const auto want = static_cast<std::size_t>(elements) * ag_bytes;
        if (elements == 0U || header + want > packet.size() ||
            header + want > m_max_cto_) {
            // 元素数与净荷长度必须自洽，且整帧不得超 MAX_CTO
            return Err(ErrorCode::CmdSyntax);
        }
        for (std::size_t k = 0; k < static_cast<std::size_t>(elements); ++k) {
            Bytes element(packet.begin() + header + k * ag_bytes,
                          packet.begin() + header + (k + 1) * ag_bytes);
            m_memory_[m_mta_ + static_cast<Address>(k)] = std::move(element);
        }
        // 地址按元素计数，推进量为元素数（AG 不参与地址推进）
        m_mta_ += static_cast<Address>(elements);
        return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
    }

    Bytes handleShortDownload(BytesView packet) {
        if (m_reject_short_download_) {
            return Err(ErrorCode::CmdUnknown);
        }
        if (m_write_protected_) {
            return Err(ErrorCode::WriteProtected);
        }
        if (packet.size() < 8U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto elements = static_cast<ElementCount>(packet[1]);
        // 净荷长度 = 元素数 * AG；SHORT_DOWNLOAD 头固定 8 字节，
        // 数据天然按 AG 对齐，不额外插对齐字节
        const auto ag_bytes = AgToBytes(m_ag_);
        const auto want = static_cast<std::size_t>(elements) * ag_bytes;
        if (elements == 0U || want + 8U > packet.size() ||
            want + 8U > m_max_cto_) {
            return Err(ErrorCode::CmdSyntax);
        }
        const Address address = ParseShortUploadAddress(packet, m_order_);
        for (std::size_t k = 0; k < static_cast<std::size_t>(elements); ++k) {
            Bytes element(packet.begin() + 8 + k * ag_bytes,
                          packet.begin() + 8 + (k + 1) * ag_bytes);
            m_memory_[address + static_cast<Address>(k)] = std::move(element);
        }
        return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
    }

    static Bytes Err(ErrorCode code) {
        return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                     static_cast<std::uint8_t>(code)};
    }

    AddressGranularity m_ag_;
    ByteOrder m_order_;
    std::uint8_t m_max_cto_;
    std::uint16_t m_max_dto_;
    MemoryImage m_memory_;
    Address m_mta_{0};
    AddressExtension m_mta_ext_{0};
    bool m_reject_short_upload_{false};
    /// @brief 批次14：SHORT_DOWNLOAD 是否回 ERR_CMD_UNKNOWN（测回落通路）
    bool m_reject_short_download_{false};
    /// @brief 批次14：写回是否一律拒绝（ERR_WRITE_PROTECTED）
    bool m_write_protected_{false};
    std::size_t m_timeout_upload_n_{0};
    bool m_timeout_all_uploads_{false};
    std::size_t m_upload_seen_{0};
    int m_total_{0};
    std::map<CommandCode, int> m_counts_;
};

/// @brief 一次完整的 Master 脚手架：MockTransport + Session + Executor +
/// MemoryAccess
struct Harness {
    explicit Harness(AddressGranularity ag = AddressGranularity::Byte,
                     ByteOrder order = ByteOrder::Intel,
                     std::uint8_t max_cto = 8U, std::uint16_t max_dto = 8U)
        : slave(ag, order, max_cto, max_dto),
          executor(transport, session,
                   CommandTimeouts{std::chrono::milliseconds(150),
                                   std::chrono::milliseconds(150), 2}) {
        transport.SetResponse([this](BytesView p) { return slave(p); });
        transport.Open(executor);
        (void)executor.ExecuteConnect(0x00);
    }

    test::MockTransport transport;
    FakeSlave slave;
    Session session;
    CommandExecutor executor;
};

Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

// --------------------------------------------------------------------------
// 基本读取：SHORT_UPLOAD 路径
// --------------------------------------------------------------------------

TEST(MemoryAccessBasic, ShortUploadReadsMemory) {
    Harness h;
    const Bytes content = BytesOf({0xDE, 0xAD, 0xBE, 0xEF});
    h.slave.SetMemory(0x1000, content);

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.ReadElements(0x1000, 0x00, 4);
    EXPECT_EQ(got, content);
    EXPECT_EQ(h.slave.count(CommandCode::ShortUpload), 1);
    EXPECT_EQ(h.slave.count(CommandCode::SetMta), 0)
        << "单包可容纳时不应走 SET_MTA+UPLOAD";
}

TEST(MemoryAccessBasic, ReadBytesDividesByAg) {
    Harness h;
    const Bytes content = BytesOf({0x01, 0x02, 0x03, 0x04, 0x05, 0x06});
    h.slave.SetMemory(0x2000, content);

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.ReadBytes(0x2000, 0x00, 6);
    EXPECT_EQ(got, content);
}

TEST(MemoryAccessBasic, ReadBytesRejectsNonMultipleOfAg) {
    // AG=WORD：奇数字节数必须被拒绝
    Harness h(AddressGranularity::Word, ByteOrder::Intel, 0x10U, 0x0010U);
    MemoryAccess access(h.executor, h.session);
    EXPECT_THROW((void)access.ReadBytes(0x1000, 0x00, 3), XcpException);
    try {
        (void)access.ReadBytes(0x1000, 0x00, 3);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

TEST(MemoryAccessBasic, ZeroLengthRejected) {
    Harness h;
    MemoryAccess access(h.executor, h.session);
    EXPECT_THROW((void)access.ReadElements(0x1000, 0x00, 0), XcpException);
    EXPECT_THROW((void)access.ReadBytes(0x1000, 0x00, 0), XcpException);
}

TEST(MemoryAccessBasic, RequiresConnectedSession) {
    Harness h;
    h.session.Reset();  // 模拟未连接
    MemoryAccess access(h.executor, h.session);
    EXPECT_THROW((void)access.ReadElements(0x1000, 0x00, 4), XcpException);
    try {
        (void)access.ReadElements(0x1000, 0x00, 4);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
}

// --------------------------------------------------------------------------
// AG=1/2/4 参数化：分块与边界（计划 §9.2）
// --------------------------------------------------------------------------

struct AgCase {
    AddressGranularity ag;
    std::uint8_t max_cto;
    ElementCount short_upload_max;  ///< MAX_CTO/AG
    ElementCount upload_max;        ///< MAX_CTO/AG - 1
};

class MemoryAccessAgTest : public ::testing::TestWithParam<AgCase> {};

TEST_P(MemoryAccessAgTest, ChunkingSplitsAtUploadLimit) {
    const AgCase c = GetParam();
    const auto ag_bytes = AgToBytes(c.ag);

    // 造一块比单块上限大得多的数据，强制多块 UPLOAD
    const ElementCount total = c.upload_max * 3 + 1;
    // 元素地址以元素为单位计数（原始 PDF 第 113 页 Table 43），
    // 故 total 个元素落在 total 个连续地址上；
    // 本 mock 每地址存 ag_bytes 字节，总字节数 = total * AG
    const auto total_bytes = static_cast<std::size_t>(total) * ag_bytes;
    Bytes content;
    content.reserve(total_bytes);
    for (std::size_t i = 0; i < total_bytes; ++i) {
        content.push_back(static_cast<std::uint8_t>(i & 0xFFU));
    }

    Harness h(c.ag, ByteOrder::Intel, c.max_cto,
              static_cast<std::uint16_t>(c.max_cto * 4U));
    h.slave.SetMemoryElements(0x4000, content, ag_bytes);

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.UploadChunked(0x4000, 0x00, total);

    ASSERT_EQ(got.size(), total_bytes);
    EXPECT_EQ(got, content);
    // 每块之前都重发 SET_MTA（计划 §5.3）
    EXPECT_EQ(h.slave.count(CommandCode::SetMta),
              h.slave.count(CommandCode::Upload));
    EXPECT_GT(h.slave.count(CommandCode::Upload), 1);
}

TEST_P(MemoryAccessAgTest, SingleShortUploadWithinLimit) {
    const AgCase c = GetParam();
    const auto ag_bytes = AgToBytes(c.ag);
    const auto bytes = static_cast<std::size_t>(c.short_upload_max) * ag_bytes;
    Bytes content(bytes, 0x5AU);

    Harness h(c.ag, ByteOrder::Intel, c.max_cto,
              static_cast<std::uint16_t>(c.max_cto * 4U));
    h.slave.SetMemoryElements(0x8000, content, ag_bytes);

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.ReadElements(0x8000, 0x00, c.short_upload_max);
    EXPECT_EQ(got, content);
    EXPECT_EQ(h.slave.count(CommandCode::ShortUpload), 1);
}

TEST_P(MemoryAccessAgTest, BeyondShortUploadLimitFallsBackToChunked) {
    const AgCase c = GetParam();
    const auto ag_bytes = AgToBytes(c.ag);
    // 超过 SHORT_UPLOAD 上限但不超 UPLOAD 分块能力
    const ElementCount total = c.short_upload_max + 1;
    const auto total_bytes = static_cast<std::size_t>(total) * ag_bytes;
    Bytes content(total_bytes, 0x3CU);

    Harness h(c.ag, ByteOrder::Intel, c.max_cto,
              static_cast<std::uint16_t>(c.max_cto * 4U));
    h.slave.SetMemoryElements(0x9000, content, ag_bytes);

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.ReadElements(0x9000, 0x00, total);
    EXPECT_EQ(got, content);
    EXPECT_EQ(h.slave.count(CommandCode::ShortUpload), 0)
        << "超限时应直接走 SET_MTA+UPLOAD";
    EXPECT_GT(h.slave.count(CommandCode::Upload), 0);
}

TEST_P(MemoryAccessAgTest, AddressAdvancesByAgBytes) {
    const AgCase c = GetParam();
    const auto ag_bytes = AgToBytes(c.ag);
    const ElementCount total = c.upload_max * 2;  // 恰好两块
    const auto total_bytes = static_cast<std::size_t>(total) * ag_bytes;
    Bytes content(total_bytes);
    for (std::size_t i = 0; i < total_bytes; ++i) {
        content[i] = static_cast<std::uint8_t>(0x10U + i);
    }

    Harness h(c.ag, ByteOrder::Intel, c.max_cto,
              static_cast<std::uint16_t>(c.max_cto * 4U));
    h.slave.SetMemoryElements(0xC000, content, ag_bytes);

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.UploadChunked(0xC000, 0x00, total);
    // 结果必须严格按地址顺序拼接（AG 换算正确）
    EXPECT_EQ(got, content);
}

INSTANTIATE_TEST_SUITE_P(
    AgMatrix, MemoryAccessAgTest,
    ::testing::Values(AgCase{AddressGranularity::Byte, 0x08U, 8U, 7U},
                      AgCase{AddressGranularity::Word, 0x08U, 4U, 3U},
                      AgCase{AddressGranularity::DWord, 0x08U, 2U, 1U},
                      AgCase{AddressGranularity::Byte, 0x40U, 64U, 63U},
                      AgCase{AddressGranularity::Word, 0x10U, 8U, 7U},
                      AgCase{AddressGranularity::DWord, 0x20U, 8U, 7U}),
    [](const ::testing::TestParamInfo<AgCase>& info) {
        return std::string("Ag") + std::to_string(AgToBytes(info.param.ag)) +
               "_MaxCto" + std::to_string(info.param.max_cto);
    });

// --------------------------------------------------------------------------
// 降级：SHORT_UPLOAD 返回 ERR_CMD_UNKNOWN
// --------------------------------------------------------------------------

TEST(MemoryAccessFallback, ErrCmdUnknownDisablesShortUploadForSession) {
    Harness h;
    const Bytes content = BytesOf({0xAA, 0xBB, 0xCC});
    h.slave.SetMemory(0x1100, content);
    h.slave.RejectShortUpload();

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.ReadElements(0x1100, 0x00, 3);

    EXPECT_EQ(got, content) << "降级后仍应读出正确数据";
    EXPECT_FALSE(h.session.Parameters().short_upload_available)
        << "应标记 SHORT_UPLOAD 不可用";
    EXPECT_EQ(h.slave.count(CommandCode::ShortUpload), 1)
        << "只应尝试一次 SHORT_UPLOAD";
    EXPECT_GT(h.slave.count(CommandCode::Upload), 0);
}

TEST(MemoryAccessFallback, SubsequentReadsSkipShortUpload) {
    Harness h;
    h.slave.SetMemory(0x1200, BytesOf({0x01, 0x02}));
    h.slave.RejectShortUpload();

    MemoryAccess access(h.executor, h.session);
    (void)access.ReadElements(0x1200, 0x00, 2);  // 第一次触发降级
    const int attempts_after_first = h.slave.count(CommandCode::ShortUpload);

    (void)access.ReadElements(0x1200, 0x00, 2);  // 第二次应直接走 UPLOAD
    EXPECT_EQ(h.slave.count(CommandCode::ShortUpload), attempts_after_first)
        << "降级后不应再尝试 SHORT_UPLOAD";
}

// --------------------------------------------------------------------------
// 地址溢出
// --------------------------------------------------------------------------

TEST(MemoryAccessOverflow, RejectsReadPastEndOfAddressSpace) {
    Harness h;
    MemoryAccess access(h.executor, h.session);
    // 0xFFFFFFFE + 4 字节 > 0xFFFFFFFF
    EXPECT_THROW((void)access.ReadElements(0xFFFFFFFEU, 0x00, 4), XcpException);
    try {
        (void)access.ReadElements(0xFFFFFFFEU, 0x00, 4);
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

TEST(MemoryAccessOverflow, AcceptsReadEndingExactlyAtLimit) {
    Harness h;
    h.slave.SetMemory(0xFFFFFFFCU, BytesOf({0x11, 0x22, 0x33, 0x44}));
    MemoryAccess access(h.executor, h.session);
    // 恰好读到最后一个字节：合法
    EXPECT_NO_THROW((void)access.ReadElements(0xFFFFFFFCU, 0x00, 4));
}

TEST(MemoryAccessOverflow, AgMultipliedRangeOverflowDetected) {
    // AG=DWORD：0xFFFFFFF0 + 8 元素 => 超出 32 位
    Harness h(AddressGranularity::DWord, ByteOrder::Intel, 0x08U, 0x0008U);
    MemoryAccess access(h.executor, h.session);
    EXPECT_THROW((void)access.ReadElements(0xFFFFFFF0U, 0x00, 8), XcpException);
}

// --------------------------------------------------------------------------
// 超时恢复：UPLOAD 前重发 SET_MTA（计划 §5.3 / §6.2 第 3 条）
// --------------------------------------------------------------------------

TEST(MemoryAccessRecovery, UploadTimeoutRecoversByResendingSetMta) {
    Harness h;
    const auto max_cto = h.session.MaxCto();
    const auto chunk_limit =
        static_cast<ElementCount>(max_cto) - 1U;  // UPLOAD 单块上限
    const ElementCount total = chunk_limit * 3U;  // 强制 3 块
    Bytes content(total, 0x77);
    h.slave.SetMemory(0x10000, content);

    // 第 2 次 UPLOAD 不响应；Executor 会 SYNCH 恢复后重试该命令。
    // 注意 setMemory/超时计数从 1 起（Harness 构造只发 CONNECT，不产生
    // UPLOAD）。
    h.slave.SetTimeoutUploadAt(2);

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.UploadChunked(0x10000, 0x00, total);

    EXPECT_EQ(got, content) << "超时恢复后仍应读全数据";
    EXPECT_GE(h.slave.count(CommandCode::Synch), 1);
    // 关键断言：3 块 + 1 次超时重试 = 4 次 UPLOAD，每次前各有 SET_MTA
    EXPECT_EQ(h.slave.count(CommandCode::Upload), 4);
    EXPECT_EQ(h.slave.count(CommandCode::SetMta), 4);
}

TEST(MemoryAccessRecovery, RetryExhaustionProducesRecoveryFailed) {
    Harness h;
    h.slave.SetMemory(0x20000, BytesOf({0x01, 0x02, 0x03}));
    // 所有 UPLOAD 都不响应 -> 重试耗尽
    h.slave.SetTimeoutAllUploads();

    MemoryAccess access(h.executor, h.session);
    try {
        (void)access.UploadChunked(0x20000, 0x00, 3);
        FAIL() << "应在重试耗尽后抛出";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::RecoveryFailed);
        // 错误消息应携带已完成元素数上下文（计划 §5.3）
        EXPECT_NE(std::string(e.what()).find("已完成"), std::string::npos);
    }
}

// --------------------------------------------------------------------------
// 响应长度校验
// --------------------------------------------------------------------------

TEST(MemoryAccessResLength, MismatchedResLengthRejected) {
    // Slave 返回的字节数与请求的 elements*AG 不符 -> MalformedPacket
    auto transport = std::make_unique<test::MockTransport>();
    Session session;
    CommandTimeouts timeouts{std::chrono::milliseconds(150),
                             std::chrono::milliseconds(150), 0};
    CommandExecutor executor(*transport, session, timeouts);

    transport->SetResponse([](BytesView packet) -> Bytes {
        const auto cmd = static_cast<CommandCode>(packet[0]);
        if (cmd == CommandCode::Connect) {
            return ConnectResponseBytes(AddressGranularity::Byte,
                                        ByteOrder::Intel, 8U, 8U);
        }
        if (cmd == CommandCode::ShortUpload) {
            // 请求 4 元素，却只返回 2 字节数据
            return Bytes{static_cast<std::uint8_t>(PacketType::Res), 0x01,
                         0x02};
        }
        return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
    });
    transport->Open(executor);
    (void)executor.ExecuteConnect(0x00);

    try {
        (void)executor.ExecuteShortUpload(4, 0x00, 0x1000);
        FAIL() << "长度不符应抛 MalformedPacket";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
    }
}

// --------------------------------------------------------------------------
// 多字节地址编码：Motorola 会话下的 SET_MTA / SHORT_UPLOAD 地址字节序
// --------------------------------------------------------------------------

TEST(MemoryAccessByteOrder, MotorolaSessionEncodesAddressBigEndian) {
    Harness h(AddressGranularity::Byte, ByteOrder::Motorola, 0x08U, 0x0008U);
    const Bytes content = BytesOf({0x99, 0x88, 0x77, 0x66});
    h.slave.SetMemory(0x12345678, content);

    MemoryAccess access(h.executor, h.session);
    const Bytes got = access.ReadElements(0x12345678, 0x00, 4);
    EXPECT_EQ(got, content) << "大端会话下地址解码错误会导致读错数据";
    EXPECT_EQ(h.session.GetByteOrder(), ByteOrder::Motorola);
}

// --------------------------------------------------------------------------
// 批次14（T14-13）：写回通路（SHORT_DOWNLOAD 优先 / 分块 DOWNLOAD / 负例）
// --------------------------------------------------------------------------

TEST(MemoryAccessWrite, ShortDownloadUsedWhenItFits) {
    // MAX_CTO=16 → (16-8)/1 = 8 元素，4 字节一帧装得下
    Harness h(AddressGranularity::Byte, ByteOrder::Intel, 0x10U, 0x0010U);
    MemoryAccess access(h.executor, h.session);
    const Bytes data = BytesOf({0x00, 0x00, 0x80, 0x3F});
    access.WriteBytes(0x6000, 0x00, data);
    EXPECT_EQ(h.slave.count(CommandCode::ShortDownload), 1);
    EXPECT_EQ(h.slave.count(CommandCode::SetMta), 0);
    EXPECT_EQ(h.slave.count(CommandCode::Download), 0);
    // 真的落进了 Slave 内存
    EXPECT_EQ(h.slave.Peek(0x6000, 4), data);
}

TEST(MemoryAccessWrite, MaxCtoEightCannotUseShortDownload) {
    // MAX_CTO=8：SHORT_DOWNLOAD 头占满一帧（docs L2026）→ 必须走分块
    Harness h;
    MemoryAccess access(h.executor, h.session);
    access.WriteBytes(0x6100, 0x00, BytesOf({0xAA, 0xBB}));
    EXPECT_EQ(h.slave.count(CommandCode::ShortDownload), 0);
    EXPECT_EQ(h.slave.count(CommandCode::Download), 1);
    EXPECT_EQ(h.slave.count(CommandCode::SetMta), 1);
}

TEST(MemoryAccessWrite, ChunkedDownloadSetsMtaPerChunk) {
    // MAX_CTO=8, AG=1 → 每块 (8-2)/1 = 6 元素；14 字节 → 6+6+2 三块
    Harness h;
    MemoryAccess access(h.executor, h.session);
    Bytes data;
    for (int i = 0; i < 14; ++i) {
        data.push_back(static_cast<std::uint8_t>(0x10 + i));
    }
    access.WriteBytes(0x7000, 0x00, data);
    EXPECT_EQ(h.slave.count(CommandCode::Download), 3);
    EXPECT_EQ(h.slave.count(CommandCode::SetMta), 3)
        << "每块都必须显式 SET_MTA（不依赖 Slave 的 MTA 自增）";
    EXPECT_EQ(h.slave.Peek(0x7000, 14), data);
}

TEST(MemoryAccessWrite, FallsBackWhenShortDownloadUnsupported) {
    Harness h(AddressGranularity::Byte, ByteOrder::Intel, 0x10U, 0x0010U);
    h.slave.RejectShortDownload();
    MemoryAccess access(h.executor, h.session);
    const Bytes data = BytesOf({0x11, 0x22, 0x33, 0x44});
    access.WriteBytes(0x6200, 0x00, data);
    // ERR_CMD_UNKNOWN 无副作用（docs L1631）→ 回落 SET_MTA+DOWNLOAD 且成功
    EXPECT_EQ(h.slave.count(CommandCode::ShortDownload), 1);
    EXPECT_EQ(h.slave.count(CommandCode::Download), 1);
    EXPECT_EQ(h.slave.Peek(0x6200, 4), data);
}

TEST(MemoryAccessWrite, WriteProtectedPropagatesProtocolError) {
    Harness h;
    h.slave.SetWriteProtected(true);
    MemoryAccess access(h.executor, h.session);
    try {
        access.WriteBytes(0x6300, 0x00, BytesOf({0x01, 0x02}));
        FAIL() << "写保护必须上抛 ProtocolError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        ASSERT_TRUE(e.GetErrorCode().has_value());
        EXPECT_EQ(*e.GetErrorCode(), ErrorCode::WriteProtected);
    }
}

TEST(MemoryAccessWrite, RejectsEmptyAndNonMultipleOfAg) {
    Harness h(AddressGranularity::Word, ByteOrder::Intel, 0x10U, 0x0010U);
    MemoryAccess access(h.executor, h.session);
    EXPECT_THROW(access.WriteBytes(0x1000, 0x00, BytesView{}), XcpException);
    try {
        access.WriteBytes(0x1000, 0x00, BytesOf({0x01, 0x02, 0x03}));
        FAIL() << "奇数字节在 AG=WORD 下必须被拒";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
    EXPECT_EQ(h.slave.totalCommands(), 1)
        << "本地预检失败时一条写命令都不发（AG 换算不猜）";
}

TEST(MemoryAccessWrite, AgIsOnlyUnitConversionNeverMultipliedIntoAddress) {
    // B-1/AG 口径：AG=WORD 时字节数 → 元素数要除以 AG，块地址按**元素**推进
    // （第二块起点 = 0x8000 + 15，不是 0x8000 + 30）
    Harness h(AddressGranularity::Word, ByteOrder::Intel, 0x20U, 0x0020U);
    MemoryAccess access(h.executor, h.session);
    Bytes data;
    for (int i = 0; i < 40; ++i) {  // 40 字节 = 20 元素
        data.push_back(static_cast<std::uint8_t>(i));
    }
    // MAX_CTO=0x20, AG=2 → 每块 (32-2)/2 = 15 元素；20 元素 → 15+5 两块
    access.WriteBytes(0x8000, 0x00, data);
    EXPECT_EQ(h.slave.count(CommandCode::Download), 2);
    EXPECT_EQ(h.slave.Peek(0x8000, 40), data);
}

TEST(MemoryAccessWrite, RequiresConnectedSessionAndOverflowGuard) {
    Harness h;
    MemoryAccess access(h.executor, h.session);
    h.session.Reset();
    EXPECT_THROW(access.WriteBytes(0x1000, 0x00, BytesOf({0x01})),
                 XcpException);
}

}  // namespace
}  // namespace calmcar::xcp
