/**
 * @file xcp_master_integration_test.cpp
 * @brief XcpMaster 端到端集成测试（Mock Transport），覆盖计划文档 §9.5 全部 7
 * 条。
 */

#include "libxcp/xcp_master.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/command_executor.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

Bytes BytesOf(std::initializer_list<std::uint8_t> init) {
    return Bytes(init.begin(), init.end());
}

/// @brief 作用域退出时确保线程被 join（避免异常路径下 joinable 线程析构触发
/// terminate）
class ThreadJoiner {
public:
    explicit ThreadJoiner(std::thread& thread) : m_thread_(thread) {}
    ~ThreadJoiner() {
        if (m_thread_.joinable()) {
            m_thread_.join();
        }
    }
    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;

private:
    std::thread& m_thread_;
};

/// @brief 记录 Master 侧事件，用于断言 EV/SERV/DTO 分流
class EventRecorder : public IEventListener {
public:
    void OnEvent(const EventPacket& event) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        ++m_events_;
        if (event.event_code && *event.event_code == EventCode::CmdPending) {
            ++m_cmd_pending_;
        }
    }
    void OnService(const ServicePacket&) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        ++m_services_;
    }
    void OnDto(const DtoPacket&) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        ++m_dtos_;
    }

    [[nodiscard]] int events() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_events_;
    }
    [[nodiscard]] int cmdPending() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_cmd_pending_;
    }
    [[nodiscard]] int services() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_services_;
    }
    [[nodiscard]] int dtos() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_dtos_;
    }

private:
    mutable std::mutex m_mutex_;
    int m_events_{0};
    int m_cmd_pending_{0};
    int m_services_{0};
    int m_dtos_{0};
};

/**
 * @brief 功能完整的 Mock XCP Slave
 *
 * 覆盖 CONNECT / DISCONNECT / GET_STATUS / GET_COMM_MODE_INFO / SET_MTA /
 * UPLOAD / SHORT_UPLOAD / SYNCH，并支持按命令注入错误与丢包。
 */
class MockXcpSlave {
public:
    MockXcpSlave(AddressGranularity ag = AddressGranularity::Byte,
                 ByteOrder order = ByteOrder::Intel, std::uint8_t max_cto = 8U,
                 std::uint16_t max_dto = 8U, bool optional = true)
        : m_ag_(ag),
          m_order_(order),
          m_max_cto_(max_cto),
          m_max_dto_(max_dto),
          m_optional_(optional) {}

    /// @brief 写入模拟 ECU 内存（按元素装载）
    /// @param address 起始**元素**地址
    /// @param data 数据字节；长度必须是 AG 的整数倍
    /// @note 第 k 个元素位于地址 address + k，承载 AG 字节
    void SetMemory(Address address, const Bytes& data) {
        const auto ag_bytes = AgToBytes(m_ag_);
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

    /// @brief 让指定命令持续返回错误
    void SetError(CommandCode cmd, ErrorCode code) { m_errors_[cmd] = code; }

    /// @brief 让指定命令始终不响应（模拟持续丢包）
    void DropCommand(CommandCode cmd) { m_dropped_.insert(cmd); }

    /// @brief 让指定命令丢 N 次后恢复
    void DropCommandTimes(CommandCode cmd, int times) {
        m_drop_times_[cmd] = times;
    }

    /// @brief 令 SHORT_UPLOAD 返回 ERR_CMD_UNKNOWN
    void SetShortUploadUnsupported() { m_short_upload_unsupported_ = true; }

    /// @brief 变量标定批次：CONNECT RESOURCE 字节（默认含 CAL/PAG|DAQ）
    void SetResourceMask(std::uint8_t mask) { m_resource_mask_ = mask; }

    /// @brief 变量标定批次：为指定命令注入持续 Positive Response
    void SetResponse(CommandCode cmd, Bytes response) {
        m_responses_[cmd] = std::move(response);
    }

    /// @brief 变量标定批次：让指定命令的第 n 次调用不回响应（模拟丢包）
    void DropNthCall(CommandCode cmd, std::size_t n) {
        m_drop_nth_[cmd] = n;
    }

    /// @brief 命令执行后丢弃第 n 次响应，模拟操作已生效但应答丢失
    void DropResponseAfterExecuteNthCall(CommandCode cmd, std::size_t n) {
        m_drop_response_after_execute_nth_[cmd] = n;
    }

    /// @brief 设置模拟标定页内容（供 COPY_CAL_PAGE 回归验证）
    void SetCalibrationPageData(std::uint8_t segment, std::uint8_t page,
                                 Bytes data) {
        m_calibration_pages_[{segment, page}] = std::move(data);
    }

    [[nodiscard]] Bytes CalibrationPageData(std::uint8_t segment,
                                            std::uint8_t page) const {
        const auto it = m_calibration_pages_.find({segment, page});
        return it == m_calibration_pages_.end() ? Bytes{} : it->second;
    }

    /// @brief 变量标定批次：按 MTA 读 4 字节 DWORD（Intel），应用
    ///        Result = Value & (AND<<S) ^ (XOR<<S)，MTA 不变
    ///        （docs §7.5.2.5；xcp.h CRO_MODIFY_BITS_LEN=6）
    Bytes modifyBits(BytesView packet) {
        if (packet.size() < 6U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const std::size_t s = packet[1];
        if (s > 16U) {
            return Err(ErrorCode::OutOfRange);
        }
        const std::uint32_t and_mask = static_cast<std::uint32_t>(packet[2]) |
                                       (static_cast<std::uint32_t>(packet[3])
                                        << 8);
        const std::uint32_t xor_mask = static_cast<std::uint32_t>(packet[4]) |
                                       (static_cast<std::uint32_t>(packet[5])
                                        << 8);
        auto data = read(m_mta_, 4U);
        if (!data) {
            return Err(ErrorCode::AccessDenied);
        }
        std::uint32_t value = static_cast<std::uint32_t>((*data)[0]) |
                              (static_cast<std::uint32_t>((*data)[1]) << 8) |
                              (static_cast<std::uint32_t>((*data)[2]) << 16) |
                              (static_cast<std::uint32_t>((*data)[3]) << 24);
        value = (value & (and_mask << s)) ^ (xor_mask << s);
        // MODIFY_BITS 操作当前 MTA 处的 32bit 字（docs §7.5.2.5），
        // 按元素写入：每元素 AG 字节，地址以元素计数
        const auto ag_bytes = AgToBytes(m_ag_);
        Bytes word;
        word.reserve(4U);
        for (std::size_t i = 0; i < 4U; ++i) {
            word.push_back(
                static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU));
        }
        for (std::size_t off = 0; off < 4U; off += ag_bytes) {
            m_memory_[m_mta_ + static_cast<Address>(off / ag_bytes)] =
                Bytes(word.begin() + off, word.begin() + off + ag_bytes);
        }
        // MODIFY_BITS 不修改 MTA（docs §7.5.2.5）——此处刻意不自增
        return Res({});
    }

    /// @brief 某个命令被调用的次数
    [[nodiscard]] int Count(CommandCode cmd) const {
        const auto it = m_counts_.find(cmd);
        return it == m_counts_.end() ? 0 : it->second;
    }

    /// @brief MockTransport 的响应脚本入口
    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];


        if (m_dropped_.count(cmd) > 0) {
            return {};
        }
        const auto drop_it = m_drop_times_.find(cmd);
        if (drop_it != m_drop_times_.end() && drop_it->second > 0) {
            --drop_it->second;
            return {};
        }
        // 变量标定批次：仅第 n 次调用丢包（恢复重放时序测试用）
        const auto nth_drop_it = m_drop_nth_.find(cmd);
        if (nth_drop_it != m_drop_nth_.end() &&
            static_cast<std::size_t>(m_counts_[cmd]) == nth_drop_it->second) {
            return {};
        }
        const auto err_it = m_errors_.find(cmd);
        if (err_it != m_errors_.end()) {
            return Err(err_it->second);
        }

        // 变量标定批次：脚本注入的持续响应优先于内置 switch
        const auto scripted_it = m_responses_.find(cmd);
        if (scripted_it != m_responses_.end()) {
            return scripted_it->second;
        }

