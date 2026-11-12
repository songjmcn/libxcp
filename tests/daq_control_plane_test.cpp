/**
 * @file daq_control_plane_test.cpp
 * @brief 批次 B2：DAQ 控制面（WRITE_DAQ_MULTIPLE 与 DTO_CTR_PROPERTIES）验收。
 *
 * 锁四件事：
 *   1) WRITE_DAQ_MULTIPLE 的线格式：`[C7][n]` + 每条目固定 8 字节（末条也有
 *      dummy），以及 `MAX_CTO` 决定的每帧容量；
 *   2) 批量写入必须**只在本 ODT 内切包**，且账本与逐条写入完全一致；
 *   3) DTO_CTR_PROPERTIES 的请求 MODIFIER/MODE 与响应 PROPERTIES 位语义
 *      （注意 `*Fixed` 是"1 = 不可改"，与 `*Present` 相反）；
 *   4) 只有 `ERR_CMD_UNKNOWN` 降级为 `nullopt`，其余协议错误必须上抛。
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "libxcp/command_codec.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/response_parser.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

using test::MockTransport;

/// @brief 把初始化列表转成报文容器（便于断言线格式）
Bytes BytesOf(std::initializer_list<std::uint8_t> values) {
    return Bytes(values.begin(), values.end());
}

/// @brief 构造正响应报文（自动加 0xFF 前缀）
Bytes Res(const std::vector<std::uint8_t>& body) {
    Bytes out;
    out.reserve(1 + body.size());
    out.push_back(static_cast<std::uint8_t>(PacketType::Res));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// @brief 构造只携带错误码的负响应报文
Bytes Err(ErrorCode code) {
    return Bytes{static_cast<std::uint8_t>(PacketType::Err),
                 static_cast<std::uint8_t>(code)};
}

/**
 * @brief 只实现 B2 相关命令的脚本化 Slave
 * @details 其余命令（含 GET_DAQ_PROCESSOR_INFO 等 Optional 取证命令）一律回
 *          `ERR_CMD_UNKNOWN`，用于验证"可选命令缺失"时的降级口径；同时记录收到
 *          的每一帧，供断言线格式与调用次数。
 * @note COMM_MODE_BASIC 取 0x00：Intel 字节序、AG=BYTE，且**不**声明
 *       SLAVE_BLOCK_MODE / OPTIONAL —— 让 CONNECT 阶段不再触发额外查询，
 *       控制面断言才能只看 B2 自己的命令。
 */
class ControlPlaneSlave {
public:
    /// @brief 构造
    /// @param max_cto 对端宣告的 MAX_CTO（决定批量写每帧容量）
    explicit ControlPlaneSlave(std::uint8_t max_cto = 26U)
        : m_max_cto_(max_cto) {}

    /// @brief 让指定命令持续回某个错误码
    void FailWith(CommandCode cmd, ErrorCode code) { m_errors_[cmd] = code; }

    /// @brief 清除全部错误注入（模拟故障恢复）
    void ClearFailures() { m_errors_.clear(); }

    /// @brief 设定 DTO_CTR_PROPERTIES 响应的 PROPERTIES/RELATED/MODE
    void SetDtoCtr(std::uint8_t properties, std::uint16_t related,
                   std::uint8_t mode) {
        m_ctr_properties_ = properties;
        m_ctr_related_ = related;
        m_ctr_mode_ = mode;
    }

    /// @brief 收到的帧（按到达顺序）
    [[nodiscard]] const std::vector<Bytes>& Received() const {
        return m_received_;
    }

    /// @brief 某个命令被收到的次数
    [[nodiscard]] int Count(CommandCode cmd) const {
        int n = 0;
        for (const auto& packet : m_received_) {
            if (!packet.empty() && static_cast<CommandCode>(packet[0]) == cmd) {
                ++n;
            }
        }
        return n;
    }

    /// @brief 第 index 次收到的指定命令报文（不存在时返回空）
    [[nodiscard]] Bytes FrameOf(CommandCode cmd, std::size_t index) const {
        std::size_t seen = 0;
        for (const auto& packet : m_received_) {
            if (!packet.empty() && static_cast<CommandCode>(packet[0]) == cmd) {
                if (seen == index) {
                    return packet;
                }
                ++seen;
            }
        }
        return {};
    }

