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
#include <set>
#include <string>
#include <thread>
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

    /// @brief 写入模拟 ECU 内存
    void SetMemory(Address address, const Bytes& data) {
        for (std::size_t i = 0; i < data.size(); ++i) {
            m_memory_[address + static_cast<Address>(i)] = data[i];
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
        const auto err_it = m_errors_.find(cmd);
        if (err_it != m_errors_.end()) {
            return Err(err_it->second);
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
                if (packet.size() < 7U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_mta_ = decodeAddress(packet.subspan(3, 4));
                return Res({});
            case CommandCode::Upload:
                return upload(packet);
            case CommandCode::ShortUpload:
                return shortUpload(packet);
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
        std::vector<std::uint8_t> body{0x15, comm_mode, m_max_cto_};
        if (m_order_ == ByteOrder::Intel) {
            body.push_back(static_cast<std::uint8_t>(m_max_dto_ & 0xFFU));
            body.push_back(
                static_cast<std::uint8_t>((m_max_dto_ >> 8) & 0xFFU));
        } else {
            body.push_back(
                static_cast<std::uint8_t>((m_max_dto_ >> 8) & 0xFFU));
            body.push_back(static_cast<std::uint8_t>(m_max_dto_ & 0xFFU));
        }
        body.push_back(0x10U);
        body.push_back(0x10U);
        return Res(body);
    }

    Bytes upload(BytesView packet) {
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
        // （docs/XCP_1.3.0_document.md §12.4：UPLOAD(6) -> 6 字节）
        std::vector<std::uint8_t> body;
        body.insert(body.end(), data->begin(), data->end());
        m_mta_ += static_cast<Address>(elements) * AgToBytes(m_ag_);
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
        if (elements == 0U || elements > static_cast<ElementCount>(
                                             m_max_cto_ / AgToBytes(m_ag_))) {
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
        m_mta_ = address + static_cast<Address>(elements) * AgToBytes(m_ag_);
        return Res(body);
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

    AddressGranularity m_ag_;
    ByteOrder m_order_;
    std::uint8_t m_max_cto_;
    std::uint16_t m_max_dto_;
    bool m_optional_{true};
    bool m_connected_{false};
    bool m_short_upload_unsupported_{false};
    Address m_mta_{0};
    std::map<Address, std::uint8_t> m_memory_;
    std::map<CommandCode, ErrorCode> m_errors_;
    std::set<CommandCode> m_dropped_;
    std::map<CommandCode, int> m_drop_times_;
    std::map<CommandCode, int> m_counts_;
};

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
        master = std::make_unique<XcpMaster>(std::move(transport), timeouts,
                                             listener);
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

TEST(XcpMasterIntegration, AccessLockedSurfacesUnsupportedFeature) {
    Rig rig;
    rig.slave.SetMemory(0x4000, BytesOf({0x01, 0x02, 0x03, 0x04}));
    rig.slave.SetError(CommandCode::ShortUpload, ErrorCode::AccessLocked);
    rig.master->Connect();
    try {
        (void)rig.master->ReadMemoryBytes(0x4000, 0x00, 4);
        FAIL() << "ERR_ACCESS_LOCKED 应映射为 UnsupportedFeature";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
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
        XcpMaster master(nullptr);
        FAIL() << "空 Transport 应被拒绝";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidArgument);
    }
}

}  // namespace
}  // namespace calmcar::xcp