        switch (cmd) {
            case CommandCode::Connect:
                return connectResponse();
            case CommandCode::Disconnect:
                m_connected_ = false;
                return Res({});
            case CommandCode::GetStatus:
                return Res({0x00, 0x00, 0x01, 0x07, 0x00});
            case CommandCode::GetCommModeInfo:
                return Res({0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13});
            case CommandCode::Synch:
                // SYNCH 始终以 ERR_CMD_SYNCH 应答
                return Err(ErrorCode::CmdSynch);
            case CommandCode::SetMta:
                // XCP 1.3 布局：[F6][MODE][rsv][EXT@3][ADDR@4..7]（8 字节，
                // XCPlite 对手端协议调试核证）
                if (packet.size() < 8U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_mta_ = decodeAddress(packet.subspan(4, 4));
                return Res({});
            case CommandCode::Upload:
                return upload(packet);
            case CommandCode::ShortUpload:
                return shortUpload(packet);
            case CommandCode::Download: {
                if (packet.size() < 2U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const auto elements =
                    static_cast<std::size_t>(packet[1]);
                const auto ag_bytes = AgToBytes(m_ag_);
                // AG>2（DWORD）时 [F0][SIZE] 之后有 2 字节对齐
                // （原始 PDF 第 142 页 Table 82："only if AG >2"）
                const auto header = 2U + static_cast<std::size_t>(
                                              DownloadAlignmentBytes(m_ag_));
                const auto count = elements * ag_bytes;
                if (elements == 0U || packet.size() < header + count) {
                    return Err(ErrorCode::CmdSyntax);
                }
                for (std::size_t k = 0; k < elements; ++k) {
                    Bytes element(packet.begin() + header + k * ag_bytes,
                                  packet.begin() + header + (k + 1) * ag_bytes);
                    m_memory_[m_mta_ + static_cast<Address>(k)] =
                        std::move(element);
                }
                // 地址按元素计数，推进量为元素数
                m_mta_ += static_cast<Address>(elements);
                return ShouldDropResponseAfterExecute(cmd) ? Bytes{} : Res({});
            }
            case CommandCode::ShortDownload: {
                if (packet.size() < 8U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const auto elements =
                    static_cast<std::size_t>(packet[1]);
                const auto ag_bytes = AgToBytes(m_ag_);
                const auto count = elements * ag_bytes;
                if (elements == 0U || packet.size() < 8U + count) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const Address address = decodeAddress(packet.subspan(4, 4));
                for (std::size_t k = 0; k < elements; ++k) {
                    Bytes element(packet.begin() + 8U + k * ag_bytes,
                                  packet.begin() + 8U + (k + 1) * ag_bytes);
                    m_memory_[address + static_cast<Address>(k)] =
                        std::move(element);
                }
                m_mta_ = address + static_cast<Address>(elements);
                return ShouldDropResponseAfterExecute(cmd) ? Bytes{} : Res({});
            }
            case CommandCode::ModifyBits: {
                const Bytes response = modifyBits(packet);
                return ShouldDropResponseAfterExecute(cmd) ? Bytes{} : response;
            }
            case CommandCode::GetCalPage: {
                if (packet.size() < 3U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const auto it = m_active_cal_pages_.find({packet[1], packet[2]});
                const std::uint8_t page =
                    it == m_active_cal_pages_.end() ? 0U : it->second;
                return Res({0x00U, 0x00U, page});
            }
            case CommandCode::GetSegmentMode: {
                if (packet.size() < 3U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const auto it = m_segment_modes_.find(packet[2]);
                const std::uint8_t mode =
                    it == m_segment_modes_.end() ? 0U : it->second;
                return Res({0x00U, mode});
            }
            case CommandCode::SetCalPage: {
                if (packet.size() < 4U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const std::uint8_t mode = packet[1];
                const std::uint8_t segment = packet[2];
                const std::uint8_t page = packet[3];
                if ((mode & 0x01U) != 0U) {
                    m_active_cal_pages_[{0x01U, segment}] = page;
                }
                if ((mode & 0x02U) != 0U) {
                    m_active_cal_pages_[{0x02U, segment}] = page;
                }
                return ShouldDropResponseAfterExecute(cmd) ? Bytes{} : Res({});
            }
            case CommandCode::SetSegmentMode: {
                if (packet.size() < 3U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_segment_modes_[packet[2]] = packet[1];
                return ShouldDropResponseAfterExecute(cmd) ? Bytes{} : Res({});
            }
            case CommandCode::CopyCalPage: {
                if (packet.size() < 5U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const auto source = std::make_pair(packet[1], packet[2]);
                const auto destination = std::make_pair(packet[3], packet[4]);
                m_calibration_pages_[destination] = m_calibration_pages_[source];
                return ShouldDropResponseAfterExecute(cmd) ? Bytes{} : Res({});
            }
            default:
                return Err(ErrorCode::CmdUnknown);
        }
    }

private:
    /// @brief 构造 Positive Response
    static Bytes Res(const std::vector<std::uint8_t>& body) {
        Bytes out;
        out.reserve(1 + body.size());
        out.push_back(static_cast<std::uint8_t>(PacketType::Res));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    /// @brief 构造 Negative Response
    static Bytes Err(ErrorCode code) {
        return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                     static_cast<std::uint8_t>(code)};
    }

    [[nodiscard]] bool ShouldDropResponseAfterExecute(CommandCode cmd) const {
        const auto it = m_drop_response_after_execute_nth_.find(cmd);
        return it != m_drop_response_after_execute_nth_.end() &&
               static_cast<std::size_t>(m_counts_.at(cmd)) == it->second;
    }

    /// @brief 按 Session 字节序解出 4 字节地址
    Address decodeAddress(BytesView four) const {
        if (m_order_ == ByteOrder::Intel) {
            return static_cast<Address>(four[0]) |
                   (static_cast<Address>(four[1]) << 8) |
                   (static_cast<Address>(four[2]) << 16) |
                   (static_cast<Address>(four[3]) << 24);
        }
        return static_cast<Address>(four[3]) |
               (static_cast<Address>(four[2]) << 8) |
               (static_cast<Address>(four[1]) << 16) |
               (static_cast<Address>(four[0]) << 24);
    }

    Bytes connectResponse() {
        m_connected_ = true;
        std::uint8_t comm_mode =
            static_cast<std::uint8_t>(AgToCommModeBasicField(m_ag_) << 1);
        if (m_order_ == ByteOrder::Motorola) {
            comm_mode |= 0x01U;
        }
        if (m_optional_) {
            comm_mode |= 0x80U;
        }
        std::vector<std::uint8_t> body{m_resource_mask_, comm_mode, m_max_cto_};
        if (m_order_ == ByteOrder::Intel) {
            body.push_back(static_cast<std::uint8_t>(m_max_dto_ & 0xFFU));
            body.push_back(
                static_cast<std::uint8_t>((m_max_dto_ >> 8) & 0xFFU));
        } else {
            body.push_back(
                static_cast<std::uint8_t>((m_max_dto_ >> 8) & 0xFFU));
            body.push_back(static_cast<std::uint8_t>(m_max_dto_ & 0xFFU));
        }
        body.push_back(0x15U);  // PROTOCOL_LAYER_VERSION（既有硬编码值）
        body.push_back(0x10U);  // TRANSPORT_LAYER_VERSION
        return Res(body);
    }

    Bytes upload(BytesView packet) {
        if (packet.size() < 2U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto elements = static_cast<ElementCount>(packet[1]);
        if (elements == 0U ||
            static_cast<std::size_t>(elements) * AgToBytes(m_ag_) + 1U >
                m_max_cto_) {
            return Err(ErrorCode::OutOfRange);
        }
        auto data = read(m_mta_, elements);
        if (!data) {
            return Err(ErrorCode::AccessDenied);
        }
        // UPLOAD 的 RES 为 [FF][data...]，不含元素计数字节
        // （docs/XCP_1.3.0_document.md §12.4：UPLOAD(6) -> 6 字节）
        std::vector<std::uint8_t> body;
        body.insert(body.end(), data->begin(), data->end());
        // 地址按元素计数，推进量为元素数（AG 不参与地址推进）
        m_mta_ += static_cast<Address>(elements);
        return Res(body);
    }

    Bytes shortUpload(BytesView packet) {
        if (m_short_upload_unsupported_) {
            return Err(ErrorCode::CmdUnknown);
        }
        if (packet.size() < 8U) {
            return Err(ErrorCode::CmdSyntax);
        }
        const auto elements = static_cast<ElementCount>(packet[1]);
        if (elements == 0U ||
            static_cast<std::size_t>(elements) * AgToBytes(m_ag_) >
                m_max_cto_) {
            return Err(ErrorCode::OutOfRange);
        }
        const Address address = decodeAddress(packet.subspan(4, 4));
        auto data = read(address, elements);
        if (!data) {
            return Err(ErrorCode::AccessDenied);
        }
        // SHORT_UPLOAD 的 RES 同样为 [FF][data...]（docs §12.5：size=4 -> 4
        // 字节）
        std::vector<std::uint8_t> body;
        body.insert(body.end(), data->begin(), data->end());
        // SHORT_UPLOAD 之后 MTA 前进到数据块末尾之后
        m_mta_ = address + static_cast<Address>(elements);
        return Res(body);
    }

    /// @brief 自元素地址读取若干元素
    /// @note 地址以元素计数：第 k 个元素位于 address + k，承载 AG 字节
    ///       （原始 PDF 第 112 页 Table 42、第 113 页 Table 43）
    std::optional<Bytes> read(Address address, ElementCount elements) {
        Bytes out;
        const auto count = static_cast<std::size_t>(elements);
        out.reserve(count * AgToBytes(m_ag_));
        for (std::size_t k = 0; k < count; ++k) {
            const auto it = m_memory_.find(address + static_cast<Address>(k));
            if (it == m_memory_.end()) {
                return std::nullopt;
            }
            out.insert(out.end(), it->second.begin(), it->second.end());
        }
        return out;
    }

    AddressGranularity m_ag_;
    ByteOrder m_order_;
    std::uint8_t m_max_cto_;
    std::uint16_t m_max_dto_;
    bool m_optional_{true};
    bool m_connected_{false};
    bool m_short_upload_unsupported_{false};
    std::uint8_t m_resource_mask_{0x15U};  ///< CONNECT RESOURCE（默认含 CAL/PAG|DAQ|PGM）
    Address m_mta_{0};
    /// @brief 模拟 ECU 内存：按**地址**存元素，每地址 AG 字节
    /// @note 原始 PDF 第 112 页 Table 42 / 第 113 页 Table 43：XCP 地址以
    ///       元素计数，一个地址承载 AG 字节（WORD 下 Address n+1 = Byte 02）。
    std::map<Address, Bytes> m_memory_;
    std::map<CommandCode, ErrorCode> m_errors_;
    std::map<CommandCode, Bytes> m_responses_;
    std::map<CommandCode, std::size_t> m_drop_nth_;
    std::map<CommandCode, std::size_t> m_drop_response_after_execute_nth_;
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint8_t>
        m_active_cal_pages_;
    std::map<std::uint8_t, std::uint8_t> m_segment_modes_;
    std::map<std::pair<std::uint8_t, std::uint8_t>, Bytes> m_calibration_pages_;
    std::set<CommandCode> m_dropped_;
    std::map<CommandCode, int> m_drop_times_;
    std::map<CommandCode, int> m_counts_;
};

/// @brief 构造 Positive Response（供测试脚本注入；与 Slave 内部同口径）
Bytes Res(std::initializer_list<std::uint8_t> body) {
    Bytes out;
    out.reserve(1 + body.size());
    out.push_back(static_cast<std::uint8_t>(PacketType::Res));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// @brief Master + MockTransport + MockSlave 的组合脚手架
struct Rig {
    explicit Rig(MockXcpSlave slave_impl = MockXcpSlave{},
                 CommandTimeouts timeouts =
                     CommandTimeouts{std::chrono::milliseconds(300),
                                     std::chrono::milliseconds(300), 2},
                 IEventListener* listener = nullptr)
        : slave(std::move(slave_impl)) {
        auto transport = std::make_unique<test::MockTransport>();
        transport_ptr = transport.get();
        transport->SetResponse([this](BytesView p) { return slave(p); });
        XcpMasterOptions options;
        options.timeouts = timeouts;
        options.event_listener = listener;
        master = std::make_unique<XcpMaster>(std::move(transport), options);
    }

    MockXcpSlave slave;  ///< 模拟 Slave
    test::MockTransport* transport_ptr{
        nullptr};                       ///< Transport 视图（由 master 拥有）
    std::unique_ptr<XcpMaster> master;  ///< 被测对象
};

// --------------------------------------------------------------------------
// §9.5 第 1 条：CONNECT → GET_COMM_MODE_INFO → GET_STATUS → SHORT_UPLOAD →
// DISCONNECT
// --------------------------------------------------------------------------

TEST(XcpMasterIntegration, FullHappyPathSequence) {
    Rig rig;
    const Bytes content = BytesOf({0xDE, 0xAD, 0xBE, 0xEF});
    rig.slave.SetMemory(0x70012340, content);

    rig.master->Connect();
    EXPECT_TRUE(rig.master->IsConnected());
    EXPECT_EQ(rig.master->GetSessionState(), SessionState::Connected);

    const auto params = rig.master->GetSessionParameters();
    EXPECT_EQ(params.connect.max_cto, 8U);
    EXPECT_EQ(params.connect.max_dto, 8U);
    EXPECT_TRUE(params.comm_mode_info.has_value());
    EXPECT_TRUE(params.status.has_value());
    EXPECT_TRUE(params.short_upload_available);

    const Bytes got = rig.master->ReadMemoryBytes(0x70012340, 0x00, 4);
    EXPECT_EQ(got, content);

    const GetStatusResponse status = rig.master->QueryStatus();
    EXPECT_EQ(status.state_number, 0x01U);

    rig.master->Disconnect();
    EXPECT_FALSE(rig.master->IsConnected());

    // 命令顺序应符合连接编排（计划 §5.1）
    EXPECT_EQ(rig.slave.Count(CommandCode::Connect), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetCommModeInfo), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetStatus),
              2);  // Connect 内 1 次 + QueryStatus 1 次
    EXPECT_EQ(rig.slave.Count(CommandCode::ShortUpload), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::Disconnect), 1);
}

TEST(XcpMasterIntegration, OptionalUnavailableSkipsCommModeInfo) {
    Rig rig(MockXcpSlave(AddressGranularity::Byte, ByteOrder::Intel, 8U, 8U,
                         false));
    rig.master->Connect();
    EXPECT_TRUE(rig.master->IsConnected());
    EXPECT_EQ(rig.slave.Count(CommandCode::GetCommModeInfo), 0)
        << "OPTIONAL 位为 0 时不应查询 GET_COMM_MODE_INFO";
    EXPECT_FALSE(rig.master->GetSessionParameters().comm_mode_info.has_value());
}

TEST(XcpMasterIntegration, DisconnectClosesTransport) {
    Rig rig;
    rig.master->Connect();
    ASSERT_TRUE(rig.transport_ptr->IsOpen());
    rig.master->Disconnect();
    EXPECT_FALSE(rig.transport_ptr->IsOpen());
    EXPECT_NO_THROW(rig.master->Disconnect());  // 幂等
}

TEST(XcpMasterIntegration, DestructorDisconnectsAndReleasesResources) {
    Rig rig;
    rig.master->Connect();
    ASSERT_TRUE(rig.master->IsConnected());
    // 注意：transport 由 master 拥有，reset() 后 transport_ptr
    // 即悬空，不可再解引用。 因此改用仍存活的 MockSlave 断言析构确实发送了
    // DISCONNECT。
    rig.master.reset();
    EXPECT_EQ(rig.slave.Count(CommandCode::Disconnect), 1)
        << "析构应尽力发送 DISCONNECT 释放逻辑会话";
}

// --------------------------------------------------------------------------
// §9.5 第 2 条：ERR_CMD_UNKNOWN 降级
// --------------------------------------------------------------------------

TEST(XcpMasterIntegration, CommModeInfoUnknownDegradesAndConnectSucceeds) {
    Rig rig;
    rig.slave.SetError(CommandCode::GetCommModeInfo, ErrorCode::CmdUnknown);
    EXPECT_NO_THROW(rig.master->Connect());
    EXPECT_TRUE(rig.master->IsConnected());
    EXPECT_FALSE(rig.master->GetSessionParameters().comm_mode_info.has_value());
}

TEST(XcpMasterIntegration, ShortUploadUnknownDegradesToChunkedUpload) {
    Rig rig;
    const Bytes content = BytesOf({0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
    rig.slave.SetMemory(0x8000, content);
    rig.slave.SetShortUploadUnsupported();

    rig.master->Connect();
    const Bytes got = rig.master->ReadMemoryBytes(0x8000, 0x00, 6);

    EXPECT_EQ(got, content);
    EXPECT_FALSE(rig.master->GetSessionParameters().short_upload_available);
    EXPECT_GE(rig.slave.Count(CommandCode::SetMta), 1);
    EXPECT_GE(rig.slave.Count(CommandCode::Upload), 1);
}

TEST(XcpMasterIntegration, ConnectFailsWhenStatusUnavailable) {
    Rig rig;
    rig.slave.SetError(CommandCode::GetStatus, ErrorCode::CmdBusy);
    EXPECT_THROW(rig.master->Connect(), XcpException);
    EXPECT_FALSE(rig.master->IsConnected());
    // 失败清理：通道必须被关闭（计划 §5.1）
    EXPECT_FALSE(rig.transport_ptr->IsOpen());
}

TEST(XcpMasterIntegration, ConnectFailsOnMalformedConnectResponse) {
    Rig rig;
    rig.transport_ptr->SetResponse([&rig](BytesView p) -> Bytes {
        if (static_cast<CommandCode>(p[0]) == CommandCode::Connect) {
            return Bytes{static_cast<std::uint8_t>(PacketType::Res),
                         0x15};  // 长度不足
        }
        return rig.slave(p);
    });
    EXPECT_THROW(rig.master->Connect(), XcpException);
    EXPECT_FALSE(rig.master->IsConnected());
    EXPECT_FALSE(rig.transport_ptr->IsOpen());
}

// --------------------------------------------------------------------------
// §9.5 第 3 条：按 AG / MAX_CTO 多块 UPLOAD
// --------------------------------------------------------------------------

struct AgIntegrationCase {
    AddressGranularity ag;
    std::uint8_t max_cto;
};

class XcpMasterAgTest : public ::testing::TestWithParam<AgIntegrationCase> {};

TEST_P(XcpMasterAgTest, MultiChunkReadMatchesMockMemory) {
    const auto ag = GetParam().ag;
    const auto max_cto = GetParam().max_cto;
    const auto ag_bytes = AgToBytes(ag);

    Rig rig(MockXcpSlave(ag, ByteOrder::Intel, max_cto, 0x0100U));
    const ElementCount total =
        static_cast<ElementCount>(max_cto / ag_bytes) * 3U;
    const auto total_bytes = static_cast<std::size_t>(total) * ag_bytes;
    Bytes content(total_bytes);
    for (std::size_t i = 0; i < total_bytes; ++i) {
        content[i] = static_cast<std::uint8_t>((i * 7U) & 0xFFU);
    }
    rig.slave.SetMemory(0x10000, content);

    rig.master->Connect();
    const Bytes got = rig.master->ReadMemory(0x10000, 0x00, total);
    EXPECT_EQ(got, content);
    EXPECT_GT(rig.slave.Count(CommandCode::Upload), 1) << "应发生多块 UPLOAD";
}

TEST_P(XcpMasterAgTest, ByteApiRequiresMultipleOfAg) {
    const auto ag = GetParam().ag;
    if (AgToBytes(ag) == 1U) {
        GTEST_SKIP() << "AG=BYTE 时任意字节数都合法";
    }
    Rig rig(MockXcpSlave(ag, ByteOrder::Intel, GetParam().max_cto, 0x0100U));
    rig.master->Connect();
    EXPECT_THROW((void)rig.master->ReadMemoryBytes(0x1000, 0x00, 1),
                 XcpException);
}

INSTANTIATE_TEST_SUITE_P(
    AgIntegration, XcpMasterAgTest,
    ::testing::Values(AgIntegrationCase{AddressGranularity::Byte, 0x08U},
                      AgIntegrationCase{AddressGranularity::Word, 0x08U},
                      AgIntegrationCase{AddressGranularity::DWord, 0x08U},
                      AgIntegrationCase{AddressGranularity::DWord, 0x20U}),
    [](const ::testing::TestParamInfo<AgIntegrationCase>& info) {
        return std::string("Ag") + std::to_string(AgToBytes(info.param.ag)) +
               "_Cto" + std::to_string(info.param.max_cto);
    });

// --------------------------------------------------------------------------
// §9.5 第 4 条：等待 RES 时插入 EV/SERV
// --------------------------------------------------------------------------

TEST(XcpMasterIntegration, EventsDuringReadDoNotBreakMatching) {
    EventRecorder recorder;
    Rig rig(MockXcpSlave{}, CommandTimeouts{}, &recorder);
    const Bytes content = BytesOf({0xAA, 0xBB, 0xCC, 0xDD});
    rig.slave.SetMemory(0x5000, content);
    rig.master->Connect();

    // SHORT_UPLOAD 期间先注入 EV 与 SERV，再给最终 RES
    rig.transport_ptr->SetResponse([&rig](BytesView p) -> Bytes {
        if (static_cast<CommandCode>(p[0]) == CommandCode::ShortUpload) {
            const Bytes ev{static_cast<std::uint8_t>(PacketType::Ev),
                           static_cast<std::uint8_t>(EventCode::ResumeMode)};
            rig.transport_ptr->InjectPacket(BytesView{ev});
            const Bytes serv{static_cast<std::uint8_t>(PacketType::Serv), 0x07};
            rig.transport_ptr->InjectPacket(BytesView{serv});
        }
        return rig.slave(p);
    });

    const Bytes got = rig.master->ReadMemoryBytes(0x5000, 0x00, 4);
    EXPECT_EQ(got, content);
    EXPECT_GE(recorder.events(), 1);
    EXPECT_EQ(recorder.services(), 1);
}

// --------------------------------------------------------------------------
// §9.5 第 5 条：多个 EV_CMD_PENDING 后收到最终 RES
// --------------------------------------------------------------------------

TEST(XcpMasterIntegration, MultipleCmdPendingThenFinalResponse) {
    EventRecorder recorder;
    Rig rig(MockXcpSlave{},
            CommandTimeouts{std::chrono::milliseconds(600),
                            std::chrono::milliseconds(200), 2},
            &recorder);
    const Bytes content = BytesOf({0x01, 0x02, 0x03, 0x04});
    rig.slave.SetMemory(0x6000, content);
    rig.master->Connect();

    // SHORT_UPLOAD 的 send 不直接回响应，完全依赖注入序列
    rig.transport_ptr->SetResponse([&rig](BytesView p) -> Bytes {
        if (static_cast<CommandCode>(p[0]) == CommandCode::ShortUpload) {
            return {};
        }
        return rig.slave(p);
    });

    // 后台注入 3 个 EV_CMD_PENDING 与 1 个最终
    // RES；用守卫保证任何异常路径下都能 join
    std::thread injector([&rig] {
        for (int i = 0; i < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            const Bytes ev{static_cast<std::uint8_t>(PacketType::Ev),
                           static_cast<std::uint8_t>(EventCode::CmdPending)};
            rig.transport_ptr->InjectPacket(BytesView{ev});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        // RES = [FF][data...]：请求 4 字节，故响应数据恰好 4 字节（无计数字节）
        const Bytes res{static_cast<std::uint8_t>(PacketType::Res), 0x01, 0x02,
                        0x03, 0x04};
        rig.transport_ptr->InjectPacket(BytesView{res});
    });
    ThreadJoiner joiner(injector);

    const Bytes got = rig.master->ReadMemoryBytes(0x6000, 0x00, 4);
    injector.join();

    EXPECT_EQ(got, content);
    EXPECT_EQ(recorder.cmdPending(), 3);
    EXPECT_EQ(rig.slave.Count(CommandCode::Synch), 0)
        << "EV_CMD_PENDING 不应触发恢复";
}

// --------------------------------------------------------------------------
// §9.5 第 6 条：中间块 Timeout，经 SYNCH 和 SET_MTA 后成功
// --------------------------------------------------------------------------

TEST(XcpMasterIntegration, ChunkTimeoutRecoversViaSynchAndSetMta) {
    Rig rig;
    const auto max_cto = 8U;
    const ElementCount total = max_cto * 2U;  // AG=BYTE，需 3 块
    Bytes content(total);
    for (std::size_t i = 0; i < total; ++i) {
        content[i] = static_cast<std::uint8_t>(0xA0U + i);
    }
    rig.slave.SetMemory(0x20000, content);
    rig.slave.DropCommandTimes(CommandCode::Upload, 1);  // 第 1 次 UPLOAD 丢包

    rig.master->Connect();
    const Bytes got = rig.master->ReadMemory(0x20000, 0x00, total);

    EXPECT_EQ(got, content) << "超时恢复后应读出完整数据";
    EXPECT_GE(rig.slave.Count(CommandCode::Synch), 1);
    // 恢复重试前必须重建 MTA（计划 §5.3）
    EXPECT_GE(rig.slave.Count(CommandCode::SetMta), 2);
}

TEST(XcpMasterIntegration, UnrecoverableTimeoutReportsRecoveryFailed) {
    // 第 1 次 UPLOAD 丢包后，恢复重试仍丢包 -> 耗尽
    Rig rig(MockXcpSlave{}, CommandTimeouts{std::chrono::milliseconds(100),
                                            std::chrono::milliseconds(100), 1});
    // 元素数必须超过 MAX_CTO/AG(=8)，否则会走 SHORT_UPLOAD 而非 UPLOAD
    const ElementCount total = 20U;
    rig.slave.SetMemory(0x30000, Bytes(total, 0x5AU));
    rig.slave.DropCommand(CommandCode::Upload);

    rig.master->Connect();
    try {
        (void)rig.master->ReadMemory(0x30000, 0x00, total);
        FAIL() << "应报 RecoveryFailed";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::RecoveryFailed);
    }
}

// --------------------------------------------------------------------------
// §9.5 第 7 条：畸形包与错误分类
// --------------------------------------------------------------------------

TEST(XcpMasterIntegration, AccessLockedSurfacesProtocolError) {
    Rig rig;
    rig.slave.SetMemory(0x4000, BytesOf({0x01, 0x02, 0x03, 0x04}));
    rig.slave.SetError(CommandCode::ShortUpload, ErrorCode::AccessLocked);
    rig.master->Connect();
    try {
        (void)rig.master->ReadMemoryBytes(0x4000, 0x00, 4);
        FAIL() << "ERR_ACCESS_LOCKED 应映射为 ProtocolError（批次 7）";
    } catch (const XcpException& e) {
        // 批次 7：Seed&Key 解锁已落地，读命令遇锁以 ProtocolError 报告，
        // 指引调用方显式调用 XcpMaster::Unlock()（计划 §6.4 不自动解锁）
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::AccessLocked));
    }
}

TEST(XcpMasterIntegration, AccessDeniedSurfacesProtocolError) {
    Rig rig;
    rig.master->Connect();  // 未配置内存 -> Slave 回 ERR_ACCESS_DENIED
    try {
        (void)rig.master->ReadMemoryBytes(0x9000, 0x00, 4);
        FAIL() << "ERR_ACCESS_DENIED 应映射为 ProtocolError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::AccessDenied));
    }
}

TEST(XcpMasterIntegration, MalformedResponseLengthRejected) {
    Rig rig;
    rig.master->Connect();
    rig.transport_ptr->SetResponse([&rig](BytesView p) -> Bytes {
        if (static_cast<CommandCode>(p[0]) == CommandCode::ShortUpload) {
            return Bytes{static_cast<std::uint8_t>(PacketType::Res),
                         0x01};  // 长度不符
        }
        return rig.slave(p);
    });
    try {
        (void)rig.master->ReadMemoryBytes(0x1000, 0x00, 4);
        FAIL() << "响应长度不符应报 MalformedPacket";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::MalformedPacket);
    }
}

TEST(XcpMasterIntegration, TransportSendFailureClassified) {
    Rig rig;
    rig.master->Connect();
    rig.transport_ptr->FailNextSend();
    try {
        (void)rig.master->QueryStatus();
        FAIL() << "发送失败应报 TransportError";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::TransportError);
    }
}

TEST(XcpMasterIntegration, ReadBeforeConnectIsInvalidState) {
    Rig rig;
    try {
        (void)rig.master->ReadMemoryBytes(0x1000, 0x00, 4);
        FAIL() << "未连接读取应报 InvalidState";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
}

TEST(XcpMasterIntegration, ConnectTwiceIsRejected) {
    Rig rig;
    rig.master->Connect();
    EXPECT_THROW(rig.master->Connect(), XcpException);
    EXPECT_TRUE(rig.master->IsConnected());
}

TEST(XcpMasterIntegration, NullTransportRejected) {
    try {
        XcpMaster master(nullptr, XcpMasterOptions{});
        FAIL() << "空 Transport 应被拒绝";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

// --------------------------------------------------------------------------
// 变量标定批次（docs §7.5.2.5 / §7.5.3）：XcpMaster API + Mock Slave 编排
// --------------------------------------------------------------------------

TEST(XcpCalibration, ConnectProbesPagProcessorInfoWhenCalPagPresent) {
    // CONNECT RESOURCE 含 CAL/PAG → 连接期自动 GET_PAG_PROCESSOR_INFO，
    // 结果缓存进 Session
    Rig rig;
    rig.slave.SetResponse(CommandCode::GetPagProcessorInfo,
                          Res({0x04, 0x01}));  // MAX_SEGMENT=4, FREEZE 支持
    rig.master->Connect();
    EXPECT_EQ(rig.slave.Count(CommandCode::GetPagProcessorInfo), 1);
    const auto cached = rig.master->QueryPagProcessorInfo();
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(cached->max_segment, 0x04U);
    EXPECT_TRUE(HasPagProperty(cached->properties,
                               PagPropertyBit::kFreezeSupported));
    // 取证缓存生效：第二次查询不再发命令
    (void)rig.master->QueryPagProcessorInfo();
    EXPECT_EQ(rig.slave.Count(CommandCode::GetPagProcessorInfo), 1);
}

TEST(XcpCalibration, SlaveWithoutPagingDegradesWithoutBreakingConnect) {
    // Optional 面缺失（ERR_CMD_UNKNOWN）→ nullopt 缓存、连接不阻断（计划 §6.2）
    Rig rig;  // 默认 Slave 对未知命令回 ERR_CMD_UNKNOWN
    rig.master->Connect();
    EXPECT_TRUE(rig.master->IsConnected());
    EXPECT_FALSE(rig.master->QueryPagProcessorInfo().has_value());
}

TEST(XcpCalibration, CalPagAbsentRejectsAllCalibrationApis) {
    // CONNECT RESOURCE 不含 CAL/PAG(bit0) → XcpMaster 本地拒绝，不发命令。
    // Slave 默认 RESOURCE 含 CAL/PAG；SetResourceMask 必须在 Connect() 前生效。
    Rig rig;
    rig.slave.SetResourceMask(static_cast<std::uint8_t>(Resource::Daq));
    rig.master->Connect();
    EXPECT_FALSE(rig.master->HasCalPagResource());
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();
    try {
        rig.master->SetCalPage(CalPageModeBit::kEcu | CalPageModeBit::kXcp, 0,
                               1);
        FAIL() << "无 CAL/PAG 资源应报 UnsupportedFeature";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
    try {
        (void)rig.master->GetCalPage(CalPageAccessMode::Ecu, 0);
        FAIL() << "无 CAL/PAG 资源应报 UnsupportedFeature";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
    try {
        rig.master->ModifyBits(0x1000, 0x00, 0, 0xFFFF, 0x00FF);
        FAIL() << "无 CAL/PAG 资源应报 UnsupportedFeature";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
    EXPECT_EQ(rig.transport_ptr->SentPackets().size(), sent_before)
        << "本地拒绝不应发出任何命令";
}

TEST(XcpCalibration, ModifyBitsAppliesAndXorMasksViaMtaSequence) {
    // SET_MTA + MODIFY_BITS 序列；读回验证位操作结果，MTA 不被 MODIFY_BITS 修改
    Rig rig;
    rig.slave.SetMemory(0x1000, BytesOf({0xFF, 0xFF, 0xFF, 0xFF}));
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    rig.master->ModifyBits(0x1000, 0x00, /*shift=*/8, /*and=*/0x00FF,
                           /*xor=*/0x0F00);
    // 期望报文顺序：SET_MTA → MODIFY_BITS
    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 2U);
    EXPECT_EQ(sent[sent_before][0],
              static_cast<std::uint8_t>(CommandCode::SetMta));
    ASSERT_GE(sent[sent_before + 1].size(), 6U);
    EXPECT_EQ(sent[sent_before + 1][0],
              static_cast<std::uint8_t>(CommandCode::ModifyBits));
    EXPECT_EQ(sent[sent_before + 1][1], 0x08U);

    const Bytes got = rig.master->ReadMemoryBytes(0x1000, 0x00, 4);
    // Value=0xFFFFFFFF; AND=(0x00FF<<8)=0x0000FF00 → 0x0000FF00;
    // XOR=(0x0F00<<8)=0x000F0000 → 0x000FFF00（Intel 字节序）
    EXPECT_EQ(got, BytesOf({0x00, 0xFF, 0x0F, 0x00}));
}

TEST(XcpCalibration, ModifyBitsTimeoutDroppedBeforeExecutionIsNotRetried) {
    // 命令执行前丢包：执行结果仍按协议边界报告未知，SYNCH 后不重放。
    Rig rig{MockXcpSlave{},
            CommandTimeouts{std::chrono::milliseconds(120),
                            std::chrono::milliseconds(120), 2}};
    rig.slave.SetMemory(0x2000, BytesOf({0x00, 0x00, 0x00, 0x00}));
    rig.slave.DropNthCall(CommandCode::ModifyBits, 1);
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    try {
        rig.master->ModifyBits(0x2000, 0x00, /*shift=*/0, /*and=*/0xFFFF,
                               /*xor=*/0x00F0);
        FAIL() << "超时的 MODIFY_BITS 应报告结果未知";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::OperationOutcomeUnknown);
        EXPECT_EQ(e.GetCommandCode(),
                  std::optional<CommandCode>(CommandCode::ModifyBits));
        EXPECT_EQ(e.RetryCount(), 1);
    }

    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 3U);
    EXPECT_EQ(sent[sent_before][0],
              static_cast<std::uint8_t>(CommandCode::SetMta));
    EXPECT_EQ(sent[sent_before + 1][0],
              static_cast<std::uint8_t>(CommandCode::ModifyBits));
    EXPECT_EQ(sent[sent_before + 2][0],
              static_cast<std::uint8_t>(CommandCode::Synch));
    EXPECT_EQ(rig.slave.Count(CommandCode::ModifyBits), 1);
    EXPECT_EQ(rig.master->ReadMemoryBytes(0x2000, 0x00, 4),
              BytesOf({0x00, 0x00, 0x00, 0x00}));
}

TEST(XcpCalibration, ModifyBitsAppliedResponseLostIsNotReplayed) {
    // 对手端先应用 XOR，再丢弃响应；若 Master 重放，结果会被翻转回原值。
    Rig rig{MockXcpSlave{},
            CommandTimeouts{std::chrono::milliseconds(120),
                            std::chrono::milliseconds(120), 2}};
    rig.slave.SetMemory(0x2000, BytesOf({0x00, 0x00, 0x00, 0x00}));
    rig.slave.DropResponseAfterExecuteNthCall(CommandCode::ModifyBits, 1);
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    try {
        rig.master->ModifyBits(0x2000, 0x00, /*shift=*/0, /*and=*/0xFFFF,
                               /*xor=*/0x00F0);
        FAIL() << "响应丢失的 MODIFY_BITS 应报告结果未知";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::OperationOutcomeUnknown);
        EXPECT_EQ(e.GetCommandCode(),
                  std::optional<CommandCode>(CommandCode::ModifyBits));
        EXPECT_EQ(e.RetryCount(), 1);
    }

    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 3U);
    EXPECT_EQ(sent[sent_before][0],
              static_cast<std::uint8_t>(CommandCode::SetMta));
    EXPECT_EQ(sent[sent_before + 1][0],
              static_cast<std::uint8_t>(CommandCode::ModifyBits));
    EXPECT_EQ(sent[sent_before + 2][0],
              static_cast<std::uint8_t>(CommandCode::Synch));
    EXPECT_EQ(rig.slave.Count(CommandCode::ModifyBits), 1);
    EXPECT_EQ(rig.master->ReadMemoryBytes(0x2000, 0x00, 4),
              BytesOf({0xF0, 0x00, 0x00, 0x00}));
}

template <typename Operation>
void ExpectUnknownMutationTimeout(CommandCode command, Operation&& operation) {
    try {
        operation();
        FAIL() << "响应丢失的标定变更应报告 OperationOutcomeUnknown";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::OperationOutcomeUnknown);
        EXPECT_EQ(e.GetCommandCode(), std::optional<CommandCode>(command));
        EXPECT_EQ(e.RetryCount(), 1);
    }
}

TEST(XcpCalibration, DownloadAppliedResponseLostIsNotReplayed) {
    Rig rig{MockXcpSlave{},
            CommandTimeouts{std::chrono::milliseconds(120),
                            std::chrono::milliseconds(120), 2}};
    const Bytes data = BytesOf({0x11, 0x22, 0x33, 0x44});
    rig.slave.SetMemory(0x3000, BytesOf({0x00, 0x00, 0x00, 0x00}));
    rig.slave.DropResponseAfterExecuteNthCall(CommandCode::Download, 1);
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    ExpectUnknownMutationTimeout(CommandCode::Download, [&] {
        rig.master->WriteMemoryBytes(0x3000, 0x00, BytesView{data});
    });

    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 3U);
    EXPECT_EQ(sent[sent_before][0], static_cast<std::uint8_t>(CommandCode::SetMta));
    EXPECT_EQ(sent[sent_before + 1][0],
              static_cast<std::uint8_t>(CommandCode::Download));
    EXPECT_EQ(sent[sent_before + 2][0],
              static_cast<std::uint8_t>(CommandCode::Synch));
    EXPECT_EQ(rig.slave.Count(CommandCode::Download), 1);
    EXPECT_EQ(rig.master->ReadMemoryBytes(0x3000, 0x00, 4), data);
}

TEST(XcpCalibration, ShortDownloadAppliedResponseLostIsNotReplayed) {
    Rig rig{MockXcpSlave(AddressGranularity::Byte, ByteOrder::Intel, 16U),
            CommandTimeouts{std::chrono::milliseconds(120),
                            std::chrono::milliseconds(120), 2}};
    const Bytes data = BytesOf({0x51, 0x62, 0x73, 0x84});
    rig.slave.SetMemory(0x3100, BytesOf({0x00, 0x00, 0x00, 0x00}));
    rig.slave.DropResponseAfterExecuteNthCall(CommandCode::ShortDownload, 1);
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    ExpectUnknownMutationTimeout(CommandCode::ShortDownload, [&] {
        rig.master->WriteMemoryBytes(0x3100, 0x00, BytesView{data});
    });

    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 2U);
    EXPECT_EQ(sent[sent_before][0],
              static_cast<std::uint8_t>(CommandCode::ShortDownload));
    EXPECT_EQ(sent[sent_before + 1][0],
              static_cast<std::uint8_t>(CommandCode::Synch));
    EXPECT_EQ(rig.slave.Count(CommandCode::ShortDownload), 1);
    EXPECT_EQ(rig.master->ReadMemoryBytes(0x3100, 0x00, 4), data);
}

TEST(XcpCalibration, SetCalPageAppliedResponseLostIsNotReplayed) {
    Rig rig{MockXcpSlave{},
            CommandTimeouts{std::chrono::milliseconds(120),
                            std::chrono::milliseconds(120), 2}};
    rig.slave.DropResponseAfterExecuteNthCall(CommandCode::SetCalPage, 1);
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    ExpectUnknownMutationTimeout(CommandCode::SetCalPage, [&] {
        rig.master->SetCalPage(CalPageModeBit::kEcu, 1, 2);
    });

    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 2U);
    EXPECT_EQ(sent[sent_before][0],
              static_cast<std::uint8_t>(CommandCode::SetCalPage));
    EXPECT_EQ(sent[sent_before + 1][0],
              static_cast<std::uint8_t>(CommandCode::Synch));
    EXPECT_EQ(rig.slave.Count(CommandCode::SetCalPage), 1);
    const auto page = rig.master->GetCalPage(CalPageAccessMode::Ecu, 1);
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->page, 2U);
}