    /// @brief MockTransport 的响应脚本入口
    Bytes operator()(BytesView packet) {
        if (packet.empty()) {
            return {};
        }
        m_received_.emplace_back(packet.begin(), packet.end());
        const auto cmd = static_cast<CommandCode>(packet[0]);
        const auto err_it = m_errors_.find(cmd);
        if (err_it != m_errors_.end()) {
            return Err(err_it->second);
        }
        switch (cmd) {
            case CommandCode::Connect:
                // [resource][comm_mode_basic=0x00][MAX_CTO][MAX_DTO WORD]
                // [PL version][TL version]
                return Res({0x15, 0x00, m_max_cto_, 0x08, 0x00, 0x10, 0x10});
            case CommandCode::Disconnect:
            case CommandCode::SetMta:
            case CommandCode::ClearDaqList:
            case CommandCode::SetDaqPtr:
            case CommandCode::WriteDaq:
            case CommandCode::WriteDaqMultiple:
            case CommandCode::SetDaqListMode:
                return Res({});
            case CommandCode::StartStopDaqList:
                // 正响应必须带 FIRST_PID（RES 至少 1 字节），否则解析器判短帧
                return Res({0x00});
            case CommandCode::GetStatus:
                return Res({0x00, 0x00, 0x01, 0x07, 0x00});
            case CommandCode::DtoCtrProperties:
                return Res({m_ctr_properties_,
                            static_cast<std::uint8_t>(m_ctr_related_ & 0xFFU),
                            static_cast<std::uint8_t>((m_ctr_related_ >> 8U) &
                                                      0xFFU),
                            m_ctr_mode_});
            default:
                // Optional 命令一律"未实现"，验证降级口径
                return Err(ErrorCode::CmdUnknown);
        }
    }

private:
    std::uint8_t m_max_cto_{26U};                ///< 宣告的 MAX_CTO
    std::vector<Bytes> m_received_;              ///< 收到的原始帧
    std::map<CommandCode, ErrorCode> m_errors_;  ///< 注入的错误码
    std::uint8_t m_ctr_properties_{0x10};        ///< 默认 DAQ_MODE_PRESENT
    std::uint16_t m_ctr_related_{0};             ///< 相关事件通道号
    std::uint8_t m_ctr_mode_{0};                 ///< MODE 原始位
};

/**
 * @brief Codec / Parser 层断言用的最小上下文
 * @details 线格式断言不依赖任何会话状态，直接构造两个组件即可。
 */
struct CodecFixture {
    explicit CodecFixture(ByteOrder order = ByteOrder::Intel,
                          AddressGranularity ag = AddressGranularity::Byte)
        : codec(order, ag), parser(order, ag) {}

    CommandCodec codec;     ///< 被测编码器
    ResponseParser parser;  ///< 被测解析器
};

// ======================= WRITE_DAQ_MULTIPLE 线格式 =======================

TEST(DaqControlPlane, WriteDaqMultipleFrameLayout) {
    CodecFixture f;
    const std::vector<WriteDaqElement> elements{
        {kDaqBitOffsetNone, 2U, 0x11223344U, 0x00U},
        {0x05U, 1U, 0x55667788U, 0x01U},
    };
    const Bytes cto = f.codec.EncodeWriteDaqMultiple(elements);

    // [C7][n=2] + 2 × 8 字节 = 18 字节
    ASSERT_EQ(cto.size(), 18U);
    EXPECT_EQ(cto[0], 0xC7U);
    EXPECT_EQ(cto[1], 0x02U);
    // 第 1 条目：bit_offset / size / addr(LE) / extension / dummy
    EXPECT_EQ(cto[2], kDaqBitOffsetNone);
    EXPECT_EQ(cto[3], 0x02U);
    EXPECT_EQ(cto[4], 0x44U);
    EXPECT_EQ(cto[5], 0x33U);
    EXPECT_EQ(cto[6], 0x22U);
    EXPECT_EQ(cto[7], 0x11U);
    EXPECT_EQ(cto[8], 0x00U);
    EXPECT_EQ(cto[9], 0x00U);
    // 第 2 条目
    EXPECT_EQ(cto[10], 0x05U);
    EXPECT_EQ(cto[11], 0x01U);
    EXPECT_EQ(cto[12], 0x88U);
    EXPECT_EQ(cto[13], 0x77U);
    EXPECT_EQ(cto[14], 0x66U);
    EXPECT_EQ(cto[15], 0x55U);
    EXPECT_EQ(cto[16], 0x01U);
    // 规范明确最后一个条目也要 Dummy；少一个字节整帧就短 1 字节
    EXPECT_EQ(cto[17], 0x00U);
}

