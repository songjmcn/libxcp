// =============================================================================
// a2l_e2e_test.cpp —— 批次10 工作包 10.3：A2L → XCP 端到端验收
//
// 覆盖 R4 文档头部的首个验收里程碑（前半）：
//   1) IF_DATA XCP 自动建链：Slave 绑定 A2L 声明端口，Transport 端点
//      完全取自 XcpInfo()（host 经集成层解析，见 ToTransportAddress 注释）；
//   2) A2L 与 Slave 运行时一致性：GetSessionParameters → CompareWithRuntime
//      （B-16：0 个 Error；已知差异恰为 MAX_CTO/MAX_DTO 两条 Warning）；
//   3) 变量名 → 地址 → SHORT_UPLOAD 读值：符号表寻址（B-1）驱动真实读，
//      raw → ToPhysical（B-8/B-14），含 RL 推导后的 CHARACTERISTIC；
//   4) 错误路径：未预置内存的地址 → 结构化 XcpException（不静默）。
//
// 装配胶水只在本测试内（不新增生产公开 API，AGENTS.md 约束）。
// **写路径（DOWNLOAD）不在本批**：libxcp 核心尚无写内存接口（协议层仅有
// 命令码枚举），扩展属核心命令层，需用户明确指示，随 10.4（DAQ 命令组）
// 一并处理 —— 详见批次10记录 §5。
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/command_executor.hpp"  // CommandTimeouts
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "udp_test_slave.hpp"

#ifndef A2L_GOLDEN_DIR
#error "A2L_GOLDEN_DIR 缺失（tests/CMakeLists.txt 注入）"
#endif

namespace {

using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::AddressGranularity;
using calmcar::xcp::a2l::ByteOrder;
using calmcar::xcp::a2l::Bytes;
using calmcar::xcp::a2l::IA2lDatabase;
using calmcar::xcp::a2l::LoadOptions;
using calmcar::xcp::a2l::PhysicalValue;
using calmcar::xcp::a2l::RuntimeXcpParams;
using calmcar::xcp::a2l::Severity;
using calmcar::xcp::a2l::SymbolInfo;
namespace test = calmcar::xcp::test;

/// @brief golden_mask 的 IF_DATA 声明端口（fixture 的 Slave 必须绑定同端口）
constexpr std::uint16_t kA2lPort = 0x15B9;

/**
 * @brief A2L 主机名 → Transport 可用地址（集成胶水，见批次10记录 §5）
 * @details `udp_transport.cpp:253` 的 parseIpv4 只接受 IPv4 字面量，
 * 且远端过滤用字面量比较（:465）——名称解析属后续集成层职责，本批只做
 * 确定性的本地映射（localhost → 127.0.0.1），非回环名原样透传并由连接报错暴露。
 */
std::string ToTransportAddress(const std::string& host) {
    if (host.empty()) {
        return host;
    }
    std::string lower = host;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower == "localhost" ? "127.0.0.1" : host;
}

/**
 * @brief XCP 版本字节 → A2L PROTOCOL_LAYER 版本（uint16 风格）
 * @details XCP CONNECT RES 的 PROTOCOL_VERSION 高/低 nibble = major/minor
 * （0x10 = 1.0）；A2L 侧 0x0100 = major 1 / minor 0。
 */
std::uint16_t VersionByteToA2l(std::uint8_t version_byte) {
    const auto major = static_cast<std::uint16_t>((version_byte >> 4) & 0x0F);
    const auto minor = static_cast<std::uint16_t>(version_byte & 0x0F);
    return static_cast<std::uint16_t>((major << 8) | minor);
}

/// @brief golden 样本路径（build tree，fixture 生成）
std::string Golden(const char* name) {
    return std::string(A2L_GOLDEN_DIR) + "/" + name;
}

/**
 * @brief DTO 收集监听器（批次14，T14-12）
 * @details 回调在 Transport 工作线程执行，因此内部用 mutex 保护；测试侧用
 *          WaitForCount() 有界等待，不做 sleep 猜时间。
 */
class DtoListener : public calmcar::xcp::IEventListener {
public:
    void OnEvent(const calmcar::xcp::EventPacket& /*event*/) override {}
    void OnService(const calmcar::xcp::ServicePacket& /*service*/) override {}
    void OnDto(const calmcar::xcp::DtoPacket& dto) override {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        m_dtos_.push_back(dto);
        m_cv_.notify_all();
    }

