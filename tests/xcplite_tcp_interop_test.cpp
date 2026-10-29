/**
 * @file xcplite_tcp_interop_test.cpp
 * @brief XCPlite 真实 TCP 互操作 Gate（计划 §8.2）。
 *
 * @par 为什么必须用真实进程
 *   loopback 只能证明"我方发送与接收自洽"，无法发现"我方假设与 XCPlite 实际
 *   线路布局不符"。本文件让 `xcp_test_slave`（由 `thirdparty/XCPlite`
 *   构建，使用其自己的 `xcpEthTl.c` framing）以 TCP 方式监听，Master 侧走
 *   `TcpTransport` + `AsamV14` Profile，全程真实 socket、真实编解码，不替换
 *   任何一侧的 codec。
 *
 * @par 覆盖范围（对应计划 §8.2 条目）
 *   - Connect/Disconnect 与版本证据（PL/TL 主版本为 1）；
 *   - GET_STATUS 与 Polling Upload；
 *   - 标定写入并回读；
 *   - Dynamic DAQ：Select + START_STOP_SYNCH，DTO 依 ODT layout 解码，
 *     并容忍 XCPlite 把帧补白到 4 字节倍数（补白计入 wire LEN）；
 *   - DISCONNECT 后 socket 关闭、可再次建链；
 *   - 对端进程退出后我方**不得**静默重连。
 *
 * @par 不在本文件覆盖的条目及原因
 *   - "多 DTO 单次 send / 任意 recv 分片 / 人为制造队列 lost 只告警"：这些
 *     需要控制对端的发送节奏与丢帧时机，由 `tcp_test_slave`（B-T3）的故障
 *     注入在 `xcp_tcp_loopback_test.cpp` 中确定性覆盖；
 *   - AsamV15 独立对端：计划 §8.3 已允许本期以"自有 loopback 已验证"收尾。
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/daq/dto_envelope_decoder.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/tcp_framing_candidates.hpp"
#include "libxcp/tcp_framing_negotiation.hpp"
#include "libxcp/tcp_framing_verification.hpp"
#include "libxcp/tcp_framing_verdict.hpp"
#include "libxcp/tcp_transport.hpp"
#include "libxcp/tcp_transport_config.hpp"
#include "libxcp/tcp_wire_profile.hpp"
#include "libxcp/xcp_master.hpp"

#include "libxcp/transport_selection.hpp"
#include "xcplite_slave_fixture.hpp"
#include "xcp_test_slave/xcplite_test_types.hpp"

namespace calmcar::xcp {
namespace {

using test::ResolveA2lSymbol;
using test::XcpliteSlaveProcess;

/// @brief 采集 DTO 的监听器（只在收包线程拷贝，断言留给主线程）
class DtoCollector final : public IEventListener {
public:
    void OnEvent(const EventPacket&) override {}
    void OnService(const ServicePacket&) override {}
    void OnDto(const DtoPacket& dto) override {
        std::lock_guard<std::mutex> lock(m_mutex_);
        m_frames_.push_back(dto.data);
    }

    /// @brief 已收到的 DTO 帧数
    [[nodiscard]] std::size_t Count() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_frames_.size();
    }

    /// @brief DTO 帧快照
    [[nodiscard]] std::vector<Bytes> Snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_frames_;
    }

    /// @brief 等待累计帧数达到下界
    [[nodiscard]] bool WaitFor(std::size_t at_least,
                               std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (Count() < at_least) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

private:
    mutable std::mutex m_mutex_;
    std::vector<Bytes> m_frames_;
};

/// @brief 把标量按小端编码为字节（两侧同机，会话恒为小端）
template <typename T>
Bytes LeBytes(const T& value) {
    Bytes out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

/// @brief 期望值的**前缀**比较（XCPlite 会把 DTO 净荷补白到 4 字节倍数）
bool BytesPrefixEq(BytesView payload, BytesView want) {
    return payload.size() >= want.size() &&
           std::memcmp(payload.data(), want.data(), want.size()) == 0;
}

/// @brief XCPlite DTO 信封布局（DAQ_KEY_BYTE=0xC0 → (WORD, aligned)）
/// @details 识别字段为 `[PID(relODT)][FILL][DAQ(WORD)]` 共 4 字节；4 字节 LE
///          时间戳只随本事件**首个** ODT 帧出现。
DtoFrameLayout MakeXcpliteEnvelopeLayout() {
    DtoFrameLayout layout;
    layout.identification_field_type =
        IdentificationFieldType::RelativeWordAligned;
    layout.first_odt = 0;
    layout.counter_enabled = false;
    layout.timestamp_enabled = true;
    layout.timestamp_size_bits = 32U;
    layout.timestamp_relative_first_only = true;
    layout.header_bytes = 4U;
    return layout;
}

/// @brief 用核心解码器切分一个 DTO 帧；非法形态返回空
std::optional<DtoEnvelope> DecodeFrame(const Bytes& frame) {
    static const DtoEnvelopeDecoder decoder(ByteOrder::Intel);
    static const DtoFrameLayout layout = MakeXcpliteEnvelopeLayout();
    try {
        return decoder.Decode(BytesView{frame}, layout);
    } catch (const XcpException&) {
        return std::nullopt;
    }
}

/**
 * @brief TCP 互操作夹具：以 XCPlite 的 TCP 服务启动 Slave
 * @details 与 UDP 夹具的差别集中在两处——子进程参数加 `--tcp`，就绪探测与
 *          建链都用 `TcpTransport`。就绪探测若不跟着换传输方式，会一直探测
 *          不成功并把"环境问题"误报成"Slave 未就绪"。
 */