TEST(XcpCalibration, SetSegmentModeAppliedResponseLostIsNotReplayed) {
    Rig rig{MockXcpSlave{},
            CommandTimeouts{std::chrono::milliseconds(120),
                            std::chrono::milliseconds(120), 2}};
    rig.slave.DropResponseAfterExecuteNthCall(CommandCode::SetSegmentMode, 1);
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    ExpectUnknownMutationTimeout(CommandCode::SetSegmentMode, [&] {
        rig.master->SetSegmentFreeze(true, 2);
    });

    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 2U);
    EXPECT_EQ(sent[sent_before][0],
              static_cast<std::uint8_t>(CommandCode::SetSegmentMode));
    EXPECT_EQ(sent[sent_before + 1][0],
              static_cast<std::uint8_t>(CommandCode::Synch));
    EXPECT_EQ(rig.slave.Count(CommandCode::SetSegmentMode), 1);
    const auto mode = rig.master->GetSegmentMode(2);
    ASSERT_TRUE(mode.has_value());
    EXPECT_TRUE(HasSegmentMode(mode->mode, SegmentModeBit::kFreeze));
}

TEST(XcpCalibration, CopyCalPageAppliedResponseLostIsNotReplayed) {
    Rig rig{MockXcpSlave{},
            CommandTimeouts{std::chrono::milliseconds(120),
                            std::chrono::milliseconds(120), 2}};
    const Bytes source = BytesOf({0xA1, 0xB2, 0xC3});
    rig.slave.SetCalibrationPageData(0, 0, source);
    rig.slave.SetCalibrationPageData(0, 1, BytesOf({0x00, 0x00, 0x00}));
    rig.slave.DropResponseAfterExecuteNthCall(CommandCode::CopyCalPage, 1);
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    ExpectUnknownMutationTimeout(CommandCode::CopyCalPage, [&] {
        rig.master->CopyCalPage(CopyCalPageRequest{0, 0, 0, 1});
    });

    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 2U);
    EXPECT_EQ(sent[sent_before][0],
              static_cast<std::uint8_t>(CommandCode::CopyCalPage));
    EXPECT_EQ(sent[sent_before + 1][0],
              static_cast<std::uint8_t>(CommandCode::Synch));
    EXPECT_EQ(rig.slave.Count(CommandCode::CopyCalPage), 1);
    EXPECT_EQ(rig.slave.CalibrationPageData(0, 1), source);
}