    /// @brief 等待收到至少 n 帧 DTO；返回实际计数（超时即返回当前值）
    std::size_t WaitForCount(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex_);
        (void)m_cv_.wait_for(lock, timeout,
                             [this, n] { return m_dtos_.size() >= n; });
        return m_dtos_.size();
    }

    /// @brief 最后一帧 DTO（调用方须先确认 WaitForCount 已达标）
    [[nodiscard]] calmcar::xcp::DtoPacket LastDto() const {
        const std::lock_guard<std::mutex> lock(m_mutex_);
        return m_dtos_.empty() ? calmcar::xcp::DtoPacket{} : m_dtos_.back();
    }

private:
    mutable std::mutex m_mutex_;
    std::condition_variable m_cv_;
    std::vector<calmcar::xcp::DtoPacket> m_dtos_;
};

/**
 * @brief 端到端夹具：固定端口 Slave（预置内存）+ A2L 装载 + A2L 端点自动建链
 * @details 每个用例独立重建（Slave 端口 0x15B9 被占用时构造抛异常，用例
 * 直接失败并给出明确信息——端口冲突即环境问题，不做静默回退）。
 */
class A2lE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        // 1) Slave：绑定 A2L 声明端口 + 预置 ECU 模拟内存（地址来自符号表）
        slave_ = std::make_unique<test::UdpTestSlave>(kA2lPort);
        // E2E 夹具验证完整 DAQ 能力链，必须在 CONNECT 前开启模拟，
        // 这样 RESOURCE 声明才能与运行时能力一致。
        slave_->SetDaqSimulationEnabled(true);
        slave_->Start();
        const Bytes mask_raw{0x00, 0x0F};  // M_MASK：0x0F00 → 掩码提取 15
        slave_->SetMemory(0x3000, mask_raw);
        const Bytes byte_val{0x2A};  // M_UBYTE → IDENTICAL 42
        slave_->SetMemory(0x3020, byte_val);
        const Bytes arr8{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
        slave_->SetMemory(0x3048, arr8);  // M_ARR8（B-1 基址）
        const Bytes ch_val{0x34, 0x12};   // C_VALUE：UWORD 0x1234（RL 推导）
        slave_->SetMemory(0x3100, ch_val);

        // 2) A2L 装载（默认 require_if_data_xcp=true，golden_mask 含 IF_DATA）
        auto loaded = A2lBridge::Load(Golden("golden_mask.a2l"));
        ASSERT_TRUE(loaded.HasValue())
            << "Load 失败: "
            << calmcar::xcp::a2l::ToString(loaded.ErrorInfo().code) << " / "
            << loaded.ErrorInfo().message;
        bridge_ = std::move(loaded).Value();
        ASSERT_NE(bridge_->Database(), nullptr);
        ASSERT_NE(bridge_->XcpInfo(), nullptr);
        ASSERT_EQ(bridge_->XcpInfo()->transports.size(), 1u);
        ASSERT_EQ(bridge_->XcpInfo()->transports[0].kind,
                  calmcar::xcp::a2l::TransportEndpoint::Kind::UdpIp);
        ASSERT_EQ(bridge_->XcpInfo()->transports[0].remote_port, kA2lPort);

        // 3) 自动建链：Transport 端点全部取自 A2L（XcpInfo）快照
        calmcar::xcp::UdpTransportConfig cfg;
        cfg.remote_host =
            ToTransportAddress(bridge_->XcpInfo()->transports[0].remote_host);
        cfg.remote_port = bridge_->XcpInfo()->transports[0].remote_port;
        cfg.local_host = "127.0.0.1";
        cfg.local_port = 0;
        cfg.receive_poll_interval_ms = 20;
        master_ = std::make_unique<calmcar::xcp::XcpMaster>(
            std::make_unique<calmcar::xcp::UdpTransport>(cfg),
            calmcar::xcp::CommandTimeouts{}, &dto_listener_);
        master_->Connect();
    }

    void TearDown() override {
        // 顺序：master 先关（发 DISCONNECT），再销毁 bridge，最后停 Slave
        master_.reset();
        bridge_.reset();
        slave_.reset();
    }

    /// @brief DTO 监听器（必须先于 master_ 声明：析构顺序相反，
    ///        master_ 先销毁才不会用到已析构的监听器）
    DtoListener dto_listener_;
    std::unique_ptr<test::UdpTestSlave> slave_;
    std::unique_ptr<A2lBridge> bridge_;
    std::unique_ptr<calmcar::xcp::XcpMaster> master_;
};