class XcpliteTcpInteropTest : public ::testing::Test {
protected:
    /// @brief 启动 TCP Slave 并校验 A2L 确实声明了 TCP 端点
    void SetUp() override {
        try {
            slave_.Start();
        } catch (const std::exception& e) {
            FAIL() << "XCPlite TCP Slave 启动失败: " << e.what();
            return;
        }
        ASSERT_TRUE(slave_.UsesTcp()) << "本文件必须跑在 TCP 上";
        // A2L 里的传输段落决定了夹具与其它工具读取到的端点类型；若 XCPlite
        // 仍写出 XCP_ON_UDP_IP，说明 --tcp 没有真正传到 A2lInit。
        std::ifstream a2l(slave_.A2lPath());
        ASSERT_TRUE(a2l) << "A2L 不存在: " << slave_.A2lPath().string();
        std::string text((std::istreambuf_iterator<char>(a2l)),
                         std::istreambuf_iterator<char>());
        ASSERT_NE(text.find("XCP_ON_TCP_IP"), std::string::npos)
            << "A2L 未声明 XCP_ON_TCP_IP，本次并非 TCP 互操作";

        // Start 已用真实 CONNECT 探测就绪，不将其误计为零连接。
        // 只提取 TCP 端点，显式线路参数不依赖 A2L 对齐推断。
        auto loaded = a2l::A2lBridge::Load(slave_.A2lPath().string());
        ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
        const auto* info = loaded.Value()->XcpInfo();
        ASSERT_NE(info, nullptr);
        ASSERT_FALSE(info->ambiguous);
        calmcar::xcp::A2lTransportFacts facts;
        for (const auto& endpoint : info->transports) {
            if (endpoint.kind != a2l::TransportEndpoint::Kind::TcpIp) continue;
            if (!facts.declared) {
                facts.declared = true;
                facts.host = endpoint.remote_host;
                facts.port = endpoint.remote_port;
            }
            ++facts.instance_count;
            facts.instances.push_back({endpoint.remote_host, endpoint.remote_port,
                                       0, endpoint.version});
        }
        ASSERT_TRUE(facts.declared) << "A2L 未声明 TCP 端点";
        ASSERT_EQ(facts.host, "127.0.0.1");
        ASSERT_EQ(facts.port, slave_.Port());
        calmcar::xcp::TransportSelectionRequest request;
        request.transport = calmcar::xcp::TransportKind::Tcp;
        request.tcp_profile = TcpWireProfile::AsamV14;
        request.packet_alignment_bits = std::uint8_t{8};
        std::string error;
        selection_ = calmcar::xcp::ResolveTransportSelection(request, facts, &error);
        ASSERT_TRUE(selection_.has_value()) << error;
        ASSERT_EQ(selection_->endpoint_source, calmcar::xcp::EndpointSource::A2l);
        ASSERT_EQ(selection_->host, facts.host);
        ASSERT_EQ(selection_->port, facts.port);
        ASSERT_EQ(selection_->wire_profile, TcpWireProfile::AsamV14);
        ASSERT_EQ(selection_->alignment, PacketAlignment::Bit8);
    }

