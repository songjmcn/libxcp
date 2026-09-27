/**
 * @file xcp_daq_test.cpp
 * @brief 批次14（T14-06/07/08）：DAQ 编排、WRITE_DAQ 账本与写回的单元验收。
 *
 * 与 `a2l_e2e_test.cpp` 的分工：那边是"A2L + 真实 UDP Slave"的端到端闭环；
 * 这里是纯协议层的确定性断言（MockTransport + 脚本化 Slave），专门锁四件事：
 *   1) 每个 Entry 前都显式 SET_DAQ_PTR（不依赖 Slave 指针自增，docs L2172）；
 *   2) 超时恢复必须重放隐含状态（MTA / DAQ 指针，docs L2783）；
 *   3) 账本不留半份（配置失败即回滚）+ 会话级清理（断连补发 STOP、代际递增）；
 *   4) 写回的块上限与 SHORT_DOWNLOAD→DOWNLOAD 回落口径。
 */

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <memory>
#include <vector>

#include "libxcp/command_executor.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

using test::MockTransport;

/// @brief Slave 侧记账用的一条 WRITE_DAQ（由 Slave 的隐含指针推出）
struct ExpectedEntry {
    std::uint16_t daq;   ///< DAQ List 号
    std::uint8_t odt;    ///< ODT 号（0 基）
    std::uint8_t entry;  ///< Entry 号（0 基）
    Address address;     ///< 32 位地址
    std::uint8_t size;   ///< 元素数（AG 单位）
};

/**
 * @brief 脚本化 DAQ / 写回 Slave。
 * @details 只实现本文件需要的命令；其余按 docs L1631 回 ERR_CMD_UNKNOWN
 *          （且不产生副作用）。可按命令注入"前 N 次不响应"（驱动 SYNCH 恢复）
 *          与"持续某错误码"（驱动写保护/能力缺失分支）。
 */
class DaqSlave {
public:
    explicit DaqSlave(std::uint8_t max_cto = 16U) : m_max_cto_(max_cto) {}

    /// @brief 让某命令的前 N 次请求不响应（模拟丢包）
    void DropTimes(CommandCode cmd, int times) { m_drop_times_[cmd] = times; }
    /// @brief 让某命令持续返回指定错误码
    void FailWith(CommandCode cmd, ErrorCode code) { m_errors_[cmd] = code; }
    /// @brief 设定 START_STOP_DAQ_LIST 返回的 FIRST_PID
    void SetFirstPid(std::uint8_t pid) { m_first_pid_ = pid; }
    /// @brief 设定 GET_DAQ_PROCESSOR_INFO 返回的 DAQ_PROPERTIES
    void SetProcessorProperties(std::uint8_t properties) {
        m_processor_properties_ = properties;
        m_processor_info_supported_ = true;
    }
    /// @brief 某命令被收到的次数
    [[nodiscard]] int Count(CommandCode cmd) const {
        const auto it = m_counts_.find(cmd);
        return it == m_counts_.end() ? 0 : it->second;
    }
    /// @brief DOWNLOAD/SHORT_DOWNLOAD 落进的模拟内存（按字节地址归档）
    [[nodiscard]] const std::map<Address, std::uint8_t>& Memory() const {
        return m_memory_;
    }
    /// @brief 收到的 WRITE_DAQ 序列（指针三元组由 Slave 侧维护）
    [[nodiscard]] const std::vector<ExpectedEntry>& Written() const {
        return m_written_;
    }

    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        const auto cmd = static_cast<CommandCode>(packet[0]);
        ++m_counts_[cmd];
        const auto drop_it = m_drop_times_.find(cmd);
        if (drop_it != m_drop_times_.end() && drop_it->second > 0) {
            --drop_it->second;
            return {};  // 不响应 → 触发 SYNCH 恢复
        }
        const auto err_it = m_errors_.find(cmd);
        if (err_it != m_errors_.end()) {
            return Err(err_it->second);
        }