// ---------------------------------------------------------------------------
// 1) IF_DATA XCP 自动建链 + 会话参数
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, AutoLinkFromA2lEndpointConnects) {
    EXPECT_TRUE(master_->IsConnected());
    EXPECT_TRUE(slave_->IsConnected());
    EXPECT_EQ(master_->GetSessionState(),
              calmcar::xcp::SessionState::Connected);

    const calmcar::xcp::SessionParameters params =
        master_->GetSessionParameters();
    // CONNECT 协商结果（B-16 运行时真值来源）
    EXPECT_EQ(params.connect.max_cto, 8);
    EXPECT_EQ(params.connect.max_dto, 8);
    EXPECT_EQ(params.connect.byte_order, calmcar::xcp::ByteOrder::Intel);
    EXPECT_EQ(params.connect.address_granularity,
              calmcar::xcp::AddressGranularity::Byte);
    EXPECT_EQ(params.connect.protocol_layer_version, 0x10);  // XCP 1.0 字节
    EXPECT_TRUE(calmcar::xcp::HasResource(params.connect.resource_mask,
                                          calmcar::xcp::Resource::Daq));
    // Connect 自动附带 GET_COMM_MODE_INFO 查询
    ASSERT_TRUE(params.comm_mode_info.has_value());
    EXPECT_TRUE(params.short_upload_available);
    // 至少处理 CONNECT + GET_COMM_MODE_INFO + GET_STATUS
    EXPECT_GE(slave_->CommandCount(), 3u);
}

// ---------------------------------------------------------------------------
// 2) A2L ↔ Slave 运行时一致性（B-16：运行时为真值，Error 阻断受影响功能）
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, RuntimeConsistencyMatchesA2l) {
    // 一致性用例验证开启 DAQ 后的运行时真值，不依赖 Slave 默认状态。
    slave_->SetDaqSimulationEnabled(true);
    const calmcar::xcp::SessionParameters params =
        master_->GetSessionParameters();
    const auto& c = params.connect;

    RuntimeXcpParams rt;
    rt.protocol_version = VersionByteToA2l(c.protocol_layer_version);
    rt.max_cto = c.max_cto;
    rt.max_dto = c.max_dto;
    rt.byte_order = (c.byte_order == calmcar::xcp::ByteOrder::Intel)
                        ? ByteOrder::MsbLast
                        : ByteOrder::MsbFirst;
    switch (c.address_granularity) {
        case calmcar::xcp::AddressGranularity::Word:
            rt.address_granularity = AddressGranularity::Word;
            break;
        case calmcar::xcp::AddressGranularity::DWord:
            rt.address_granularity = AddressGranularity::Dword;
            break;
        case calmcar::xcp::AddressGranularity::Byte:
        default:
            rt.address_granularity = AddressGranularity::Byte;
            break;
    }
    rt.has_daq =
        calmcar::xcp::HasResource(c.resource_mask, calmcar::xcp::Resource::Daq);

    auto diff = bridge_->CompareWithRuntime(rt);
    ASSERT_TRUE(diff.HasValue());
    // golden_mask: A2L 声明 MAX_CTO=0x10/MAX_DTO=0x20，Slave 实际 8/8 →
    // 恰好 2 条 Warning（B-16：容量取两端保守值），version/BO/AG/DAQ 全一致。
    EXPECT_EQ(diff.Value().size(), 2u);
    int errors = 0;
    for (const auto& d : diff.Value()) {
        if (d.severity == Severity::Error) {
            ++errors;
        }
        EXPECT_NE(d.parameter_name, std::string("ADDRESS_GRANULARITY"));
        EXPECT_NE(d.parameter_name, std::string("BYTE_ORDER"));
    }
    EXPECT_EQ(errors, 0) << "运行时协商参数一致时不得出现 Error 项（B-16）";
    bool has_cto = false;
    bool has_dto = false;
    for (const auto& d : diff.Value()) {
        EXPECT_EQ(d.severity, Severity::Warning) << d.parameter_name;
        has_cto = has_cto || d.parameter_name == "MAX_CTO";
        has_dto = has_dto || d.parameter_name == "MAX_DTO";
    }
    EXPECT_TRUE(has_cto && has_dto);

    // 反向：runtime major 不兼容 → 必须升为 Error（运行时为真值的意义）
    RuntimeXcpParams major_bad = rt;
    major_bad.protocol_version = 0x0200;
    auto bad = bridge_->CompareWithRuntime(major_bad);
    ASSERT_TRUE(bad.HasValue());
    int major_errors = 0;
    for (const auto& d : bad.Value()) {
        if (d.parameter_name == "PROTOCOL_VERSION_MAJOR") {
            EXPECT_EQ(d.severity, Severity::Error);
            ++major_errors;
        }
    }
    EXPECT_EQ(major_errors, 1);
}