    /// @brief 终止 Slave 进程
    void TearDown() override { slave_.Stop(); }

    /**
     * @brief 构造指向 Slave 的 TCP Master（未连接）
     * @param listener 事件监听器（非拥有，生命周期须长于 Master）
     * @return 持有独立 TcpTransport 的 Master
     * @details 使用 `AsamV14` Profile：XCPlite 的线路布局是
     *          `[LEN=Packet 长度][CTR][Packet]`，LEN 之外没有 Tail。
     */
    [[nodiscard]] std::unique_ptr<XcpMaster> MakeMaster(
        IEventListener* listener = nullptr) const {
        TcpTransportConfig cfg;
        cfg.role = TcpRole::Master;
        cfg.remote_host = selection_->host;
        cfg.remote_port = selection_->port;
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;
        // XCPlite 的 xcptl_cfg.h 为 PACKET_ALIGNMENT 4；在 asam-v1.4 下该
        // 值只影响对端内部的补白，不改变本侧 framing。
        cfg.packet_alignment = selection_->alignment;
        cfg.wire_profile = selection_->wire_profile;
        cfg.connect_timeout_ms = 3000;
        cfg.send_timeout_ms = 3000;
        cfg.keep_alive = true;

        XcpMasterOptions options;
        // 真实进程对手端：命令超时放宽到 2s（本机回环正常时远低于此）
        options.timeouts = CommandTimeouts{std::chrono::milliseconds(2000),
                                           std::chrono::milliseconds(2000), 1};
        options.event_listener = listener;
        return std::make_unique<XcpMaster>(std::make_unique<TcpTransport>(cfg),
                                           options);
    }

    /// @brief 构造并已 CONNECT 的 Master
    [[nodiscard]] std::unique_ptr<XcpMaster> MakeConnectedMaster(
        IEventListener* listener = nullptr) const {
        auto master = MakeMaster(listener);
        master->Connect();
        return master;
    }

    std::optional<calmcar::xcp::ResolvedTransportSelection> selection_;  ///< 已验证的连接决策。
    /// @brief Slave 进程句柄
    XcpliteSlaveProcess slave_{std::nullopt, 0, /*use_tcp=*/true};
};

// ---------------------------------------------------------------------------
// 1) 建链与版本证据
// ---------------------------------------------------------------------------

/**
 * @brief TCP 建链成功，且 PL/TL 主版本如实为 1。
 * @details 本条只断言"对端事实"：XCPlite 的版本宏是 0x0104/0x0104，主版本
 *          为 1；具体 Minor 是否被运行时确认取决于 GET_VERSION 是否实现。
 */