TEST(XcpCalibration, SetCalPageGoldenPacketOnWire) {
    Rig rig;
    rig.slave.SetResponse(CommandCode::SetCalPage, Res({}));
    rig.master->Connect();
    const std::size_t sent_before = rig.transport_ptr->SentPackets().size();

    rig.master->SetCalPage(CalPageModeBit::kEcu | CalPageModeBit::kXcp, 1, 2);
    const auto& sent = rig.transport_ptr->SentPackets();
    ASSERT_EQ(sent.size(), sent_before + 1U);
    EXPECT_EQ(sent[sent_before], BytesOf({0xEB, 0x03, 0x01, 0x02}));
}

TEST(XcpCalibration, GetCalPageDecodesActivePage) {
    Rig rig;
    rig.slave.SetResponse(CommandCode::GetCalPage,
                          Res({0x00, 0x00, 0x07}));
    rig.master->Connect();
    const auto resp = rig.master->GetCalPage(CalPageAccessMode::Ecu, 0);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->page, 0x07U);
}

TEST(XcpCalibration, GetSegmentInfoModesDecodeByRequestMode) {
    Rig rig;
    rig.master->Connect();
    // Mode 0：DWORD 基本信息（剥 FF 后 4 字节）
    rig.slave.SetResponse(CommandCode::GetSegmentInfo,
                          Res({0x00, 0x31, 0x00, 0x00, 0x00, 0x00}));
    const auto basic = rig.master->GetSegmentInfo(SegmentInfoMode::BasicInfo, 1,
                                                  SegmentInfoSelector::SegmentAddress);
    ASSERT_TRUE(basic.has_value());
    ASSERT_TRUE(std::holds_alternative<SegmentBasicInfo>(basic->data));
    EXPECT_EQ(std::get<SegmentBasicInfo>(basic->data).value, 0x00003100U);
    // Mode 1：标准属性 5 字段
    rig.slave.SetResponse(CommandCode::GetSegmentInfo,
                          Res({0x08, 0x02, 0x03, 0x01, 0x00}));
    const auto props = rig.master->GetSegmentInfo(
        SegmentInfoMode::StandardProperties, 1,
        SegmentInfoSelector::SegmentAddress);
    ASSERT_TRUE(props.has_value());
    ASSERT_TRUE(
        std::holds_alternative<SegmentStandardProperties>(props->data));
    EXPECT_EQ(std::get<SegmentStandardProperties>(props->data).max_pages,
              0x08U);
}