// ---------------------------------------------------------------------------
// 3) 变量名 → 地址 → SHORT_UPLOAD 读值（真实 Socket 往返）
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, ReadScalarsThroughA2lAddress) {
    const IA2lDatabase* db = bridge_->Database();
    ASSERT_NE(db, nullptr);

    // 标量 UBYTE：地址/扩展直通 slave，raw → IDENTICAL 物理值
    auto ub = db->Find("MASK_ECU::M_UBYTE");
    ASSERT_TRUE(ub.HasValue());
    EXPECT_EQ(ub.Value().xcp_address, 0x3020u);
    const Bytes ub_raw = master_->ReadMemoryBytes(
        ub.Value().xcp_address, ub.Value().address_extension,
        ub.Value().element_size_bytes);
    ASSERT_EQ(ub_raw.size(), 1u);
    EXPECT_EQ(ub_raw[0], 0x2A);
    auto ub_phys = db->ToPhysical("MASK_ECU::M_UBYTE", ub_raw);
    ASSERT_TRUE(ub_phys.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(ub_phys.Value()));
    EXPECT_EQ(std::get<std::int64_t>(ub_phys.Value()), 42);

    // 掩码 UWORD：BIT_MASK 值提取（0x0F00 → 15）
    auto masked = db->Find("MASK_ECU::M_MASK");
    ASSERT_TRUE(masked.HasValue());
    EXPECT_EQ(masked.Value().bit_mask, 0x0F00u);
    const Bytes mask_raw = master_->ReadMemoryBytes(
        masked.Value().xcp_address, masked.Value().address_extension, 2);
    auto mask_phys = db->ToPhysical("MASK_ECU::M_MASK", mask_raw);
    ASSERT_TRUE(mask_phys.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(mask_phys.Value()));
    EXPECT_EQ(std::get<std::int64_t>(mask_phys.Value()), 15);

    // 数组 B-1：整段 raw 原样读回（8 字节），元素地址按 AG 推进
    auto arr = db->Find("MASK_ECU::M_ARR8");
    ASSERT_TRUE(arr.HasValue());
    auto byte_size = db->ByteSizeOf("MASK_ECU::M_ARR8");
    ASSERT_TRUE(byte_size.HasValue());
    EXPECT_EQ(byte_size.Value(), 8u);
    const Bytes arr_raw = master_->ReadMemoryBytes(
        arr.Value().xcp_address, arr.Value().address_extension, 8);
    const Bytes arr_expect{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    EXPECT_EQ(arr_raw, arr_expect);
    auto elem3 = calmcar::xcp::a2l::ComputeElementAddress(
        arr.Value(), 3, AddressGranularity::Byte);
    ASSERT_TRUE(elem3.HasValue());
    EXPECT_EQ(elem3.Value(), 0x3048u + 3);  // 0-based：第 4 个元素在 +3
    const Bytes elem3_raw = master_->ReadMemoryBytes(
        elem3.Value(), arr.Value().address_extension, 1);
    ASSERT_EQ(elem3_raw.size(), 1u);
    EXPECT_EQ(elem3_raw[0], 0x03);

    // CHARACTERISTIC：RECORD_LAYOUT 推导出元素类型后走同一读路径（B-4）
    auto ch = db->Find("MASK_ECU::C_VALUE");
    ASSERT_TRUE(ch.HasValue());
    EXPECT_EQ(ch.Value().data_type, calmcar::xcp::a2l::AsamDataType::UWord);
    EXPECT_EQ(ch.Value().element_size_bytes, 2);
    EXPECT_TRUE(ch.Value().read_write);
    EXPECT_EQ(ch.Value().address_extension, 0x12);
    const Bytes ch_raw = master_->ReadMemoryBytes(
        ch.Value().xcp_address, ch.Value().address_extension,
        ch.Value().element_size_bytes);
    ASSERT_EQ(ch_raw.size(), 2u);
    auto ch_phys = db->ToPhysical("MASK_ECU::C_VALUE", ch_raw);
    ASSERT_TRUE(ch_phys.HasValue());
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(ch_phys.Value()));
    EXPECT_EQ(std::get<std::int64_t>(ch_phys.Value()), 0x1234);
}