        switch (cmd) {
            case CommandCode::Connect:
                return Res({0x15, 0xC0, m_max_cto_, 0x08, 0x00, 0x10, 0x10});
            case CommandCode::Disconnect:
                return Res({});
            case CommandCode::GetStatus:
                // byte1 = Current Session Status（bit6 DAQ_RUNNING，docs
                // L2220）
                return Res(
                    {m_daq_running_ ? 0x40U : 0x00U, 0x00, 0x01, 0x07, 0x00});
            case CommandCode::GetCommModeInfo:
                return Res({0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13});
            case CommandCode::GetDaqProcessorInfo:
                if (!m_processor_info_supported_) {
                    return Err(ErrorCode::CmdUnknown);
                }
                // properties, MAX_DAQ=4, MAX_EVENT_CHANNEL=2, MIN_DAQ=0,
                // DAQ_KEY_BYTE=0（Absolute + DAQ 扩展模式）
                return Res({m_processor_properties_, 0x04, 0x00, 0x02, 0x00,
                            0x00, 0x00});
            case CommandCode::Synch:
                return Err(ErrorCode::CmdSynch);
            case CommandCode::SetMta:
                // XCP 1.3 布局：[F6][MODE][rsv][EXT@3][ADDR@4..7]（8 字节，
                // XCPlite 对手端协议调试核证）
                if (packet.size() < 8U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_mta_ = le32(packet.subspan(4, 4));
                return Res({});
            case CommandCode::ClearDaqList:
                if (packet.size() < 4U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_ptr_ = Ptr{};
                m_written_.clear();
                m_daq_running_ = false;
                return Res({});
            case CommandCode::SetDaqPtr:
                if (packet.size() < 6U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_ptr_.daq =
                    static_cast<std::uint16_t>(packet[2] | (packet[3] << 8));
                m_ptr_.odt = packet[4];
                m_ptr_.entry = packet[5];
                return Res({});
            case CommandCode::WriteDaq: {
                if (packet.size() < 8U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_written_.push_back(
                    ExpectedEntry{m_ptr_.daq, m_ptr_.odt, m_ptr_.entry,
                                  le32(packet.subspan(4, 4)), packet[2]});
                ++m_ptr_.entry;  // Slave 侧自增（docs L2172）
                return Res({});
            }
            case CommandCode::SetDaqListMode:
                if (packet.size() < 8U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                // RES LEN 6（xcp.h:719）
                return Res(
                    {packet[1], packet[2], packet[3], packet[4], packet[5]});
            case CommandCode::StartStopDaqList:
                if (packet.size() < 4U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_daq_running_ = packet[1] == 0x01U;
                return Res({m_first_pid_});
            case CommandCode::StartStopSynch:
                if (packet.size() < 2U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                m_daq_running_ =
                    packet[1] == 0x01U;  // 0=stop all、2=stop selected
                return Res({});
            case CommandCode::Download: {
                if (packet.size() < 2U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const auto elements = static_cast<std::size_t>(packet[1]);
                if (elements + 2U > packet.size()) {
                    return Err(ErrorCode::CmdSyntax);
                }
                // 先存块起点：边写边自增 m_mta_ 会让 "m_mta_ + i" 双重步进
                const Address base = m_mta_;
                for (std::size_t i = 0; i < elements; ++i) {
                    m_memory_[base + static_cast<Address>(i)] = packet[2 + i];
                }
                m_mta_ = base + static_cast<Address>(elements);  // docs L1983
                return Res({});
            }
            case CommandCode::ShortDownload: {
                if (packet.size() < 8U) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const auto elements = static_cast<std::size_t>(packet[1]);
                if (elements + 8U > packet.size()) {
                    return Err(ErrorCode::CmdSyntax);
                }
                const Address address = le32(packet.subspan(4, 4));
                for (std::size_t i = 0; i < elements; ++i) {
                    m_memory_[address + static_cast<Address>(i)] =
                        packet[8 + i];
                }
                return Res({});
            }
            default:
                return Err(ErrorCode::CmdUnknown);
        }
    }

private:
    /// @brief Slave 侧隐含 DAQ 指针
    struct Ptr {
        std::uint16_t daq{0};
        std::uint8_t odt{0};
        std::uint8_t entry{0};
    };

    static Bytes Res(const std::vector<std::uint8_t>& body) {
        Bytes out;
        out.reserve(1 + body.size());
        out.push_back(static_cast<std::uint8_t>(PacketType::Res));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    static Bytes Err(ErrorCode code) {
        return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                     static_cast<std::uint8_t>(code)};
    }
    static Address le32(BytesView four) {
        return static_cast<Address>(four[0]) |
               (static_cast<Address>(four[1]) << 8) |
               (static_cast<Address>(four[2]) << 16) |
               (static_cast<Address>(four[3]) << 24);
    }

    std::uint8_t m_max_cto_;
    Address m_mta_{0};
    Ptr m_ptr_;
    bool m_daq_running_{false};
    std::uint8_t m_first_pid_{0x20U};
    bool m_processor_info_supported_{false};
    std::uint8_t m_processor_properties_{0U};
    std::map<CommandCode, int> m_counts_;
    std::map<CommandCode, int> m_drop_times_;
    std::map<CommandCode, ErrorCode> m_errors_;
    std::vector<ExpectedEntry> m_written_;
    std::map<Address, std::uint8_t> m_memory_;
};

/**
 * @brief 夹具：MockTransport + 脚本化 Slave + 已连接的 XcpMaster。
 * @details Transport 所有权交给 XcpMaster（其构造要求），夹具只保留观察用裸
 *          指针。成员销毁顺序为 master → slave（声明顺序相反），因此
 *          ~XcpMaster 的尽力断开仍能拿到活的 Slave 与响应脚本。
 */
struct Rig {
    explicit Rig(std::uint8_t max_cto = 16U) : slave(max_cto) {
        auto transport_owner = std::make_unique<MockTransport>();
        transport = transport_owner.get();
        transport->SetResponse([this](BytesView p) { return slave(p); });
        master = std::make_unique<XcpMaster>(
            std::move(transport_owner),
            CommandTimeouts{std::chrono::milliseconds(150),
                            std::chrono::milliseconds(150), 2});
        master->Connect();
    }

    DaqSlave slave;
    MockTransport* transport{nullptr};
    std::unique_ptr<XcpMaster> master;
};

/// @brief 一份合法的两 Entry 配置（daq=1，odt0 内两条）
DaqListSpec MakeSpec() {
    DaqListSpec spec;
    spec.daq_list = 1;
    spec.event_channel = 2;
    spec.prescaler = 1;
    spec.priority = 0;
    DaqOdtSpec odt;
    odt.entries = {
        {0x3000, 0x00, 2, kDaqBitOffsetNone},
        {0x3020, 0x00, 1, kDaqBitOffsetNone},
    };
    spec.odts = {odt};
    return spec;
}

// ---------------------------------------------------------------------------
// 配置序列与账本
// ---------------------------------------------------------------------------

TEST(XcpDaqConfigure, IssuesSetDaqPtrBeforeEveryWriteDaq) {
    Rig rig;
    rig.master->ConfigureDaqList(MakeSpec());

    // 2 个 Entry → 1×CLEAR + 2×(SET_DAQ_PTR + WRITE_DAQ) + 1×SET_MODE
    EXPECT_EQ(rig.slave.Count(CommandCode::ClearDaqList), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::SetDaqPtr), 2)
        << "不依赖 Slave 的指针自增（docs L2172：写过末位后指针未定义）";
    EXPECT_EQ(rig.slave.Count(CommandCode::WriteDaq), 2);
    EXPECT_EQ(rig.slave.Count(CommandCode::SetDaqListMode), 1);

    const auto& ledger = rig.master->DaqLedger();
    ASSERT_EQ(ledger.size(), 2u);
    EXPECT_EQ(ledger[0].daq_list, 1);
    EXPECT_EQ(ledger[0].odt_number, 0);
    EXPECT_EQ(ledger[0].odt_entry, 0);
    EXPECT_EQ(ledger[0].address, 0x3000u);
    EXPECT_EQ(ledger[0].size, 2);
    EXPECT_FALSE(ledger[0].pid.has_value()) << "START 之前 PID 不可知，不猜";
    EXPECT_EQ(ledger[1].odt_entry, 1);
    EXPECT_EQ(ledger[1].address, 0x3020u);

    // Slave 侧指针三元组必须逐条对上
    const auto& written = rig.slave.Written();
    ASSERT_EQ(written.size(), 2u);
    EXPECT_EQ(written[0].entry, 0);
    EXPECT_EQ(written[1].entry, 1);
}

TEST(XcpDaqConfigure, StartFillsPidFromFirstPidAndIsSentOnWire) {
    Rig rig;
    rig.slave.SetFirstPid(0x24);
    rig.master->ConfigureDaqList(MakeSpec());
    const std::uint8_t first_pid = rig.master->StartDaqList(1);
    EXPECT_EQ(first_pid, 0x24U);
    const auto& ledger = rig.master->DaqLedger();
    ASSERT_EQ(ledger.size(), 2u);
    ASSERT_TRUE(ledger[0].pid.has_value());
    ASSERT_TRUE(ledger[1].pid.has_value());
    // 同一 ODT 的两条 Entry 共享一个 PID（Absolute ODT Number，docs L2225）
    EXPECT_EQ(*ledger[0].pid, 0x24U);
    EXPECT_EQ(*ledger[1].pid, 0x24U);
    // GET_STATUS 的 DAQ_RUNNING 位随 START 生效（docs L2220）
    const GetStatusResponse status = rig.master->QueryStatus();
    EXPECT_TRUE(status.daq_running);
}

TEST(XcpDaqConfigure, FailureRollsBackLedgerLeavingNoHalfAccount) {
    Rig rig;
    rig.slave.FailWith(CommandCode::WriteDaq, ErrorCode::WriteProtected);
    EXPECT_THROW(rig.master->ConfigureDaqList(MakeSpec()), XcpException);
    EXPECT_TRUE(rig.master->DaqLedger().empty())
        << "B-6：账本必须完整，失败即整批回滚（不留半份账）";
}

TEST(XcpDaqConfigure, RejectsInvalidSpecWithoutSendingCommands) {
    Rig rig;
    DaqListSpec empty;
    empty.daq_list = 0;
    EXPECT_THROW(rig.master->ConfigureDaqList(empty), XcpException);

    DaqListSpec zero_size = MakeSpec();
    zero_size.odts[0].entries[0].size = 0;
    EXPECT_THROW(rig.master->ConfigureDaqList(zero_size), XcpException);
    EXPECT_EQ(rig.slave.Count(CommandCode::ClearDaqList), 0)
        << "入参非法时一条命令都不发";
}

TEST(XcpDaqConfigure, DynamicProcessorPropertyFailsBeforeDaqWrite) {
    Rig rig;
    rig.slave.SetProcessorProperties(
        static_cast<std::uint8_t>(DaqProcessorPropertyBit::kConfigType));
    EXPECT_THROW(rig.master->ConfigureDaqList(MakeSpec()), XcpException);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetDaqProcessorInfo), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::ClearDaqList), 0);
    EXPECT_EQ(rig.slave.Count(CommandCode::WriteDaq), 0);
}

TEST(XcpDaqConfigure, PidOffFailsBeforeAnyCommand) {
    Rig rig;
    DaqListSpec spec = MakeSpec();
    spec.pid_off = true;
    EXPECT_THROW(rig.master->ConfigureDaqList(spec), XcpException);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetDaqProcessorInfo), 0);
    EXPECT_EQ(rig.slave.Count(CommandCode::ClearDaqList), 0);
    EXPECT_EQ(rig.slave.Count(CommandCode::WriteDaq), 0);
}

TEST(XcpDaqConfigure, MaxDtoPrecheckMultipliesAddressGranularity) {
    Rig rig;
    DaqListSpec spec = MakeSpec();
    spec.odts[0].entries = {{0x3000, 0x00, 8, kDaqBitOffsetNone}};
    EXPECT_THROW(rig.master->ConfigureDaqList(spec), XcpException);
    EXPECT_EQ(rig.slave.Count(CommandCode::GetDaqProcessorInfo), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::ClearDaqList), 0);
    EXPECT_EQ(rig.slave.Count(CommandCode::WriteDaq), 0);
}