TEST(DaqControlPlane, WriteDaqMultipleMotorolaOrder) {
    CodecFixture f(ByteOrder::Motorola);
    const std::vector<WriteDaqElement> elements{
        {kDaqBitOffsetNone, 1U, 0x12345678U, 0x00U},
    };
    const Bytes cto = f.codec.EncodeWriteDaqMultiple(elements);
    ASSERT_EQ(cto.size(), 10U);
    // 地址按大端落盘
    EXPECT_EQ(cto[4], 0x12U);
    EXPECT_EQ(cto[5], 0x34U);
    EXPECT_EQ(cto[6], 0x56U);
    EXPECT_EQ(cto[7], 0x78U);
    EXPECT_EQ(cto[9], 0x00U);  // Dummy 仍在末尾
}

TEST(DaqControlPlane, WriteDaqMultipleRejectsEmptyAndZeroSize) {
    CodecFixture f;
    EXPECT_THROW(f.codec.EncodeWriteDaqMultiple({}), XcpException);
    const std::vector<WriteDaqElement> zero_size{
        {kDaqBitOffsetNone, 0U, 0x1000U, 0x00U},
    };
    EXPECT_THROW(f.codec.EncodeWriteDaqMultiple(zero_size), XcpException);
}

// ======================= DTO_CTR_PROPERTIES 线格式 =======================

TEST(DaqControlPlane, DtoCtrPropertiesQueryFrame) {
    CodecFixture f;
    // 纯查询：MODIFIER=0，未使用的参数一律写 0
    const Bytes cto = f.codec.EncodeDtoCtrProperties(
        DtoCtrModifierBit::kNone, 0x0007U, 0U, DaqCounterMode::InsertCounter,
        false);
    EXPECT_EQ(cto, BytesOf({0xC5, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00}));
}

TEST(DaqControlPlane, DtoCtrPropertiesModifyDaqModeFrame) {
    CodecFixture f;
    // 只置 MODIFY_DAQ_MODE(bit1)，DAQ_MODE=1 → MODE 字节 bit0 置位
    const Bytes cto = f.codec.EncodeDtoCtrProperties(
        DtoCtrModifierBit::kModifyDaqMode, 0x0003U, 0U,
        DaqCounterMode::InsertStimCounterCopy, false);
    EXPECT_EQ(cto, BytesOf({0xC5, 0x02, 0x03, 0x00, 0x00, 0x00, 0x01}));
}

TEST(DaqControlPlane, DtoCtrPropertiesRejectsInconsistentArguments) {
    CodecFixture f;
    // 未置 MODIFY_DAQ_MODE 却传了非默认 DAQ_MODE
    EXPECT_THROW(f.codec.EncodeDtoCtrProperties(
                     DtoCtrModifierBit::kNone, 0x0001U, 0U,
                     DaqCounterMode::InsertStimCounterCopy, false),
                 XcpException);
    // 未置 MODIFY_RELATED_EVENT 却传了非零相关事件号
    EXPECT_THROW(f.codec.EncodeDtoCtrProperties(
                     DtoCtrModifierBit::kModifyDaqMode, 0x0001U, 0x0005U,
                     DaqCounterMode::InsertCounter, false),
                 XcpException);
    // 未置 MODIFY_STIM_MODE 却要求校验计数器
    EXPECT_THROW(f.codec.EncodeDtoCtrProperties(
                     DtoCtrModifierBit::kModifyDaqMode, 0x0001U, 0U,
                     DaqCounterMode::InsertCounter, true),
                 XcpException);
}

TEST(DaqControlPlane, DtoCtrPropertiesResponseParse) {
    CodecFixture f;
    // PROPERTIES=0x18：RELATED_EVENT_PRESENT(0x08) | DAQ_MODE_PRESENT(0x10)
    const Bytes res = BytesOf({0x18, 0x05, 0x00, 0x01});
    const auto parsed = f.parser.ParseDtoCtrProperties(res);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->properties, 0x18U);
    EXPECT_EQ(parsed->related_event_channel, 0x0005U);
    EXPECT_EQ(parsed->mode, 0x01U);
}