// ---------------------------------------------------------------------------
// 4) 未预置内存地址 → 结构化失败（不静默返回零填充数据）
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 5) 写回（T14-12）：变量名 → 地址 → WriteMemoryBytes → 读回换算闭环
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, WriteBackCalibrationVariable) {
    // Slave 的写回模拟默认关闭（既有 253 用例语义不变），用例需显式开启
    slave_->SetDaqSimulationEnabled(true);
    const IA2lDatabase* db = bridge_->Database();
    ASSERT_NE(db, nullptr);
    auto ch = db->Find("MASK_ECU::C_VALUE");
    ASSERT_TRUE(ch.HasValue());
    // 写前先读：确认目标地址已被 fixture 预置为 0x1234
    const Bytes before = master_->ReadMemoryBytes(
        ch.Value().xcp_address, ch.Value().address_extension, 2);
    ASSERT_EQ(before, (Bytes{0x34, 0x12}));

    // 反算物理值 → 写回 → 读回 → 正算，四步都必须成立
    auto raw_new = db->FromPhysical(
        "MASK_ECU::C_VALUE", PhysicalValue{static_cast<std::int64_t>(0x2211)});
    ASSERT_TRUE(raw_new.HasValue()) << raw_new.ErrorInfo().message;
    ASSERT_EQ(raw_new.Value().size(), 2u);
    master_->WriteMemoryBytes(ch.Value().xcp_address,
                              ch.Value().address_extension, raw_new.Value());

    const Bytes after = master_->ReadMemoryBytes(
        ch.Value().xcp_address, ch.Value().address_extension, 2);
    EXPECT_EQ(after, raw_new.Value());
    auto phys = db->ToPhysical("MASK_ECU::C_VALUE", after);
    ASSERT_TRUE(phys.HasValue());
    EXPECT_EQ(std::get<std::int64_t>(phys.Value()), 0x2211);
}