TEST(XcpDaqConfigure, ProcessorInfoIsQueriedOncePerSession) {
    Rig rig;
    rig.slave.SetProcessorProperties(0U);
    rig.master->ConfigureDaqList(MakeSpec());
    rig.master->ConfigureDaqList(MakeSpec());
    EXPECT_EQ(rig.slave.Count(CommandCode::GetDaqProcessorInfo), 1);
}

TEST(XcpDaqConfigure, StartUnconfiguredListIsInvalidState) {
    Rig rig;
    try {
        (void)rig.master->StartDaqList(7);
        FAIL() << "未配置的列表不得启动";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
    EXPECT_EQ(rig.slave.Count(CommandCode::StartStopDaqList), 0);
}

TEST(XcpDaqConfigure, ClearRemovesLedgerAndBumpsGeneration) {
    Rig rig;
    rig.master->ConfigureDaqList(MakeSpec());
    const std::uint32_t after_config = rig.master->DaqConfigGeneration();
    ASSERT_EQ(rig.master->DaqLedger().size(), 2u);
    rig.master->ClearDaqList(1);
    EXPECT_TRUE(rig.master->DaqLedger().empty());
    EXPECT_GT(rig.master->DaqConfigGeneration(), after_config)
        << "代际必须前进，让陈旧快照在解码侧失配（B-6）";
}

TEST(XcpDaqConfigure, ModeBitsFollowDocFlagSet) {
    // MODE 位值交叉参考只读 thirdparty/XCPlite/src/xcp.h:331-336
    Rig rig;
    DaqListSpec spec = MakeSpec();
    spec.stim_direction = true;
    spec.dto_counter = true;
    spec.timestamp = true;
    rig.master->ConfigureDaqList(spec);
    const std::vector<Bytes> sent = rig.transport->SentPackets();
    bool checked = false;
    for (const auto& pkt : sent) {
        if (!pkt.empty() &&
            pkt[0] == static_cast<std::uint8_t>(CommandCode::SetDaqListMode)) {
            EXPECT_EQ(pkt[1], 0x02U | 0x08U | 0x10U)
                << "STIM | DTO_CTR | TIMESTAMP 的组合位";
            checked = true;
        }
    }
    EXPECT_TRUE(checked) << "未见 SET_DAQ_LIST_MODE 报文";
}

// ---------------------------------------------------------------------------
// 隐含状态在 SYNCH 恢复后的重放（docs L2783）
// ---------------------------------------------------------------------------

TEST(XcpDaqRecovery, WriteDaqTimeoutReissuesSetDaqPtr) {
    Rig rig;
    rig.slave.DropTimes(CommandCode::WriteDaq, 1);
    rig.master->ConfigureDaqList(MakeSpec());
    EXPECT_EQ(rig.slave.Count(CommandCode::Synch), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::SetDaqPtr), 3)
        << "恢复必须重放 SET_DAQ_PTR（docs L2783），否则重试落在不可知指针上";
    EXPECT_EQ(rig.slave.Count(CommandCode::WriteDaq), 3);
    EXPECT_EQ(rig.master->DaqLedger().size(), 2u) << "账本仍要完整";
    // 重放落在同一 Entry（同址同数据 → 幂等，不会错位）
    const auto& written = rig.slave.Written();
    ASSERT_EQ(written.size(), 2u);
    EXPECT_EQ(written[0].entry, 0);
    EXPECT_EQ(written[1].entry, 1);
}