TEST(XcpCalibration, GetPageInfoAndSegmentModeRoundTrip) {
    Rig rig;
    rig.slave.SetResponse(CommandCode::GetPageInfo, Res({0x27, 0x05}));
    rig.slave.SetResponse(CommandCode::SetSegmentMode, Res({}));
    rig.slave.SetResponse(CommandCode::GetSegmentMode, Res({0x00, 0x01}));
    rig.master->Connect();

    const auto info = rig.master->GetPageInfo(0, 1);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->properties.ecu_access, PageAccessType::DontCare);
    EXPECT_EQ(info->init_segment, 0x05U);

    rig.master->SetSegmentFreeze(true, 2);
    const auto mode = rig.master->GetSegmentMode(2);
    ASSERT_TRUE(mode.has_value());
    EXPECT_TRUE(HasSegmentMode(mode->mode, SegmentModeBit::kFreeze));
}

TEST(XcpCalibration, CopyCalPageWriteProtectedSurfacesProtocolError) {
    Rig rig;
    rig.slave.SetError(CommandCode::CopyCalPage, ErrorCode::WriteProtected);
    rig.master->Connect();
    try {
        rig.master->CopyCalPage(CopyCalPageRequest{0, 0, 0, 1});
        FAIL() << "写保护应作为协议错误上抛（提示走 Flash Programming）";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        EXPECT_EQ(e.GetErrorCode(),
                  std::optional<ErrorCode>(ErrorCode::WriteProtected));
    }
}

