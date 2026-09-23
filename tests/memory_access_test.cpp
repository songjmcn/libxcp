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

/// @brief 模拟 ECU 内存：按地址存字节
using MemoryImage = std::map<Address, std::uint8_t>;

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
Address ParseSetMtaAddress(BytesView packet, ByteOrder order) {
    if (order == ByteOrder::Intel) {
        return static_cast<Address>(packet[3]) |
               (static_cast<Address>(packet[4]) << 8) |
               (static_cast<Address>(packet[5]) << 16) |
               (static_cast<Address>(packet[6]) << 24);
    }
    return static_cast<Address>(packet[6]) |
           (static_cast<Address>(packet[5]) << 8) |
           (static_cast<Address>(packet[4]) << 16) |
           (static_cast<Address>(packet[3]) << 24);
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

    void SetMemory(Address address, const Bytes& data) {
        for (std::size_t i = 0; i < data.size(); ++i) {
            m_memory_[address + static_cast<Address>(i)] = data[i];
        }
    }

    /// @brief 令 SHORT_UPLOAD 始终返回 ERR_CMD_UNKNOWN
    void RejectShortUpload() { m_reject_short_upload_ = true; }

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
                m_mta_ext_ = packet[2];
                return Bytes{static_cast<std::uint8_t>(PacketType::Res)};
            case CommandCode::Upload:
                return handleUpload(packet);
            case CommandCode::ShortUpload:
                return handleShortUpload(packet);
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
        if (elements == 0U ||
            elements + 1U >
                static_cast<ElementCount>(m_max_cto_ / AgToBytes(m_ag_))) {
            return Err(ErrorCode::OutOfRange);
        }
        auto data = read(m_mta_, elements);
        if (!data) {
            return Err(ErrorCode::AccessDenied);
        }
        // UPLOAD 的 RES 为 [FF][data...]，不含元素计数字节
        Bytes res{static_cast<std::uint8_t>(PacketType::Res)};
        res.insert(res.end(), data->begin(), data->end());
        m_mta_ += static_cast<Address>(elements) * AgToBytes(m_ag_);
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
        if (elements == 0U || elements > static_cast<ElementCount>(
                                             m_max_cto_ / AgToBytes(m_ag_))) {
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
        m_mta_ = address + static_cast<Address>(elements) * AgToBytes(m_ag_);
        return res;
    }

    std::optional<Bytes> read(Address address, ElementCount elements) {
        Bytes out;
        const auto count =
            static_cast<std::size_t>(elements) * AgToBytes(m_ag_);
        for (std::size_t i = 0; i < count; ++i) {
            const auto it = m_memory_.find(address + static_cast<Address>(i));
            if (it == m_memory_.end()) {
                return std::nullopt;
            }
            out.push_back(it->second);
        }
        return out;
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
    const auto total_bytes = static_cast<std::size_t>(total) * ag_bytes;
    Bytes content;
    content.reserve(total_bytes);
    for (std::size_t i = 0; i < total_bytes; ++i) {
        content.push_back(static_cast<std::uint8_t>(i & 0xFFU));
    }

    Harness h(c.ag, ByteOrder::Intel, c.max_cto,
              static_cast<std::uint16_t>(c.max_cto * 4U));
    h.slave.SetMemory(0x4000, content);

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
    h.slave.SetMemory(0x8000, content);

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
    h.slave.SetMemory(0x9000, content);

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
    h.slave.SetMemory(0xC000, content);

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

}  // namespace
}  // namespace calmcar::xcp