TEST(DaqControlPlane, DtoCtrPropertiesResponseMotorola) {
    CodecFixture f(ByteOrder::Motorola);
    const Bytes res = BytesOf({0x18, 0x00, 0x05, 0x01});
    const auto parsed = f.parser.ParseDtoCtrProperties(res);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->related_event_channel, 0x0005U);
}

TEST(DaqControlPlane, DtoCtrPropertiesShortResponseIsNullopt) {
    CodecFixture f;
    // 规范 Table 164 的 RES 剥掉 PID 后是 4 字节（PROPERTIES + WORD + MODE）；
    // 少到 3 字节时不能臆造 MODE 值
    const Bytes short_res = BytesOf({0x18, 0x05, 0x00});
    EXPECT_FALSE(f.parser.ParseDtoCtrProperties(short_res).has_value());
    // 恰好 4 字节是完整的：MODE 允许为 0（两个方向都不插计数器）
    const Bytes exact = BytesOf({0x18, 0x05, 0x00, 0x00});
    EXPECT_TRUE(f.parser.ParseDtoCtrProperties(exact).has_value());
}

// ======================= 位语义易混点 =======================

TEST(DaqControlPlane, FixedAndPresentBitsAreOppositeSemantics) {
    // DAQ_MODE_FIXED=0x02 表示"不可改"，DAQ_MODE_PRESENT=0x10 表示"支持"。
    // 两者语义相反，显式断言防止后续改动把它们混为一谈。
    EXPECT_EQ(static_cast<std::uint8_t>(DtoCtrPropertiesBit::kDaqModeFixed),
              0x02U);
    EXPECT_EQ(static_cast<std::uint8_t>(DtoCtrPropertiesBit::kDaqModePresent),
              0x10U);
    const auto props = static_cast<DtoCtrPropertiesBit>(0x12U);  // FIXED|PRESENT
    EXPECT_TRUE(HasDtoCtrProperty(props, DtoCtrPropertiesBit::kDaqModeFixed));
    EXPECT_TRUE(HasDtoCtrProperty(props, DtoCtrPropertiesBit::kDaqModePresent));
    // 请求侧同一位（0x02）是 MODIFY_DAQ_MODE，与响应侧 FIXED 恰好同值但方向
    // 相反：禁止把响应 PROPERTIES 当请求 MODIFIER 用
    EXPECT_EQ(static_cast<std::uint8_t>(DtoCtrModifierBit::kModifyDaqMode),
              0x02U);
}

// ======================= Master 编排：批量写路径 =======================

/**
 * @brief B2 编排测试的公共装置
 * @details 建一个含若干 Entry 的单 ODT List，便于观察批量/逐条两条路径。
 */
struct BlockHarness {
    /// @brief 构造并完成 CONNECT
    /// @param max_cto 由 Slave 宣告的 MAX_CTO
    explicit BlockHarness(std::uint8_t max_cto = 26U)
        : transport(std::make_unique<MockTransport>()), slave(max_cto) {
        transport->SetResponse([this](BytesView packet) { return slave(packet); });
        master = std::make_unique<XcpMaster>(std::move(transport));
        master->Connect();
    }

    /// @brief 构造一个含 n 个 Entry 的单 ODT List
    [[nodiscard]] DaqListSpec MakeSpec(std::uint16_t list,
                                       std::size_t n) const {
        DaqListSpec spec;
        spec.daq_list = list;
        spec.event_channel = 1U;
        DaqOdtSpec odt;
        for (std::size_t i = 0; i < n; ++i) {
            DaqEntrySpec entry;
            entry.address = static_cast<Address>(0x2000U + i);
            entry.extension = 0x00U;
            entry.size = 1U;
            odt.entries.push_back(entry);
        }
        spec.odts.push_back(odt);
        return spec;
    }

    std::unique_ptr<MockTransport> transport;  ///< 传输（交给 Master 持有）
    ControlPlaneSlave slave;                   ///< 脚本化 Slave
    std::unique_ptr<XcpMaster> master;         ///< 被测对象
};