TEST(XcpDaqRecovery, DownloadTimeoutReissuesSetMta) {
    Rig rig;
    rig.slave.FailWith(CommandCode::ShortDownload, ErrorCode::CmdUnknown);
    rig.slave.DropTimes(CommandCode::Download, 1);
    const Bytes data = {0xAA, 0xBB, 0xCC, 0xDD};
    rig.master->WriteMemoryBytes(0x4000, 0x00, BytesView{data});
    EXPECT_EQ(rig.slave.Count(CommandCode::Synch), 1);
    EXPECT_GE(rig.slave.Count(CommandCode::SetMta), 2)
        << "每块前都有 SET_MTA，恢复后再来一次（块首地址）";
    EXPECT_EQ(rig.slave.Count(CommandCode::Download), 2);
    const auto& mem = rig.slave.Memory();
    ASSERT_EQ(mem.size(), 4u);
    EXPECT_EQ(mem.at(0x4000), 0xAAU);
    EXPECT_EQ(mem.at(0x4003), 0xDDU);
}

// ---------------------------------------------------------------------------
// 写回路径选择与上限
// ---------------------------------------------------------------------------

TEST(XcpDaqWrite, ShortDownloadPreferredWhenItFits) {
    Rig rig;  // MAX_CTO=16，AG=1 → (16-8)/1 = 8 元素
    const Bytes data = {0x01, 0x02, 0x03, 0x04};
    rig.master->WriteMemoryBytes(0x5000, 0x00, BytesView{data});
    EXPECT_EQ(rig.slave.Count(CommandCode::ShortDownload), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::SetMta), 0);
    EXPECT_EQ(rig.slave.Count(CommandCode::Download), 0);
    EXPECT_EQ(rig.slave.Memory().at(0x5002), 0x03U);
}