// ---------------------------------------------------------------------------
// 6) DAQ 实时链路（T14-12）：配置 → START → 收 DTO → 按**本端账本**解码
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, DaqLiveEndToEndUsesActualLedgerOrder) {
    slave_->SetDaqSimulationEnabled(true);

    // 故意与 A2L golden_mask 的顺序相反：A2L 是 [0x3000(2B), 0x3020(1B)]，
    // 这里下发 [0x3020(1B), 0x3000(2B)]。B-6 要求解码只认账本，不认 A2L 序。
    calmcar::xcp::DaqListSpec spec;
    spec.daq_list = 2;
    spec.event_channel = 1;
    spec.prescaler = 1;
    spec.priority = 0;
    calmcar::xcp::DaqOdtSpec odt;
    odt.entries = {
        {0x3020, 0x00, 1, calmcar::xcp::kDaqBitOffsetNone},
        {0x3000, 0x00, 2, calmcar::xcp::kDaqBitOffsetNone},
    };
    spec.odts = {odt};
    ASSERT_NO_THROW(master_->ConfigureDaqList(spec));
    const std::uint8_t first_pid = master_->StartDaqList(2);

    // 账本 → 桥接侧快照视图（AG=Byte，故 elements == bytes）
    const std::vector<calmcar::xcp::DaqLedgerEntry>& ledger =
        master_->DaqLedger();
    ASSERT_EQ(ledger.size(), 2u);
    std::vector<calmcar::xcp::a2l::DaqLedgerEntryView> views;
    for (const auto& e : ledger) {
        calmcar::xcp::a2l::DaqLedgerEntryView v;
        v.daq_list = e.daq_list;
        v.odt_number = e.odt_number;
        v.odt_entry = e.odt_entry;
        v.address = e.address;
        v.address_extension = e.extension;
        v.size_bytes = static_cast<std::uint8_t>(
            e.size *
            calmcar::xcp::AgToBytes(
                master_->GetSessionParameters().connect.address_granularity));
        // 批次15（F3）：位偏直接透传 XCP 原值（0xFF=无位偏，docs L1861），
        // 归一已由桥接层入口吸收 —— 这里故意不转换，以证明调用方无需知晓两套口径
        v.bit_offset = e.bit_offset;
        v.pid = e.pid;
        views.push_back(v);
    }
    ASSERT_TRUE(ledger[0].pid.has_value());
    EXPECT_EQ(*ledger[0].pid, first_pid);

    const std::uint32_t gen = master_->DaqConfigGeneration();
    auto layout_r = bridge_->CreateDaqLayoutFromLedger(views, gen);
    ASSERT_TRUE(layout_r.HasValue()) << layout_r.ErrorInfo().message;
    // 批次15（F4）：代际可查是陈旧快照守卫的前提（调用方据此一行判过期）
    ASSERT_GT(gen, 0u);
    EXPECT_EQ(layout_r.Value()->Generation(), gen);

    // Slave 按账本发一帧 DTO（PID = FIRST_PID，净荷 1B + 2B）
    ASSERT_EQ(slave_->SendDaqListDtos(2), 1u);
    ASSERT_EQ(dto_listener_.WaitForCount(1, std::chrono::seconds(5)), 1u);
    const calmcar::xcp::DtoPacket frame = dto_listener_.LastDto();
    ASSERT_EQ(frame.data.size(), 4u);
    EXPECT_EQ(frame.data[0], frame.pid)
        << "R12 帧边界：OnDto 交付的 data 必须含 PID";

    calmcar::xcp::a2l::DtoFrameLayout envelope;  // Absolute + 1B PID
    auto samples = layout_r.Value()->Decode(
        envelope,
        calmcar::xcp::a2l::BytesView{frame.data.data(), frame.data.size()});
    ASSERT_TRUE(samples.HasValue()) << samples.ErrorInfo().message;
    ASSERT_EQ(samples.Value().size(), 2u);
    // 顺序 = 账本顺序（0x3020 在前），不是 A2L 顺序
    EXPECT_EQ(samples.Value()[0].symbol_name, "MASK_ECU::M_UBYTE");
    EXPECT_EQ(samples.Value()[0].raw, (Bytes{0x2A}));
    EXPECT_EQ(samples.Value()[1].symbol_name, "MASK_ECU::M_MASK");
    EXPECT_EQ(samples.Value()[1].raw, (Bytes{0x00, 0x0F}));
    // F2：先判换算是否真的做过，再取值（PhysicalValue 默认就是 int64 0）
    EXPECT_TRUE(samples.Value()[0].physical_valid);
    auto phys0 = samples.Value()[0].physical_value;
    ASSERT_TRUE(std::holds_alternative<std::int64_t>(phys0));
    EXPECT_EQ(std::get<std::int64_t>(phys0), 42);
}

TEST_F(A2lE2ETest, MissingLedgerRejectsDecodeButKeepsRaw) {
    // B-6：没有账本就不解释 —— 空账本得到的快照必须整帧拒绝
    auto layout_r = bridge_->CreateDaqLayoutFromLedger({}, 0U);
    ASSERT_TRUE(layout_r.HasValue());  // 构造不失败：拒绝发生在 Decode
    const Bytes frame{0x10, 0x2A, 0x00, 0x0F};
    auto samples = layout_r.Value()->Decode(
        calmcar::xcp::a2l::DtoFrameLayout{},
        calmcar::xcp::a2l::BytesView{frame.data(), frame.size()});
    ASSERT_FALSE(samples.HasValue());
    EXPECT_EQ(samples.ErrorInfo().code,
              calmcar::xcp::a2l::ErrorCode::InvalidLayout);
    EXPECT_EQ(samples.ErrorInfo().phase, calmcar::xcp::a2l::Phase::Layout);
}