TEST_F(XcpliteTcpInteropTest, ConnectOverTcpReportsMajorOne) {
    auto master = MakeConnectedMaster();

    EXPECT_TRUE(master->IsConnected());
    EXPECT_EQ(master->GetSessionState(), SessionState::Connected);

    const ProtocolVersionSnapshot pl = master->GetProtocolVersionSnapshot();
    const TransportVersionSnapshot tl = master->GetTransportVersionSnapshot();
    EXPECT_EQ(pl.evidence.reported.major, 1U);
    EXPECT_EQ(tl.evidence.reported.major, 1U);
    EXPECT_TRUE(pl.evidence.IsAvailable())
        << "已建链的对端必须给出可用的版本证据";

    // TCP 上 XCPlite 未启用 Seed&Key，保护位应为 0
    const GetStatusResponse status = master->QueryStatus();
    EXPECT_EQ(status.resource_protection, 0U);

    master->Disconnect();
    EXPECT_FALSE(master->IsConnected());
}

/**
 * @brief 显式 GET_VERSION 在 TCP 上同样应得到精确版本。
 * @details 版本探测走的是命令通道，与传输介质无关；若这里失败而 UDP 通过，
 *          说明 TCP 侧的 CRM 收发或 CTR 校验有问题。
 */
TEST_F(XcpliteTcpInteropTest, GetVersionOverTcp) {
    auto master = MakeConnectedMaster();

    const auto probe = master->ProbeVersion();
    ASSERT_EQ(probe.outcome, CommandExecutor::GetVersionOutcome::Available)
        << "XCPlite 实现了 GET_VERSION，TCP 上不可用说明命令通道异常";
    ASSERT_TRUE(probe.response.has_value());
    EXPECT_EQ(probe.response->ProtocolVersion().major, 1U);
}

// ---------------------------------------------------------------------------
// 2) Polling Upload
// ---------------------------------------------------------------------------

/**
 * @brief TCP 上的 Polling Upload 必须读到与 UDP 相同的内存内容。
 */
TEST_F(XcpliteTcpInteropTest, PollingUploadOverTcp) {
    auto master = MakeConnectedMaster();
    const auto a2l = slave_.A2lPath();

    const auto info = ResolveA2lSymbol(a2l, "g_basic_u32");
    ASSERT_TRUE(info.has_value()) << "A2L 未找到 g_basic_u32";
    const Bytes raw =
        master->ReadMemoryBytes(info->address, info->extension, 4);
    ASSERT_EQ(raw.size(), 4U);

    std::uint32_t value = 0;
    std::memcpy(&value, raw.data(), sizeof(value));
    EXPECT_EQ(value, test::kExpectBasicU32);
}

// ---------------------------------------------------------------------------
// 3) 标定写入与回读
// ---------------------------------------------------------------------------

/**
 * @brief TCP 上 DOWNLOAD 后可读回新值（写入路径不受传输介质影响）。
 */
TEST_F(XcpliteTcpInteropTest, CalibrationWriteAndReadBackOverTcp) {
    auto master = MakeConnectedMaster();
    const auto a2l = slave_.A2lPath();

    const auto info = ResolveA2lSymbol(a2l, "g_basic_u32");
    ASSERT_TRUE(info.has_value());

    const std::uint32_t magic = 0xC0FFEE11U;
    ASSERT_NO_THROW(master->WriteMemoryBytes(info->address, info->extension,
                                             LeBytes(magic)));

    const Bytes back =
        master->ReadMemoryBytes(info->address, info->extension, 4);
    ASSERT_EQ(back.size(), 4U);
    std::uint32_t value = 0;
    std::memcpy(&value, back.data(), sizeof(value));
    EXPECT_EQ(value, magic);
}

// ---------------------------------------------------------------------------
// 4) Dynamic DAQ（Select + START_STOP_SYNCH）
// ---------------------------------------------------------------------------

/**
 * @brief TCP 上动态建表并同步启动后应持续收到 DTO。
 * @details 断言刻意只做**前缀**比较：XCPlite 的 `QueueAcquire()` 会把 DTO
 *          净荷补白到 4 字节倍数，且补白计入 wire LEN（`xcpQueue32.c:199-205`
 *          与 `:208-215`），因此帧长可能大于 ODT 布局声明的长度。这正是计划
 *          §8.2 所说的"DTO LEN 内扩长，依据 ODT layout 解码真实数据"。
 */