TEST(XcpDaqWrite, FallsBackWhenShortDownloadUnsupported) {
    Rig rig;
    rig.slave.FailWith(CommandCode::ShortDownload, ErrorCode::CmdUnknown);
    const Bytes data = {0x01, 0x02, 0x03, 0x04};
    rig.master->WriteMemoryBytes(0x5100, 0x00, BytesView{data});
    EXPECT_EQ(rig.slave.Count(CommandCode::ShortDownload), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::SetMta), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::Download), 1);
    EXPECT_EQ(rig.slave.Memory().at(0x5103), 0x04U);
}

TEST(XcpDaqWrite, ChunkLimitUsesMaxCtoMinusTwo) {
    // MAX_CTO=8, AG=1 → 每块 (8-2)/1 = 6 元素；13 字节 → 6+6+1 三块
    Rig rig(8U);
    Bytes data;
    for (int i = 0; i < 13; ++i) {
        data.push_back(static_cast<std::uint8_t>(i));
    }
    rig.master->WriteMemoryBytes(0x7000, 0x00, BytesView{data});
    EXPECT_EQ(rig.slave.Count(CommandCode::Download), 3);
    EXPECT_EQ(rig.slave.Count(CommandCode::SetMta), 3)
        << "每块都显式 SET_MTA，不依赖 Slave 的 MTA 自增";
    EXPECT_EQ(rig.slave.Memory().size(), 13u);
    EXPECT_EQ(rig.slave.Memory().at(0x700C), 12);
}