TEST(XcpCalibration, CalibrationApiBeforeConnectIsInvalidState) {
    Rig rig;
    try {
        (void)rig.master->GetCalPage(CalPageAccessMode::Ecu, 0);
        FAIL() << "未连接调用标定 API 应报 InvalidState";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
}

// --------------------------------------------------------------------------
// 版本发现与裁决（批次 B1）
//
// MockXcpSlave 的 default 分支回 ERR_CMD_UNKNOWN，因此**默认即模拟不支持
// GET_VERSION 的对端**：版本应停在 ConnectMajor + MajorOnly 保守态。
//
// CONNECT 响应里硬编码 PL=0x15、TL=0x10。这两个字节是 CONNECT 响应的既有
// 硬编码值，并非刻意构造的版本号（0x15 与 RESOURCE 掩码默认值撞号，见
// `m_resource_mask_{0x15U}`）。原始 PDF 第 12 页 §3.1.1 规定 CONNECT 响应
// 该字节是「XCP Protocol Layer Version Number (most significant byte
// only)」——XCPlite 实现印证此解：`XCP_PROTOCOL_LAYER_VERSION` 为 0x0104，
// 发出时 `>> 8` 得 0x01（thirdparty/xcplite/src/xcplite.c:2078）。因此本
// mock 的 0x15 按规范解读为**主版本 21**，裁决器只据此判定"主版本可用、
// Minor 未确认"，绝不会当成精确版本——下列断言正是钉住这一点。
// --------------------------------------------------------------------------

/// @brief 默认（GET_VERSION 未实现）对端：探测被拒 → 保守态，且不伪造 Minor
TEST(XcpMasterVersion, UnimplementedGetVersionYieldsConservativeState) {
    Rig rig;
    rig.master->Connect();

    const ProtocolVersionSnapshot snap = rig.master->GetProtocolVersionSnapshot();
    // 证据来源必须是 CONNECT 主版本，而不是"运行时确认"
    EXPECT_EQ(snap.evidence.source, VersionSource::ConnectMajor);
    EXPECT_EQ(snap.evidence.confidence, VersionConfidence::MajorOnly);
    EXPECT_FALSE(snap.evidence.minor_confirmed);
    EXPECT_TRUE(snap.conservative);
    // CONNECT 原始字节 0x15 就是主版本本身（§3.1.1：只给最高字节）
    EXPECT_EQ(snap.evidence.reported.major, 0x15U);
    // Minor 恒为占位 0：**不是**声称对端是 21.0
    EXPECT_EQ(snap.evidence.reported.minor, 0U);
    // 保守态生效版本取支持下限 V1.3（能力口径，非对端真实版本）
    EXPECT_EQ(snap.effective, kProtocolVersion13);

    const TransportVersionSnapshot tls =
        rig.master->GetTransportVersionSnapshot();
    EXPECT_EQ(tls.evidence.source, VersionSource::ConnectMajor);
    EXPECT_TRUE(tls.conservative);
}

/// @brief GET_VERSION 支持时取得精确 PL/TL 版本
TEST(XcpMasterVersion, AvailableGetVersionYieldsRuntimeConfirmedVersions) {
    Rig rig;
    // 注入 GET_VERSION 响应：[Reserved][Maj PL][Min PL][Maj TL][Min TL]
    rig.slave.SetResponse(CommandCode::GetVersion,
                          BytesOf({0xFF, 0x00, 0x01, 0x05, 0x01, 0x04}));
    rig.master->Connect();

    const ProtocolVersionSnapshot ps = rig.master->GetProtocolVersionSnapshot();
    EXPECT_EQ(ps.evidence.source, VersionSource::GetVersion);
    EXPECT_EQ(ps.evidence.confidence, VersionConfidence::RuntimeConfirmed);
    EXPECT_TRUE(ps.evidence.minor_confirmed);
    EXPECT_FALSE(ps.conservative);
    EXPECT_EQ(ps.evidence.reported, (XcpProtocolVersion{1, 5}));
    EXPECT_EQ(ps.effective, (XcpProtocolVersion{1, 5}));

    // TL 必须独立取值（1.4），不得被 PL 的 1.5 覆盖
    const TransportVersionSnapshot ts =
        rig.master->GetTransportVersionSnapshot();
    EXPECT_EQ(ts.evidence.reported, (XcpProtocolVersion{1, 4}));
    EXPECT_EQ(ts.effective, (XcpProtocolVersion{1, 4}));
}

/// @brief 探测确实被发到了线路上（不是"看起来像"而是可观测行为）
TEST(XcpMasterVersion, ConnectActuallySendsGetVersionProbe) {
    Rig rig;
    rig.master->Connect();
    EXPECT_EQ(rig.slave.Count(CommandCode::GetVersion), 1U)
        << "Connect 应恰好探测一次 GET_VERSION";
}

/// @brief 上报版本高于支持上限时封顶，且 conflcits 可见
TEST(XcpMasterVersion, VersionAboveCapIsCappedAndReported) {
    Rig rig;
    rig.slave.SetResponse(CommandCode::GetVersion,
                          BytesOf({0xFF, 0x00, 0x01, 0x09, 0x01, 0x09}));
    rig.master->Connect();

    const ProtocolVersionSnapshot ps = rig.master->GetProtocolVersionSnapshot();
    // reported 保留对端原话
    EXPECT_EQ(ps.evidence.reported, (XcpProtocolVersion{1, 9}));
    EXPECT_TRUE(ps.evidence.capped);
    // effective 压到上限
    EXPECT_EQ(ps.effective, kProtocolVersion15);

    const std::vector<std::string> conflicts = rig.master->VersionConflicts();
    EXPECT_FALSE(conflicts.empty()) << "封顶必须产生可见冲突记录";
}

/// @brief 关闭自动探测后不得发送 GET_VERSION，版本退回 CONNECT 主版本
TEST(XcpMasterVersion, AutodetectDisabledSkipsProbe) {
    auto transport = std::make_unique<test::MockTransport>();
    MockXcpSlave slave;
    transport->SetResponse([&slave](BytesView p) { return slave(p); });
    XcpMasterOptions options;
    options.autodetect_version = false;
    XcpMaster master(std::move(transport), options);

    master.Connect();

    EXPECT_EQ(slave.Count(CommandCode::GetVersion), 0U)
        << "关闭自动探测后不得发送 GET_VERSION";
    const ProtocolVersionSnapshot ps = master.GetProtocolVersionSnapshot();
    EXPECT_EQ(ps.evidence.source, VersionSource::ConnectMajor);
    EXPECT_TRUE(ps.conservative);
}

/// @brief 超时不得被当作"确认的老版本"（方案 B1 硬约束的集成验证）
TEST(XcpMasterVersion, ProbeTimeoutIsNotTreatedAsConfirmedOldVersion) {
    Rig rig;
    rig.slave.DropCommand(CommandCode::GetVersion);  // 永不响应 → 超时
    rig.master->Connect();

    const ProtocolVersionSnapshot ps = rig.master->GetProtocolVersionSnapshot();
    // 关键：来源不是 GetVersion、置信度不是 RuntimeConfirmed、Minor 未确认
    EXPECT_NE(ps.evidence.source, VersionSource::GetVersion);
    EXPECT_NE(ps.evidence.confidence, VersionConfidence::RuntimeConfirmed);
    EXPECT_FALSE(ps.evidence.minor_confirmed);
    // 连接本身必须成功（optional 命令失败不阻断连接）
    EXPECT_TRUE(rig.master->IsConnected());
}

/// @brief 连接必须成功：GET_VERSION 不可用不构成连接失败
TEST(XcpMasterVersion, ConnectSucceedsEvenWhenProbeFails) {
    Rig rig;
    rig.slave.SetError(CommandCode::GetVersion, ErrorCode::CmdUnknown);
    EXPECT_NO_THROW(rig.master->Connect());
    EXPECT_TRUE(rig.master->IsConnected());
}

/// @brief require_get_version=true 时探测失败必须连接失败
TEST(XcpMasterVersion, RequireGetVersionFailsConnectWhenUnavailable) {
    auto transport = std::make_unique<test::MockTransport>();
    MockXcpSlave slave;
    transport->SetResponse([&slave](BytesView p) { return slave(p); });
    XcpMasterOptions options;
    options.require_get_version = true;
    XcpMaster master(std::move(transport), options);

    try {
        master.Connect();
        FAIL() << "require_get_version 为真且探测失败时应抛异常";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
    EXPECT_FALSE(master.IsConnected());
}

/// @brief 断连后版本证据必须清空，不得跨会话残留
TEST(XcpMasterVersion, VersionEvidenceIsClearedOnDisconnect) {
    Rig rig;
    rig.slave.SetResponse(CommandCode::GetVersion,
                          BytesOf({0xFF, 0x00, 0x01, 0x05, 0x01, 0x04}));
    rig.master->Connect();
    ASSERT_EQ(rig.master->GetProtocolVersionSnapshot().evidence.source,
              VersionSource::GetVersion);

    rig.master->Disconnect();

    // 新会话前必须回到 Unknown：否则重连到不同版本对端会沿用旧裁决
    const ProtocolVersionSnapshot ps = rig.master->GetProtocolVersionSnapshot();
    EXPECT_EQ(ps.evidence.source, VersionSource::Unknown);
    EXPECT_FALSE(ps.evidence.IsAvailable());
    EXPECT_FALSE(ps.conservative);
    EXPECT_TRUE(rig.master->VersionConflicts().empty());
}

/// @brief 断连再连到另一个版本的对端时，必须采用新对端的版本
TEST(XcpMasterVersion, ReconnectAdoptsNewPeerVersion) {
    auto transport = std::make_unique<test::MockTransport>();
    MockXcpSlave slave;
    transport->SetResponse([&slave](BytesView p) { return slave(p); });
    XcpMaster master(std::move(transport), XcpMasterOptions{});

    slave.SetResponse(CommandCode::GetVersion,
                      BytesOf({0xFF, 0x00, 0x01, 0x03, 0x01, 0x03}));
    master.Connect();
    ASSERT_EQ(master.GetProtocolVersionSnapshot().evidence.reported,
              (XcpProtocolVersion{1, 3}));

    master.Disconnect();
    // 换成 V1.5 对端
    slave.SetResponse(CommandCode::GetVersion,
                      BytesOf({0xFF, 0x00, 0x01, 0x05, 0x01, 0x05}));
    master.Connect();

    EXPECT_EQ(master.GetProtocolVersionSnapshot().evidence.reported,
              (XcpProtocolVersion{1, 5}))
        << "重连后必须采用新对端版本，不得沿用旧裁决";
}

/// @brief 调用方配置与运行时冲突时必须记录（m03674：不提供静默模式）
TEST(XcpMasterVersion, ConfiguredVersusRuntimeConflictIsVisible) {
    auto transport = std::make_unique<test::MockTransport>();
    MockXcpSlave slave;
    transport->SetResponse([&slave](BytesView p) { return slave(p); });
    slave.SetResponse(CommandCode::GetVersion,
                      BytesOf({0xFF, 0x00, 0x01, 0x05, 0x01, 0x05}));
    XcpMasterOptions options;
    options.version_policy.protocol_version = XcpProtocolVersion{1, 3};
    XcpMaster master(std::move(transport), options);

    master.Connect();

    // 运行时值优先
    EXPECT_EQ(master.GetProtocolVersionSnapshot().evidence.reported,
              (XcpProtocolVersion{1, 5}));
    const std::vector<std::string> conflicts = master.VersionConflicts();
    ASSERT_FALSE(conflicts.empty()) << "配置与运行时不一致必须可见";
}

/// @brief 主动补测：构造时关闭自动探测，之后 ProbeVersion() 仍可取得版本
TEST(XcpMasterVersion, ExplicitProbeVersionUpdatesSnapshot) {
    auto transport = std::make_unique<test::MockTransport>();
    MockXcpSlave slave;
    transport->SetResponse([&slave](BytesView p) { return slave(p); });
    XcpMasterOptions options;
    options.autodetect_version = false;
    XcpMaster master(std::move(transport), options);
    master.Connect();
    ASSERT_TRUE(master.GetProtocolVersionSnapshot().conservative);

    slave.SetResponse(CommandCode::GetVersion,
                      BytesOf({0xFF, 0x00, 0x01, 0x04, 0x01, 0x04}));
    const auto probe = master.ProbeVersion();

    EXPECT_TRUE(probe.IsAvailable());
    const ProtocolVersionSnapshot ps = master.GetProtocolVersionSnapshot();
    EXPECT_EQ(ps.evidence.source, VersionSource::GetVersion);
    EXPECT_EQ(ps.evidence.reported, (XcpProtocolVersion{1, 4}));
    EXPECT_FALSE(ps.conservative);
}

/// @brief 构造期拒绝自相矛盾的版本策略
TEST(XcpMasterVersion, ContradictoryPolicyIsRejectedAtConstruction) {
    auto transport = std::make_unique<test::MockTransport>();
    XcpMasterOptions options;
    options.version_policy.probe_get_version = true;
    options.version_policy.allow_version_probe_roundtrip = false;
    try {
        XcpMaster master(std::move(transport), options);
        FAIL() << "自相矛盾的版本策略应在构造期被拒绝";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

// =============================================================================
// 批次 G：`1.x-conservative` 能力基线（方案 §4.2.1 冻结表）
//
// 这组用例给方案 §4.2.1 表中判为 **Block** 的分组分派 DoD 测试 ID。
// 全部为**本地判定**——不依赖对端响应，不需要 Slave 配合。
// =============================================================================

/// @brief `VersionUnknown.HighVersionBlocked`：0xC0 族除 GET_VERSION 外全部 Block
/// @details 原始 PDF 第 106 页 §7.4：Level-1 命令以双字节 0xC0 起始；
///          本实现只登记 GET_VERSION（0xC0,0x00）。方案要求"禁止内容探测式
///          尝试"——即不得靠发送命令看错误码来推断能力。
TEST(VersionUnknown_HighVersionBlocked, DaqPackedModeIsBlockedLocally) {
    auto transport = std::make_unique<test::MockTransport>();
    XcpMasterOptions options;
    options.version_policy.probe_get_version = false;
    XcpMaster master(std::move(transport), options);

    using Cap = XcpMaster::UnsupportedCapability;
    EXPECT_TRUE(master.IsCapabilityUnsupported(Cap::DaqPackedMode))
        << "DAQ Packed Mode 是 V1.5 新特性且本实现未提供编码路径";
    EXPECT_TRUE(master.IsCapabilityUnsupported(Cap::Level1OtherSubcommand));
}

/// @brief `VersionUnknown.HighVersionBlocked`：拒绝入口抛 UnsupportedFeature
/// @details 关键区分：能力缺失必须报 `UnsupportedFeature`，**不能**报
///          ProtocolError/Timeout——后者会被调用方误判为对端故障。
TEST(VersionUnknown_HighVersionBlocked, ThrowIfUnsupportedRaisesUnsupported) {
    auto transport = std::make_unique<test::MockTransport>();
    XcpMasterOptions options;
    options.version_policy.probe_get_version = false;
    XcpMaster master(std::move(transport), options);

    using Cap = XcpMaster::UnsupportedCapability;
    for (const Cap cap : {Cap::DaqPackedMode, Cap::Level1OtherSubcommand,
                          Cap::Ipv6Transport, Cap::OptionalTlSubcommand}) {
        try {
            master.ThrowIfUnsupported(cap);
            FAIL() << "明确不支持的能力应抛异常（cap="
                   << static_cast<int>(cap) << "）";
        } catch (const XcpException& e) {
            EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature)
                << "必须报 UnsupportedFeature 而非对端故障类别";
        }
    }
}

/// @brief `TransportUnknown.OptionalBlocked`：四个 Optional TL Subcommand Block
TEST(TransportUnknown_OptionalBlocked, OptionalTlSubcommandIsBlockedLocally) {
    auto transport = std::make_unique<test::MockTransport>();
    XcpMasterOptions options;
    options.version_policy.probe_get_version = false;
    XcpMaster master(std::move(transport), options);

    EXPECT_TRUE(master.IsCapabilityUnsupported(
        XcpMaster::UnsupportedCapability::OptionalTlSubcommand));
}

/// @brief `TransportUnknown.OptionalBlocked`：IPv6 归属能力缺失而非"地址笔误"
/// @details 与 `src/udp_transport.cpp` 的 `looksLikeIpv6` 拒绝同口径：
///          能力缺失必须可与"地址非法"区分，否则调用方会误以为是自己写错了。
TEST(TransportUnknown_OptionalBlocked, Ipv6IsReportedAsUnsupportedNotInvalid) {
    auto transport = std::make_unique<test::MockTransport>();
    XcpMasterOptions options;
    options.version_policy.probe_get_version = false;
    XcpMaster master(std::move(transport), options);

    EXPECT_TRUE(master.IsCapabilityUnsupported(
        XcpMaster::UnsupportedCapability::Ipv6Transport));

    try {
        master.ThrowIfUnsupported(
            XcpMaster::UnsupportedCapability::Ipv6Transport);
        FAIL() << "IPv6 应抛 UnsupportedFeature";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature)
            << "IPv6 是能力缺失，不得报成 InvalidArgument（地址非法）";
    }
}

/// @brief `VersionUnknown.CoreSession`：Minor 未确认时核心会话仍可建立
/// @details 保守基线的**目的**是"少做"而不是"不能用"：CONNECT/DISCONNECT/
///          GET_STATUS/SYNCH 属 Allow，Minor unknown 不得妨碍会话建立。
TEST(VersionUnknown_CoreSession, ConservativeModeStillConnects) {
    Rig rig;
    // 让 GET_VERSION 不可用（规范允许的 normal return，原始 PDF 第 106 页 §7.4），
    // 于是只剩 CONNECT 主版本证据 → confidence=MajorOnly、minor_confirmed=false
    // → 进入 1.x-conservative。
    rig.slave.SetError(CommandCode::GetVersion, ErrorCode::CmdUnknown);
    rig.master->Connect();

    EXPECT_TRUE(rig.master->IsConnected());
    const ProtocolVersionSnapshot ps = rig.master->GetProtocolVersionSnapshot();
    EXPECT_TRUE(ps.conservative) << "Minor 未确认时应进入保守能力状态";
    EXPECT_FALSE(ps.evidence.minor_confirmed);
    // 保守态下 effective 取支持范围下限 {1,3} 作为**能力口径**
    // （src/version_resolver.cpp:138），这是"按保守交集执行能力"的标识，
    // 不代表对端真的上报了 V1.3。
    //
    // ⚠ 已知语义张力（用户裁决 m06013：保持现状 + 文档明写）：
    //   方案 §4.2.1 要求保守时 Minor "保持 unknown"，而实现令
    //   effective.minor=3。二者靠 `conservative` 标志与
    //   `confidence==MajorOnly` 区分，信息未丢失，但**调用方不得只看
    //   effective**——必须同时检查 conservative，否则会把保守口径误读为
    //   "对端是 V1.3"。本断言锁定当前行为，防止无声漂移。
    EXPECT_EQ(ps.effective, (XcpProtocolVersion{1, 3}))
        << "保守态 effective 现为下限口径；若改为'未知'表示法需同步本用例";
    EXPECT_EQ(ps.evidence.confidence, VersionConfidence::MajorOnly)
        << "区分保守口径与真 V1.3 的判据：置信度必须为 MajorOnly";
    // 会话命令确实被发出（Allow 语义的可观测证据）
    EXPECT_EQ(rig.slave.Count(CommandCode::Connect), 1)
        << "保守状态不得阻断 CONNECT";
}

/// @brief `VersionUnknown.Memory`：保守状态下内存读走替代路径
/// @details DoD 要求"实践替代路径"：SHORT_UPLOAD 遇 ERR_CMD_UNKNOWN 时应
///          降级到 SET_MTA+UPLOAD，而不是整体失败。
TEST(VersionUnknown_Memory, ConservativeModeMemoryReadUsesAlternativePath) {
    Rig rig;
    rig.slave.SetError(CommandCode::GetVersion, ErrorCode::CmdUnknown);
    rig.master->Connect();
    EXPECT_TRUE(rig.master->GetProtocolVersionSnapshot().conservative)
        << "GET_VERSION 不可用时须进入保守能力状态";

    // SHORT_UPLOAD 不可用（模拟对端未实现）→ 应自动走 SET_MTA+UPLOAD
    // 替代路径，而不是整体失败。复用本文件既有的 MockXcpSlave 脚本，
    // 避免手工拼响应而与真实 UPLOAD 语义（元素计数/MTA 推进）脱节。
    const Bytes content = BytesOf({0x11, 0x22, 0x33, 0x44});
    rig.slave.SetMemory(0x2000, content);
    rig.slave.SetShortUploadUnsupported();

    const Bytes got = rig.master->ReadMemoryBytes(0x2000, 0x00, 4);
    EXPECT_EQ(got, content)
        << "保守状态下内存读应走替代路径并取回正确数据";
    EXPECT_FALSE(rig.master->GetSessionParameters().short_upload_available);
    EXPECT_GE(rig.slave.Count(CommandCode::SetMta), 1)
        << "替代路径必须先 SET_MTA 再 UPLOAD";
    EXPECT_GE(rig.slave.Count(CommandCode::Upload), 1);
}


// =============================================================================
// 批次 G：G.3 三版本测试（V1.3 / V1.4 / V1.5 + 越界封顶）
//
// 目的：证明工程支持范围 **V1.3～V1.5** 的每一个目标版本都能被正确发现、
// 正确取到 PL/TL 两个独立值，且**越界版本一律封顶并留下可见冲突**。
// =============================================================================

/// @brief G.3：三个目标版本逐一验证——reported 与 effective 均如实取值
/// @details 使用参数化思路但保持单用例内自校验，避免新增夹具类型。
///          响应布局：[Reserved][PL Maj][PL Min][TL Maj][TL Min]
TEST(XcpMasterVersion, ThreeVersionMatrixDiscoveredFaithfully) {
    struct Case {
        std::uint8_t pl_major;
        std::uint8_t pl_minor;
        XcpProtocolVersion expected;
    };
    // V1.3（工程支持范围下限）、V1.4（XCPlite 对手端）、V1.5（范围上限）
    const Case cases[] = {
        {0x01, 0x03, XcpProtocolVersion{1, 3}},
        {0x01, 0x04, XcpProtocolVersion{1, 4}},
        {0x01, 0x05, XcpProtocolVersion{1, 5}},
    };

    for (const Case& c : cases) {
        Rig rig;
        rig.slave.SetResponse(
            CommandCode::GetVersion,
            BytesOf({0xFF, 0x00, c.pl_major, c.pl_minor, 0x01, 0x05}));
        rig.master->Connect();

        const ProtocolVersionSnapshot ps =
            rig.master->GetProtocolVersionSnapshot();
        EXPECT_EQ(ps.evidence.source, VersionSource::GetVersion);
        EXPECT_EQ(ps.evidence.confidence, VersionConfidence::RuntimeConfirmed);
        EXPECT_FALSE(ps.conservative)
            << "范围内版本不得被标为保守态";
        EXPECT_FALSE(ps.evidence.capped)
            << "范围内版本不得被标为封顶";
        EXPECT_EQ(ps.evidence.reported, c.expected);
        EXPECT_EQ(ps.effective, c.expected);

        // TL 独立取值：本例固定 TL=1.5，不得被 PL 覆盖
        const TransportVersionSnapshot ts =
            rig.master->GetTransportVersionSnapshot();
        EXPECT_EQ(ts.effective, (XcpProtocolVersion{1, 5}))
            << "TL 必须独立于 PL 取值";
    }
}

/// @brief G.3：低于支持下限（V1.2）时不得伪报为 V1.3
/// @details 支持范围下限是 V1.3；对端若报 V1.2，reported 必须保留原话。
///          这是"不伪造版本"约束在下界方向的对称验证。
TEST(XcpMasterVersion, BelowSupportedFloorIsNotSilentlyPromoted) {
    Rig rig;
    rig.slave.SetResponse(CommandCode::GetVersion,
                          BytesOf({0xFF, 0x00, 0x01, 0x02, 0x01, 0x02}));
    rig.master->Connect();

    const ProtocolVersionSnapshot ps = rig.master->GetProtocolVersionSnapshot();
    EXPECT_EQ(ps.evidence.reported, (XcpProtocolVersion{1, 2}))
        << "对端原话必须保留，不得改写成 V1.3";
}

/// @brief G.3：V1.5 之上（V1.6/V2.0）必须封顶到 V1.5 且冲突可见
/// @details 上限 V1.5 是**工程决策**（规范无兼容矩阵，方案 §1.9）；
///          因此越界必须"可用但被记录"，不能静默接受也不能硬拒绝。
TEST(XcpMasterVersion, AboveCapIsCappedAndConflictVisibleForBoth) {
    struct Case {
        std::uint8_t pl_major;
        std::uint8_t pl_minor;
    };
    const Case cases[] = {{0x01, 0x06}, {0x02, 0x00}};

    for (const Case& c : cases) {
        Rig rig;
        rig.slave.SetResponse(
            CommandCode::GetVersion,
            BytesOf({0xFF, 0x00, c.pl_major, c.pl_minor, 0x01, 0x05}));
        rig.master->Connect();

        const ProtocolVersionSnapshot ps =
            rig.master->GetProtocolVersionSnapshot();
        EXPECT_EQ(ps.evidence.reported,
                  (XcpProtocolVersion{c.pl_major, c.pl_minor}))
            << "越界版本必须保留 reported 原话";
        EXPECT_TRUE(ps.evidence.capped)
            << "越界版本必须标记 capped";
        EXPECT_EQ(ps.effective, kProtocolVersion15)
            << "effective 必须压到工程上限 V1.5";
        EXPECT_FALSE(rig.master->VersionConflicts().empty())
            << "封顶必须产生可见冲突记录，不得静默";
    }
}

}  // namespace
}  // namespace calmcar::xcp