TEST(DaqControlPlane, DefaultPathUsesSingleWriteDaqPerEntry) {
    BlockHarness h;
    h.master->ConfigureDaqList(h.MakeSpec(0U, 3U));

    // 默认关闭：3 个 Entry → 3 次 WRITE_DAQ、0 次 WRITE_DAQ_MULTIPLE
    EXPECT_EQ(h.slave.Count(CommandCode::WriteDaq), 3);
    EXPECT_EQ(h.slave.Count(CommandCode::WriteDaqMultiple), 0);
    EXPECT_EQ(h.slave.Count(CommandCode::SetDaqPtr), 3);
    ASSERT_EQ(h.master->DaqLedger().size(), 3U);
}

TEST(DaqControlPlane, EnabledPathPacksWithinOdtAndKeepsLedgerIdentical) {
    BlockHarness h;  // MAX_CTO=26 → (26-2)/8 = 3 条目/帧
    h.master->SetWriteDaqMultipleEnabled(true);
    h.master->ConfigureDaqList(h.MakeSpec(0U, 3U));

    // 3 个 Entry 恰好一帧，无需切包
    EXPECT_EQ(h.slave.Count(CommandCode::WriteDaqMultiple), 1);
    EXPECT_EQ(h.slave.Count(CommandCode::WriteDaq), 0);
    EXPECT_EQ(h.slave.Count(CommandCode::SetDaqPtr), 1);

    const Bytes frame = h.slave.FrameOf(CommandCode::WriteDaqMultiple, 0U);
    ASSERT_EQ(frame.size(), 26U);  // [C7][3] + 3 × 8
    EXPECT_EQ(frame[1], 0x03U);

    // 账本与逐条路径必须完全一致：解码布局不能被发送形态影响
    const auto& ledger = h.master->DaqLedger();
    ASSERT_EQ(ledger.size(), 3U);
    for (std::size_t i = 0; i < 3U; ++i) {
        EXPECT_EQ(ledger[i].daq_list, 0U);
        EXPECT_EQ(ledger[i].odt_number, 0U);
        EXPECT_EQ(ledger[i].odt_entry, i);
        EXPECT_EQ(ledger[i].address, 0x2000U + i);
        EXPECT_EQ(ledger[i].size, 1U);
    }
}

TEST(DaqControlPlane, EnabledPathSplitsAcrossFramesWithinSameOdt) {
    BlockHarness h;  // 每帧 3 条目
    h.master->SetWriteDaqMultipleEnabled(true);
    h.master->ConfigureDaqList(h.MakeSpec(0U, 5U));

    // 5 个 Entry → 2 帧（3 + 2），每帧起点各自 SET_DAQ_PTR
    EXPECT_EQ(h.slave.Count(CommandCode::WriteDaqMultiple), 2);
    EXPECT_EQ(h.slave.Count(CommandCode::SetDaqPtr), 2);

    const Bytes first = h.slave.FrameOf(CommandCode::WriteDaqMultiple, 0U);
    const Bytes second = h.slave.FrameOf(CommandCode::WriteDaqMultiple, 1U);
    ASSERT_EQ(first.size(), 26U);
    EXPECT_EQ(first[1], 0x03U);
    ASSERT_EQ(second.size(), 18U);  // [C7][2] + 2 × 8
    EXPECT_EQ(second[1], 0x02U);

    // 第二帧的 SET_DAQ_PTR 必须指向 entry=3
    const Bytes ptr = h.slave.FrameOf(CommandCode::SetDaqPtr, 1U);
    ASSERT_EQ(ptr.size(), 6U);
    EXPECT_EQ(ptr[4], 0x00U);  // ODT 号
    EXPECT_EQ(ptr[5], 0x03U);  // Entry 号
    EXPECT_EQ(h.master->DaqLedger().size(), 5U);
}

TEST(DaqControlPlane, EnabledPathRefusesWhenMaxCtoTooSmall) {
    // 规范要求 MAX_CTO >= 10 才能用 WRITE_DAQ_MULTIPLE；MAX_CTO=8 时
    // (8-2)/8 = 0，连一个条目都装不下，必须显式拒绝而不是静默降级
    BlockHarness h(8U);
    h.master->SetWriteDaqMultipleEnabled(true);
    EXPECT_THROW(h.master->ConfigureDaqList(h.MakeSpec(0U, 2U)), XcpException);
    EXPECT_EQ(h.slave.Count(CommandCode::WriteDaqMultiple), 0);
}