TEST(XcpDaqWrite, WriteProtectedPropagatesAsProtocolError) {
    Rig rig;
    rig.slave.FailWith(CommandCode::ShortDownload, ErrorCode::WriteProtected);
    const Bytes data = {0x11, 0x22};
    try {
        rig.master->WriteMemoryBytes(0x6000, 0x00, BytesView{data});
        FAIL() << "写保护必须上抛";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
        ASSERT_TRUE(e.GetErrorCode().has_value());
        EXPECT_EQ(*e.GetErrorCode(), ErrorCode::WriteProtected);
    }
    EXPECT_TRUE(rig.master->DaqLedger().empty());
}

TEST(XcpDaqWrite, InvalidArgumentsRejectedBeforeWire) {
    Rig rig;  // AG=Byte
    EXPECT_THROW(rig.master->WriteMemoryBytes(0x100, 0x00, BytesView{}),
                 XcpException);
    EXPECT_EQ(rig.slave.Count(CommandCode::ShortDownload), 0);
    EXPECT_EQ(rig.slave.Count(CommandCode::Download), 0);
}

// ---------------------------------------------------------------------------
// 会话级清理（T14-07）
// ---------------------------------------------------------------------------

TEST(XcpDaqSession, DisconnectStopsRunningListsFirst) {
    Rig rig;
    rig.master->ConfigureDaqList(MakeSpec());
    (void)rig.master->StartDaqList(1);
    rig.master->Disconnect();
    // 断连前必须 START_STOP_SYNCH(stop all)，且早于 DISCONNECT
    EXPECT_EQ(rig.slave.Count(CommandCode::StartStopSynch), 1);
    EXPECT_EQ(rig.slave.Count(CommandCode::Disconnect), 1);
    const std::vector<Bytes> sent = rig.transport->SentPackets();
    int synch_index = -1;
    int disconnect_index = -1;
    for (std::size_t i = 0; i < sent.size(); ++i) {
        if (!sent[i].empty() &&
            sent[i][0] ==
                static_cast<std::uint8_t>(CommandCode::StartStopSynch)) {
            synch_index = static_cast<int>(i);
        }
        if (!sent[i].empty() &&
            sent[i][0] == static_cast<std::uint8_t>(CommandCode::Disconnect)) {
            disconnect_index = static_cast<int>(i);
        }
    }
    ASSERT_GE(synch_index, 0);
    ASSERT_GE(disconnect_index, 0);
    EXPECT_LT(synch_index, disconnect_index) << "先停表再断开（顺序即证据）";
    // STOP 报文用的是 stop all（mode 0x00）
    EXPECT_EQ(static_cast<std::uint8_t>(
                  sent[static_cast<std::size_t>(synch_index)][1]),
              static_cast<std::uint8_t>(0x00U));
}