TEST_F(XcpliteTcpInteropTest, DynamicDaqOverTcp) {
    DtoCollector collector;
    auto master = MakeConnectedMaster(&collector);

    const auto a2l = slave_.A2lPath();
    const auto u32_info = ResolveA2lSymbol(a2l, "g_basic_u32");
    const auto u8_info = ResolveA2lSymbol(a2l, "g_basic_u8");
    ASSERT_TRUE(u32_info.has_value());
    ASSERT_TRUE(u8_info.has_value());

    // 事件通道号必须来自 A2L，不能硬编码（XCPlite 的事件号由生成器决定）
    std::optional<std::uint16_t> event;
    {
        std::ifstream file(a2l);
        std::string line;
        while (std::getline(file, line)) {
            const std::size_t pos = line.find("\"testev\" 0x");
            if (pos == std::string::npos) {
                continue;
            }
            const std::size_t begin = pos + std::strlen("\"testev\" 0x");
            const std::size_t end = line.find(' ', begin);
            event = static_cast<std::uint16_t>(std::stoul(
                line.substr(begin, end == std::string::npos
                                       ? std::string::npos
                                       : end - begin),
                nullptr, 16));
            break;
        }
    }
    ASSERT_TRUE(event.has_value()) << "A2L EVENT 段未找到 testev";

    DaqEntrySpec u32_entry;
    u32_entry.address = u32_info->address;
    u32_entry.extension = u32_info->extension;
    u32_entry.size = 4;
    DaqOdtSpec odt0;
    odt0.entries.push_back(u32_entry);

    DaqEntrySpec u8_entry;
    u8_entry.address = u8_info->address;
    u8_entry.extension = u8_info->extension;
    u8_entry.size = 1;
    DaqOdtSpec odt1;
    odt1.entries.push_back(u8_entry);

    DaqListSpec spec;
    spec.daq_list = 0U;
    spec.event_channel = *event;
    spec.timestamp = true;  // XCPlite 的事件首 ODT 恒带 4 字节时间戳
    spec.odts.push_back(odt0);
    spec.odts.push_back(odt1);

    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({spec}));
    ASSERT_NO_THROW(master->StartDaqSync());
    ASSERT_TRUE(collector.WaitFor(6, std::chrono::seconds(3)))
        << "TCP 上启动 DAQ 后 3s 内 DTO 不足 6 帧";

    // 按 ODT layout 解码：帧长可能因 4 字节补白而大于布局声明长度，因此
    // 只对 payload 前缀做期望值比较。
    bool saw_u32 = false;
    bool saw_u8 = false;
    int odt0_frames = 0;
    int odt1_frames = 0;
    const Bytes want_u32 = LeBytes(test::kExpectBasicU32);
    for (const auto& frame : collector.Snapshot()) {
        const auto envelope = DecodeFrame(frame);
        ASSERT_TRUE(envelope.has_value())
            << "帧非 XCPlite 信封形态（识别字段 4 字节），长度=" << frame.size();
        EXPECT_EQ(envelope->identity.daq_list, 0U);
        if (envelope->identity.odt == 0U) {
            ++odt0_frames;
            EXPECT_TRUE(envelope->raw_timestamp.has_value())
                << "本事件首个 ODT 帧必须带时间戳";
            if (BytesPrefixEq(envelope->payload, BytesView{want_u32})) {
                saw_u32 = true;
            }
        } else {
            ++odt1_frames;
            EXPECT_FALSE(envelope->raw_timestamp.has_value())
                << "非首个 ODT 帧不得重复时间戳";
            if (!envelope->payload.empty() &&
                envelope->payload[0] == test::kExpectBasicU8) {
                saw_u8 = true;
            }
        }
    }
    EXPECT_GT(odt0_frames, 0) << "两个 ODT 至少应各出现一次";
    EXPECT_GT(odt1_frames, 0);
    EXPECT_TRUE(saw_u32) << "未见 g_basic_u32 的期望值 0xDEADBEEF";
    EXPECT_TRUE(saw_u8) << "未见 g_basic_u8 的期望值 0x42";

    ASSERT_NO_THROW(master->StopDaq());
}