// ======================= DTO_CTR_PROPERTIES Master 层 =======================

TEST(DaqControlPlane, QueryDtoCounterPropertiesDecodesBits) {
    BlockHarness h;
    // PROPERTIES = 0x18 (RELATED_PRESENT | DAQ_MODE_PRESENT)，MODE=1
    h.slave.SetDtoCtr(0x18U, 0x0004U, 0x01U);
    const auto props = h.master->QueryDtoCounterProperties(0x0003U);
    ASSERT_TRUE(props.has_value());
    EXPECT_EQ(props->event_channel, 0x0003U);  // 由入参回填
    EXPECT_EQ(props->properties, 0x18U);
    EXPECT_EQ(props->related_event_channel, 0x0004U);
    EXPECT_TRUE(props->has_related_event);
    EXPECT_TRUE(props->supports_dto_counter);
    EXPECT_FALSE(props->daq_mode_fixed);
    EXPECT_EQ(props->daq_mode, DaqCounterMode::InsertStimCounterCopy);
}

TEST(DaqControlPlane, QueryUsesPureQueryModifier) {
    BlockHarness h;
    h.slave.SetDtoCtr(0x10U, 0U, 0x00U);
    (void)h.master->QueryDtoCounterProperties(0x0002U);

    const Bytes frame = h.slave.FrameOf(CommandCode::DtoCtrProperties, 0U);
    ASSERT_EQ(frame.size(), 7U);
    EXPECT_EQ(frame[1], 0x00U);  // MODIFIER == kNone → 纯查询，不改任何属性
    EXPECT_EQ(frame[2], 0x02U);  // 事件通道号低字节
    EXPECT_EQ(frame[3], 0x00U);
}

TEST(DaqControlPlane, QueryReturnsNulloptOnlyWhenCommandUnknown) {
    BlockHarness h;
    h.slave.FailWith(CommandCode::DtoCtrProperties, ErrorCode::CmdUnknown);
    EXPECT_FALSE(h.master->QueryDtoCounterProperties(0x0001U).has_value());

    // 其余协议错误必须上抛：把"通道不存在"吞成 nullopt 会被误读成
    // "查到了但没有属性"
    BlockHarness h2;
    h2.slave.FailWith(CommandCode::DtoCtrProperties, ErrorCode::OutOfRange);
    EXPECT_THROW(h2.master->QueryDtoCounterProperties(0x00FFU), XcpException);
}

TEST(DaqControlPlane, ConfigureRejectsWhenCounterUnsupported) {
    BlockHarness h;
    // DAQ_MODE_PRESENT(0x10) 未置位 → 该通道不支持 DTO 计数器
    h.slave.SetDtoCtr(0x00U, 0U, 0x00U);
    try {
        (void)h.master->ConfigureDaqCounterEvent(
            0x0001U, DaqCounterMode::InsertStimCounterCopy);
        FAIL() << "预期抛 UnsupportedFeature";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
    // 前置判定失败后不得下发修改帧
    EXPECT_EQ(h.slave.Count(CommandCode::DtoCtrProperties), 1);
}

TEST(DaqControlPlane, ConfigureRejectsWhenDaqModeFixed) {
    BlockHarness h;
    // DAQ_MODE_FIXED(0x02) + DAQ_MODE_PRESENT(0x10)
    h.slave.SetDtoCtr(0x12U, 0U, 0x00U);
    try {
        (void)h.master->ConfigureDaqCounterEvent(
            0x0001U, DaqCounterMode::InsertStimCounterCopy);
        FAIL() << "预期抛 UnsupportedFeature";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::UnsupportedFeature);
    }
    EXPECT_EQ(h.slave.Count(CommandCode::DtoCtrProperties), 1);
}