TEST(XcpDaqSession, IdleDisconnectSendsNoSynch) {
    // 未启动任何 DAQ List 时断连：不得多发命令（既有 253 用例语义的护栏）
    Rig rig;
    rig.master->Disconnect();
    EXPECT_EQ(rig.slave.Count(CommandCode::StartStopSynch), 0);
    EXPECT_EQ(rig.slave.Count(CommandCode::Disconnect), 1);
}

/**
 * @brief 断连后会话级状态的既定口径（批次14，T14-07）。
 * @details 本用例刻意**不**测"同一 XcpMaster 上 Disconnect → Connect"：
 *          `CommandExecutor::m_transport_closed_` 只在析构(65) 与
 *          OnTransportClosed(185) 处置 true，全仓无复位点（grep 实测），
 *          因此重连会在 PerformAttempt 里立刻被判超时并进入
 *          "Connecting 不允许恢复" 的 InvalidState。该缺口属批次14 范围外
 *          的既有行为，已作为遗留项登记在批次14 记录，不擅自在本批改语义。
 */
TEST(XcpDaqSession, DisconnectKeepsLedgerButClearsRunningState) {
    Rig rig;
    rig.master->ConfigureDaqList(MakeSpec());
    (void)rig.master->StartDaqList(1);
    const std::uint32_t gen = rig.master->DaqConfigGeneration();
    rig.master->Disconnect();
    // 账本是"我下发过什么"的事实，断连不抹掉（重连后要重新 CLEAR 才能复用）
    EXPECT_EQ(rig.master->DaqLedger().size(), 2u);
    // 会话级清理把代际归零：旧快照携带 gen≠0 时与新会话必然不匹配，
    // 解码侧据此仍能识破"拿上一会话的布局解释本会话 DTO"（B-6）
    EXPECT_EQ(rig.master->DaqConfigGeneration(), 0u);
    EXPECT_NE(gen, 0u);
    // 运行态已随 Session 清理（无 List 在跑 → 再次断连不会重复发 STOP）
    rig.master->Disconnect();
    EXPECT_EQ(rig.slave.Count(CommandCode::StartStopSynch), 1)
        << "第二次断连不得再补发 START_STOP_SYNCH";
}

}  // namespace
}  // namespace calmcar::xcp