TEST_F(A2lE2ETest, UnknownPidIsRejectedNotGuessed) {
    slave_->SetDaqSimulationEnabled(true);
    calmcar::xcp::DaqListSpec spec;
    spec.daq_list = 1;
    calmcar::xcp::DaqOdtSpec odt;
    odt.entries = {{0x3020, 0x00, 1, calmcar::xcp::kDaqBitOffsetNone}};
    spec.odts = {odt};
    master_->ConfigureDaqList(spec);
    const std::uint8_t first_pid = master_->StartDaqList(1);

    std::vector<calmcar::xcp::a2l::DaqLedgerEntryView> views;
    for (const auto& e : master_->DaqLedger()) {
        calmcar::xcp::a2l::DaqLedgerEntryView v;
        v.daq_list = e.daq_list;
        v.odt_number = e.odt_number;
        v.odt_entry = e.odt_entry;
        v.address = e.address;
        v.address_extension = e.extension;
        v.size_bytes = e.size;
        v.bit_offset = e.bit_offset;
        v.pid = e.pid;
        views.push_back(v);
    }
    auto layout_r = bridge_->CreateDaqLayoutFromLedger(
        views, master_->DaqConfigGeneration());
    ASSERT_TRUE(layout_r.HasValue());

    // 负例：把真实 PID 挪一位 —— 净荷只有 1 字节，若解码器误把净荷首字节
    // 当 PID（R12 风险），这里就会"看起来能解"。必须显式拒绝。
    const Bytes wrong{static_cast<std::uint8_t>(first_pid + 1U), 0x2A};
    auto bad = layout_r.Value()->Decode(
        calmcar::xcp::a2l::DtoFrameLayout{},
        calmcar::xcp::a2l::BytesView{wrong.data(), wrong.size()});
    ASSERT_FALSE(bad.HasValue());
    EXPECT_EQ(bad.ErrorInfo().code, calmcar::xcp::a2l::ErrorCode::NotFound);
    // 正例：同一帧用正确 PID 就能解出 1 条 entry
    const Bytes right{first_pid, 0x2A};
    auto ok = layout_r.Value()->Decode(
        calmcar::xcp::a2l::DtoFrameLayout{},
        calmcar::xcp::a2l::BytesView{right.data(), right.size()});
    ASSERT_TRUE(ok.HasValue()) << ok.ErrorInfo().message;
    ASSERT_EQ(ok.Value().size(), 1u);
    EXPECT_EQ(ok.Value()[0].symbol_name, "MASK_ECU::M_UBYTE");
}

// ---------------------------------------------------------------------------
// 6b) ECU 回读通路（T14-10）：READ_DAQ 取证 → EcuReadback 快照 → 按 PID 解码
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, EcuReadbackSnapshotDecodesPredefinedDto) {
    slave_->SetDaqSimulationEnabled(true);
    calmcar::xcp::DaqListSpec spec;
    spec.daq_list = 1;
    calmcar::xcp::DaqOdtSpec odt;
    odt.entries = {{0x3020, 0x00, 1, calmcar::xcp::kDaqBitOffsetNone}};
    spec.odts = {odt};
    master_->ConfigureDaqList(spec);
    const std::uint8_t first_pid = master_->StartDaqList(1);

    // 取证：READ_DAQ 回读该 Entry（B-6：PREDEFINED 布局只能这样证）
    const auto readback = master_->ReadDaqEntryAt(1, 0, 0);
    ASSERT_TRUE(readback.has_value());
    EXPECT_EQ(readback->address, 0x3020u);
    EXPECT_EQ(readback->bit_offset, calmcar::xcp::kDaqBitOffsetNone);

    const std::uint8_t ag = calmcar::xcp::AgToBytes(
        master_->GetSessionParameters().connect.address_granularity);
    calmcar::xcp::a2l::DaqReadbackEntry e;
    e.daq_list = 1;
    e.odt_number = 0;
    e.odt_entry = 0;
    e.address = readback->address;
    e.address_extension = readback->address_extension;
    e.size_bytes = static_cast<std::uint8_t>(readback->size * ag);
    // 位偏口径：XCP 的 0xFF（无位偏）→ 桥接层的 0
    // F3：直接透传 READ_DAQ 原值（0xFF），桥接层负责归一
    e.bit_offset = readback->bit_offset;
    const std::vector<calmcar::xcp::a2l::DaqReadbackEntry> readbacks = {e};
    const std::vector<calmcar::xcp::a2l::DaqListPid> pids = {{1, first_pid}};

    auto layout_r = bridge_->CreateDaqLayoutFromEcuReadback(
        readbacks, pids, master_->DaqConfigGeneration());
    ASSERT_TRUE(layout_r.HasValue()) << layout_r.ErrorInfo().message;

    ASSERT_EQ(slave_->SendDaqListDtos(1), 1u);
    ASSERT_EQ(dto_listener_.WaitForCount(1, std::chrono::seconds(5)), 1u);
    const calmcar::xcp::DtoPacket frame = dto_listener_.LastDto();
    auto samples = layout_r.Value()->Decode(
        calmcar::xcp::a2l::DtoFrameLayout{},
        calmcar::xcp::a2l::BytesView{frame.data.data(), frame.data.size()});
    ASSERT_TRUE(samples.HasValue()) << samples.ErrorInfo().message;
    ASSERT_EQ(samples.Value().size(), 1u);
    EXPECT_EQ(samples.Value()[0].symbol_name, "MASK_ECU::M_UBYTE");
    EXPECT_EQ(samples.Value()[0].raw, (Bytes{0x2A}));
    EXPECT_TRUE(samples.Value()[0].physical_valid) << "F2：换算成功才置位";
    EXPECT_EQ(std::get<std::int64_t>(samples.Value()[0].physical_value), 42);
}