TEST(DaqControlPlane, ConfigureSendsOnlyModifyDaqModeBit) {
    BlockHarness h;
    // 支持且未固定：查询与修改都返回同一份属性
    h.slave.SetDtoCtr(0x10U, 0U, 0x00U);
    (void)h.master->ConfigureDaqCounterEvent(
        0x0006U, DaqCounterMode::InsertStimCounterCopy);

    // 第 0 次是前置查询，第 1 次是修改
    ASSERT_EQ(h.slave.Count(CommandCode::DtoCtrProperties), 2);
    const Bytes modify = h.slave.FrameOf(CommandCode::DtoCtrProperties, 1U);
    ASSERT_EQ(modify.size(), 7U);
    // 只有 MODIFY_DAQ_MODE(bit1)，不碰 RELATED_EVENT / STIM_MODE
    EXPECT_EQ(modify[1], 0x02U);
    EXPECT_EQ(modify[2], 0x06U);
    EXPECT_EQ(modify[3], 0x00U);
    EXPECT_EQ(modify[6], 0x01U);  // DAQ_MODE=1
}

// ======================= 失败状态策略（B2：回滚账本 != 回滚 Slave） =======================

TEST(DaqControlPlane, StaticConfigureFailureMarksListStateUnknown) {
    BlockHarness h;
    // 让 WRITE_DAQ 在第 2 个 Entry 上失败——CLEAR_DAQ_LIST 已生效、第 1 个
    // Entry 可能已写进 Slave，此时对端处于半成品状态
    h.slave.FailWith(CommandCode::WriteDaq, ErrorCode::WriteProtected);

    EXPECT_THROW(h.master->ConfigureDaqList(h.MakeSpec(0U, 3U)), XcpException);

    // 本端账本已回滚（不拿半份布局解码），但这不代表 Slave 也回滚了
    EXPECT_TRUE(h.master->DaqLedger().empty());
    EXPECT_TRUE(h.master->IsDaqListStateUnknown(0U));
    // 未涉及的列表不受影响
    EXPECT_FALSE(h.master->IsDaqListStateUnknown(1U));
}

TEST(DaqControlPlane, StartRejectsListWithUnknownSlaveState) {
    BlockHarness h;
    h.slave.FailWith(CommandCode::WriteDaq, ErrorCode::WriteProtected);
    EXPECT_THROW(h.master->ConfigureDaqList(h.MakeSpec(0U, 2U)), XcpException);
    const int start_frames_before = h.slave.Count(CommandCode::StartStopDaqList);

    try {
        (void)h.master->StartDaqList(0U);
        FAIL() << "预期拒绝启动状态未知的 List";
    } catch (const XcpException& e) {
        EXPECT_EQ(e.Category(), ErrorCategory::InvalidState);
    }
    // 拒绝必须发生在下发之前：Slave 不能收到 START
    EXPECT_EQ(h.slave.Count(CommandCode::StartStopDaqList),
              start_frames_before);
}

TEST(DaqControlPlane, ReconfigureClearsUnknownSlaveState) {
    BlockHarness h;
    h.slave.FailWith(CommandCode::WriteDaq, ErrorCode::WriteProtected);
    EXPECT_THROW(h.master->ConfigureDaqList(h.MakeSpec(0U, 2U)), XcpException);
    ASSERT_TRUE(h.master->IsDaqListStateUnknown(0U));

    // 故障恢复后整表重配：成功路径本身以 CLEAR_DAQ_LIST 开头，能把该 List
    // 重新推到已知状态，因此登记必须被撤销
    h.slave.ClearFailures();
    h.master->ConfigureDaqList(h.MakeSpec(0U, 2U));
    EXPECT_FALSE(h.master->IsDaqListStateUnknown(0U));
    EXPECT_EQ(h.master->DaqLedger().size(), 2U);
    // 配置已回到已知状态，启动不再被本地拦截
    h.master->StartDaqList(0U);
}

TEST(DaqControlPlane, ClearDaqListClearsUnknownSlaveState) {
    BlockHarness h;
    h.slave.FailWith(CommandCode::WriteDaq, ErrorCode::WriteProtected);
    EXPECT_THROW(h.master->ConfigureDaqList(h.MakeSpec(0U, 2U)), XcpException);
    ASSERT_TRUE(h.master->IsDaqListStateUnknown(0U));

    // 显式 CLEAR_DAQ_LIST 被确认：Slave 侧的该 List 回到已知的空状态
    h.master->ClearDaqList(0U);
    EXPECT_FALSE(h.master->IsDaqListStateUnknown(0U));
}

}  // namespace
}  // namespace calmcar::xcp