// ---------------------------------------------------------------------------
// 5) 断链与重建
// ---------------------------------------------------------------------------

/**
 * @brief DISCONNECT 之后同一端口必须能再次建链。
 * @details XCPlite 在 TCP 上每次只接受一个连接；若 DISCONNECT 没真正关闭
 *          socket，第二次 CONNECT 会失败或超时。
 */
TEST_F(XcpliteTcpInteropTest, DisconnectThenReconnectOverTcp) {
    {
        auto master = MakeConnectedMaster();
        EXPECT_TRUE(master->IsConnected());
        master->Disconnect();
        EXPECT_FALSE(master->IsConnected());
    }
    auto second = MakeConnectedMaster();
    EXPECT_TRUE(second->IsConnected());
    const GetStatusResponse status = second->QueryStatus();
    EXPECT_EQ(status.resource_protection, 0U);
}

// ---------------------------------------------------------------------------
// 6) 对端退出：可观测失败，且不得静默重连
// ---------------------------------------------------------------------------

/**
 * @brief Slave 进程退出后，Master 的下一条命令必须失败而不是"假装成功"。
 * @details 计划 §8.2 明确"普通已连接命令超时不能视为合法成功"。本节同时
 *          守住"绝不静默重连"不变量：失败后 Master 仍处于已连接态，
 *          必须由调用方显式 Disconnect/Connect，不能自行恢复。
 */
TEST_F(XcpliteTcpInteropTest, PeerExitFailsCommandWithoutSilentReconnect) {
    auto master = MakeConnectedMaster();
    ASSERT_TRUE(master->IsConnected());

    // 先确认通道正常
    const auto info = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_u32");
    ASSERT_TRUE(info.has_value());
    ASSERT_NO_THROW(master->ReadMemoryBytes(info->address, info->extension, 4));

    // 直接杀掉对端进程（不经过 XCP DISCONNECT），模拟异常退出
    slave_.Stop();

    bool failed = false;
    try {
        master->ReadMemoryBytes(info->address, info->extension, 4);
    } catch (const XcpException&) {
        failed = true;
    }
    EXPECT_TRUE(failed)
        << "对端已退出，命令却成功返回——违反「不得把超时当成功」";
}

/**
 * @brief `--tcp-profile auto` 在真实 XCPlite V2.2 从站上必须落到 V1.4/8bit。
 *
 * @details 与 V1.2 用例互补：两个版本的 XCPlite 都是第三方 framing 实现，
 *          共同证明自动模式不是"只对自己家从站有效"。V2.2 的 8 bit 布局与
 *          V1.4 Profile 一致，因此默认顺序的第一个候选即应被接受。
 */
TEST_F(XcpliteTcpInteropTest, AutoProfileResolvesAsamV14EightFromV22Slave) {
    TcpFramingProbeConfig probe;
    probe.host = "127.0.0.1";
    probe.port = slave_.Port();
    probe.connect_timeout_ms = 3000U;
    probe.command_timeout_ms = 1200U;

    const auto candidates = DefaultFramingCandidates();
    const auto attempt =
        [&probe](const TcpFramingCandidate& candidate) {
            return VerifyTcpFramingCandidate(probe, candidate);
        };
    const auto outcome = NegotiateTcpFraming(candidates, attempt);

    ASSERT_TRUE(outcome.selected.has_value()) << outcome.Summary();
    EXPECT_EQ(outcome.selected->profile, TcpWireProfile::AsamV14)
        << outcome.Summary();
    EXPECT_EQ(outcome.selected->alignment, PacketAlignment::Bit8)
        << outcome.Summary();
    EXPECT_EQ(outcome.attempts.size(), 1U) << outcome.Summary();
}

}  // namespace
}  // namespace calmcar::xcp