// ---------------------------------------------------------------------------
// 6c) B-16 的 DAQ 侧比对（T14-11）：运行时真值来自 ECU，不是 A2L 自称
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, DaqRuntimeComparisonUsesEcuTruth) {
    slave_->SetDaqSimulationEnabled(true);
    const auto processor = master_->QueryDaqProcessorInfo();
    const auto resolution = master_->QueryDaqResolutionInfo();
    ASSERT_TRUE(processor.has_value());
    ASSERT_TRUE(resolution.has_value());
    // 脚本 Slave：ADDRESS_EXTENSION=DAQ(3)、IDENTIFICATION=ABSOLUTE(0)、粒度 1
    EXPECT_EQ(processor->key_byte.address_extension_mode, 3U);
    EXPECT_EQ(processor->key_byte.identification_field_type, 0U);

    const calmcar::xcp::SessionParameters params =
        master_->GetSessionParameters();
    RuntimeXcpParams rt;
    rt.protocol_version =
        VersionByteToA2l(params.connect.protocol_layer_version);
    rt.max_cto = params.connect.max_cto;
    rt.max_dto = params.connect.max_dto;
    rt.byte_order = ByteOrder::MsbLast;
    rt.address_granularity = AddressGranularity::Byte;
    rt.has_daq = true;
    rt.daq_identification_field_type =
        processor->key_byte.identification_field_type;
    rt.daq_address_extension_mode = processor->key_byte.address_extension_mode;
    rt.daq_odt_entry_min_size_bytes = resolution->granularity_daq;
    auto diff = bridge_->CompareWithRuntime(rt);
    ASSERT_TRUE(diff.HasValue());
    for (const auto& d : diff.Value()) {
        // 三项 DAQ 比对全部一致 → 不得出现；剩下的只有容量类 Warning
        EXPECT_NE(d.parameter_name, "IDENTIFICATION_FIELD_TYPE");
        EXPECT_NE(d.parameter_name, "ADDRESS_EXTENSION_MODE");
        EXPECT_NE(d.parameter_name, "GRANULARITY_ODT_ENTRY_SIZE_DAQ");
        EXPECT_EQ(d.severity, Severity::Warning) << d.parameter_name;
    }

    // 反向证据：篡改识别字段必须立刻产生 Error（证明比对不是摆设）
    RuntimeXcpParams tampered = rt;
    tampered.daq_identification_field_type = 2;  // RelativeWord
    auto bad = bridge_->CompareWithRuntime(tampered);
    ASSERT_TRUE(bad.HasValue());
    bool found_error = false;
    for (const auto& d : bad.Value()) {
        if (d.parameter_name == "IDENTIFICATION_FIELD_TYPE") {
            found_error = true;
            EXPECT_EQ(d.severity, Severity::Error);
        }
    }
    EXPECT_TRUE(found_error) << "识别字段不一致必须是 Error（B-16）";
}

// ---------------------------------------------------------------------------
// 7) 错误路径
// ---------------------------------------------------------------------------

TEST_F(A2lE2ETest, ReadUnconfiguredAddressThrows) {
    EXPECT_THROW((void)master_->ReadMemoryBytes(0x9F9F, 0x00, 4),
                 calmcar::xcp::XcpException);
}

}  // namespace
