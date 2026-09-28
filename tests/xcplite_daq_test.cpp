/**
 * @file xcplite_daq_test.cpp
 * @brief 与 XCPlite Slave 的 DAQ
 * 实时采集端到端集成测试（批次20，T20-02/T20-03）。
 *
 * 依据 code-plan/XCPlite_Slave_协议调试_后续路线_批次18-21_计划.md（批次20）与
 * code-plan/XCPlite_Slave_协议调试_实施记录_批次20_DAQ实时采集.md（D/M/G
 * 事实表）。
 *
 * 本文件锁定真实 Slave 的实然行为（证据全在 xcplite.c /
 * xcptl_cfg.h，只读核证）：
 *  - 动态 ALLOC 是唯一建表通路（D9）→ 走批次20 新增的
 * ConfigureDaqListsDynamic；
 *  - 单列表 START(mode=1) 被 TEST_CHECKS 拒（D12，xcplite.c:2668-2671）→
 *    启动必须走 StartDaqSync()（逐表 SELECT + START_STOP_SYNCH(start
 * selected)）；
 *  - DTO 信封（DAQ_KEY_BYTE=0xC0，D6/D13）：每 ODT 一帧，头 4 字节
 *    = [ODTrel(b0)][0xAA(b1)][DAQ16 LE(b2..3)]，无单字节 PID；
 *    每事件的**首个** ODT 帧再跟 4 字节时间戳（1ns tick，LE32，会回绕）；
 *  - 队列 wire 补白（D21）：queueAcquire 按 XCPTL_PACKET_ALIGNMENT(4)
 * 向上取整，且 填充计入 LEN（queue32m.c:291-296 TODO，客户可见尾部填充字节）→
 * 断言按声明长度前缀比较；
 *  - FREE_DAQ 无运行门（实然核证：xcplite.c:2562-2564 直接
 * XcpClearDaq()，隐式停流 +整表清空；CRC_DAQ_ACTIVE 只挡 ALLOC
 * 族/SET_DAQ_LIST_MODE/WRITE_DAQ :2568 等， FREE-first 序列使 ALLOC
 * 系运行门不可达 → 运行中整表重配置=成功）；
 *  - 表内存 OPTION_DAQ_MEM_SIZE=3072B（D4），超量 ALLOC → CRC_MEMORY_OVERFLOW；
 *  - 事件通道不硬编码（D18：OPTION_ENABLE_PERSISTENCE 可能令 id 跨运行继承），
 *    从 Slave 运行时生成的 A2L EVENT 段解析 testev 的通道号。
 *
 * 批次20 不动 DtoPacket→桥接解码（记录 §3 降级条款）：envelope 手工切分断言。
 * 每用例独立 Slave 进程（夹具 SetUp/TearDown），流污染不跨用例。
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/command_executor.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"

#include "xcplite_slave_fixture.hpp"
#include "xcp_test_slave/xcplite_test_types.hpp"

namespace calmcar::xcp {
namespace {

using test::kExpectBasicU32;
using test::kExpectBasicU8;
using test::ResolveA2lSymbol;

/// @brief 采集用例夹具（套件名对齐计划 DoD 的 ctest -R XcpliteDaq）
class XcpliteDaqTest : public test::XcpliteSlaveTest {};

/**
 * @brief DTO 收集器（G4：transport 工作线程直回，自持锁，核内零解析）
 * @details OnDto 在收包线程被调用（command_executor.cpp:170），这里只做
 *          加锁拷贝；断言在主线程对快照做，避免与泵循环竞争。
 */
class DtoCollector final : public IEventListener {
public:
    void OnEvent(const EventPacket&) override {}
    void OnService(const ServicePacket&) override {}
    void OnDto(const DtoPacket& dto) override {
        std::lock_guard<std::mutex> lock(m_mutex_);
        m_frames_.push_back(dto.data);
    }

    /// @brief 当前累计帧数
    [[nodiscard]] std::size_t Count() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_frames_.size();
    }

    /// @brief 快照（调用方持有副本，与后续到达解耦）
    [[nodiscard]] std::vector<Bytes> Snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_frames_;
    }

    /// @brief 等待累计帧数 ≥ at_least；超时返回 false
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

/// @brief 把标量按小端编码为字节（回环两侧同机，会话恒 LSB）
template <typename T>
Bytes LeBytes(const T& value) {
    Bytes out(sizeof(T));
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
}

/// @brief 字节段相等（std::span 无 operator==，C++20 下用 memcmp 自行比较）
bool BytesEq(BytesView a, BytesView b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size()) == 0;
}

/// @brief 前缀比较：payload 至少覆盖 want 且前 want.size() 字节相等
/// @details D21——queueAcquire 把帧补白到 4 倍数且填充计入 wire LEN，
///          Slave 侧净荷真实长度只能按声明值前缀核对，尾部字节不做断言。
bool BytesPrefixEq(BytesView payload, BytesView want) {
    return payload.size() >= want.size() &&
           std::memcmp(payload.data(), want.data(), want.size()) == 0;
}

/// @brief XCPlite DTO 信封切分结果（D13）
struct EnvelopeView {
    std::uint8_t odt_rel{0};     ///< b0：事件内 ODT 相对号（0=本事件首 ODT）
    std::uint16_t daq{0};        ///< b2..3：DAQ List 绝对号（WORD LE）
    std::uint32_t timestamp{0};  ///< 首 ODT 帧的 4B 时间戳（1ns tick）；其余 0
    bool has_timestamp{false};
    BytesView payload{};  ///< 信封后的净荷
};

/// @brief 切分 [ODTrel][0xAA][DAQ16]（+首 ODT 的 4B ts）；不合法返回 nullopt
std::optional<EnvelopeView> SplitEnvelope(const Bytes& frame) {
    if (frame.size() < 4U || frame[1] != 0xAAU) {
        return std::nullopt;
    }
    EnvelopeView v;
    v.odt_rel = frame[0];
    v.daq = static_cast<std::uint16_t>(
        frame[2] | (static_cast<std::uint16_t>(frame[3]) << 8U));
    std::size_t off = 4U;
    if (v.odt_rel == 0U) {
        if (frame.size() < 8U) {
            return std::nullopt;
        }
        v.timestamp = static_cast<std::uint32_t>(frame[4]) |
                      (static_cast<std::uint32_t>(frame[5]) << 8U) |
                      (static_cast<std::uint32_t>(frame[6]) << 16U) |
                      (static_cast<std::uint32_t>(frame[7]) << 24U);
        v.has_timestamp = true;
        off = 8U;
    }
    v.payload = BytesView(frame).subspan(off);
    return v;
}

/**
 * @brief 从 Slave 运行时 A2L 的 EVENT 段解析事件通道号（D18 不硬编码）
 * @details 匹配行内 `"testev" 0x%...` 形态（a2l_writer.c:321 的
 *          `/begin EVENT "long" "short" 0x%X DAQ ...`），取短名后的十六进制
 * id。
 */
std::optional<std::uint16_t> ResolveEventChannel(
    const std::filesystem::path& a2l_path, const std::string& short_name) {
    std::ifstream file(a2l_path);
    if (!file) {
        return std::nullopt;
    }
    const std::string needle = "\"" + short_name + "\" 0x";
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t pos = line.find(needle);
        if (pos == std::string::npos) {
            continue;
        }
        const std::size_t hex_begin = pos + needle.size();
        const std::size_t hex_end = line.find(' ', hex_begin);
        const std::string hex = line.substr(
            hex_begin, hex_end == std::string::npos ? std::string::npos
                                                    : hex_end - hex_begin);
        try {
            return static_cast<std::uint16_t>(std::stoul(hex, nullptr, 16));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

/// @brief 构造一个单 Entry ODT 规格
DaqOdtSpec MakeOdt(Address address, AddressExtension ext, std::uint8_t size) {
    DaqEntrySpec entry;
    entry.address = address;
    entry.extension = ext;
    entry.size = size;
    DaqOdtSpec odt;
    odt.entries.push_back(entry);
    return odt;
}

// ---------------------------------------------------------------------------
// T20-02：端到端正用例——建表→同步启动→DTO 值→实时刷新→停流→二次重配置
// ---------------------------------------------------------------------------

TEST_F(XcpliteDaqTest, DynamicAcqEndToEnd) {
    DtoCollector collector;
    auto master = MakeConnectedMaster(&collector);

    const auto u32_info = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_u32");
    const auto u8_info = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_u8");
    ASSERT_TRUE(u32_info.has_value()) << "A2L 未找到 g_basic_u32";
    ASSERT_TRUE(u8_info.has_value()) << "A2L 未找到 g_basic_u8";
    const auto event = ResolveEventChannel(slave_.A2lPath(), "testev");
    ASSERT_TRUE(event.has_value())
        << "A2L EVENT 段未找到 testev（D18：通道号必须来自核证而非硬编码）";

    // —— 第 0 步：启动前不得有任何 DTO（D3：isStarted 门未开零输出）——
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(collector.Count(), 0u) << "未 START 就收到 DTO——D3 门失效";

    // —— 1) 动态建表：List0 = ODT0(g_basic_u32, 4B) + ODT1(g_basic_u8, 1B) ——
    DaqListSpec spec;
    spec.daq_list = 0U;
    spec.event_channel = *event;
    spec.timestamp = true;  // D11 Slave 强制；预检头也按带 ts 计
    spec.odts.push_back(MakeOdt(u32_info->address, u32_info->extension, 4U));
    spec.odts.push_back(MakeOdt(u8_info->address, u8_info->extension, 1U));
    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({spec}));

    // 账本：2 条；pid 保持 nullopt（RELATIVE 信封无 Absolute 推导，D19/B-16）
    const auto& ledger = master->DaqLedger();
    ASSERT_EQ(ledger.size(), 2u);
    EXPECT_FALSE(ledger[0].pid.has_value());
    EXPECT_FALSE(ledger[1].pid.has_value());

    // —— 2) 同步启动：SELECT 全表 + SYNCH(start selected)（D12）——
    ASSERT_NO_THROW(master->StartDaqSync());
    ASSERT_TRUE(collector.WaitFor(8, std::chrono::seconds(2)))
        << "启动后 2s 内 DTO 不足 8 帧（slave 泵 1ms 周期，D3）";

    // —— 3) 信封切分 + 值断言 ——
    {
        const auto frames = collector.Snapshot();
        int odt0_hits = 0;
        int odt1_hits = 0;
        for (const auto& f : frames) {
            const auto v = SplitEnvelope(f);
            ASSERT_TRUE(v.has_value()) << "帧非 XCPlite 信封形态（D13）";
            EXPECT_EQ(v->daq, 0U) << "DAQ16 绝对号应为唯一列表 0";
            if (v->odt_rel == 0U) {
                // 首 ODT：4B 头 + 4B ts + 4B 净荷 =
                // 12（对齐无补白，仍按前缀核）
                ASSERT_GE(f.size(), 12U);
                ASSERT_LE(f.size(), 15U);
                EXPECT_TRUE(v->has_timestamp);
                if (BytesPrefixEq(v->payload,
                                  BytesView(LeBytes(kExpectBasicU32)))) {
                    ++odt0_hits;
                }
            } else {
                // 后续 ODT：无 ts（D13）；5B 帧上线补白到 8B（D21），净荷按 1B
                // 前缀核
                ASSERT_GE(f.size(), 5U);
                ASSERT_LE(f.size(), 8U);
                EXPECT_FALSE(v->has_timestamp);
                if (!v->payload.empty() && v->payload[0] == kExpectBasicU8) {
                    ++odt1_hits;
                }
            }
        }
        EXPECT_GT(odt0_hits, 0) << "未见 g_basic_u32==0xDEADBEEF 的 ODT0 帧";
        EXPECT_GT(odt1_hits, 0) << "未见 g_basic_u8==0x42 的 ODT1 帧";
    }

    // —— 4) 实时刷新：改 ECU 变量 → 后续帧必须带出新值 ——
    const std::uint32_t magic = 0xCAFEBABEU;
    ASSERT_NO_THROW(master->WriteMemoryBytes(
        u32_info->address, u32_info->extension, LeBytes(magic)));
    const bool refreshed = [&]() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            for (const auto& f : collector.Snapshot()) {
                const auto v = SplitEnvelope(f);
                if (v && v->odt_rel == 0U &&
                    BytesPrefixEq(v->payload, BytesView(LeBytes(magic)))) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }();
    EXPECT_TRUE(refreshed) << "写回后 DTO 未带出新值——采集非实时";

    // —— 5) 停流：SYNCH(stop all) 后等 flush（≤250ms，D12）→ 不再增帧 ——
    ASSERT_NO_THROW(master->StopDaq());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const std::size_t settled = collector.Count();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(collector.Count(), settled) << "StopDaq 后仍有新 DTO（停流失效）";

    // —— 6) 二次重配置：整表 FREE→ALLOC 重建为单 ODT 双 Entry ——
    DaqListSpec spec2;
    spec2.daq_list = 0U;
    spec2.event_channel = *event;
    spec2.timestamp = true;
    {
        DaqEntrySpec e1;
        e1.address = u32_info->address;
        e1.extension = u32_info->extension;
        e1.size = 4U;
        DaqEntrySpec e2;
        e2.address = u8_info->address;
        e2.extension = u8_info->extension;
        e2.size = 1U;
        DaqOdtSpec odt;
        odt.entries.push_back(e1);
        odt.entries.push_back(e2);
        spec2.odts.push_back(odt);
    }
    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({spec2}));
    EXPECT_EQ(master->DaqLedger().size(), 2u);
    ASSERT_NO_THROW(master->StartDaqSync());
    const std::size_t base = collector.Count();
    ASSERT_TRUE(collector.WaitFor(base + 4, std::chrono::seconds(2)))
        << "二次启动后无新 DTO";
    {
        const auto frames = collector.Snapshot();
        bool combo_hit = false;
        for (auto it = frames.begin() + static_cast<long>(base);
             it != frames.end(); ++it) {
            const auto v = SplitEnvelope(*it);
            ASSERT_TRUE(v.has_value());
            // 新布局只配了 1 个 ODT → 每帧都是 odt_rel 0；13B=4+4+4+1
            // 线上补白成 16B（D21），净荷按 5B 前缀核
            EXPECT_EQ(v->odt_rel, 0U);
            if (v->payload.size() >= 5U) {
                Bytes want = LeBytes(magic);
                want.push_back(kExpectBasicU8);
                if (BytesPrefixEq(v->payload, BytesView(want))) {
                    combo_hit = true;
                }
            }
        }
        EXPECT_TRUE(combo_hit) << "二次配置的 u32+u8 组合净荷未出现";
    }
    ASSERT_NO_THROW(master->StopDaq());
    master->Disconnect();
}

// ---------------------------------------------------------------------------
// T20-03：真实 Slave 行为锁定——运行中重建=隐式停流+重建（修
// D9）；超量建表失败回滚
// ---------------------------------------------------------------------------

TEST_F(XcpliteDaqTest, RunningReconfigureStopsStreamAndRebuilds) {
    DtoCollector collector;
    auto master = MakeConnectedMaster(&collector);

    const auto u32_info = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_u32");
    ASSERT_TRUE(u32_info.has_value());
    const auto event = ResolveEventChannel(slave_.A2lPath(), "testev");
    ASSERT_TRUE(event.has_value());

    DaqListSpec spec;
    spec.daq_list = 0U;
    spec.event_channel = *event;
    spec.timestamp = true;
    spec.odts.push_back(MakeOdt(u32_info->address, u32_info->extension, 4U));
    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({spec}));
    ASSERT_NO_THROW(master->StartDaqSync());
    ASSERT_TRUE(collector.WaitFor(4, std::chrono::seconds(2)));

    // —— 实然（修 D9）：FREE_DAQ 无运行门（xcplite.c:2562-2564 直接
    // XcpClearDaq()
    //    = 隐式停流 + 整表清空），master 的 FREE-first 序列使 ALLOC
    //    系运行门不可达 → 运行中整表重配置成功，且 Slave 已停流：不
    //    StartDaqSync 就不该有新帧 ——
    DaqListSpec spec2;
    spec2.daq_list = 0U;
    spec2.event_channel = *event;
    spec2.timestamp = true;
    spec2.priority = 1U;  // 换参数验证重建真的落地
    spec2.odts.push_back(MakeOdt(u32_info->address, u32_info->extension, 4U));
    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({spec2}));
    EXPECT_EQ(master->DaqLedger().size(), 1u) << "重建后账本应为新表";

    std::this_thread::sleep_for(
        std::chrono::milliseconds(400));  // 等在途 flush 落袋
    const std::size_t settled = collector.Count();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(collector.Count(), settled)
        << "FREE 的隐式停流必须成立（仍在增帧则 D9 修正被推翻）";

    // —— 重建表可直接再启动，且新帧仍带出正确值 ——
    ASSERT_NO_THROW(master->StartDaqSync());
    const std::size_t base = collector.Count();
    ASSERT_TRUE(collector.WaitFor(base + 2, std::chrono::seconds(2)))
        << "重配置后 StartDaqSync 无流";
    bool value_ok = false;
    const auto frames = collector.Snapshot();
    for (auto it = frames.begin() + static_cast<long>(base); it != frames.end();
         ++it) {
        const auto v = SplitEnvelope(*it);
        if (v && v->odt_rel == 0U &&
            BytesPrefixEq(v->payload, BytesView(LeBytes(kExpectBasicU32)))) {
            value_ok = true;
        }
    }
    EXPECT_TRUE(value_ok) << "重启后的帧未带出正确采集值";
    ASSERT_NO_THROW(master->StopDaq());
    master->Disconnect();
}
TEST_F(XcpliteDaqTest, MemoryOverflowFailsTransactionAndLeavesUsableTable) {
    auto master = MakeConnectedMaster();

    const auto u8_info = ResolveA2lSymbol(slave_.A2lPath(), "g_basic_u8");
    ASSERT_TRUE(u8_info.has_value());
    const auto event = ResolveEventChannel(slave_.A2lPath(), "testev");
    ASSERT_TRUE(event.has_value());

    // OPTION_DAQ_MEM_SIZE=3072B（D4）：250 ODT×1 entry ≈ 250*(8+6)+12 > 3072
    DaqListSpec huge;
    huge.daq_list = 0U;
    huge.event_channel = *event;
    huge.timestamp = true;
    for (int i = 0; i < 250; ++i) {
        huge.odts.push_back(MakeOdt(u8_info->address, u8_info->extension, 1U));
    }
    std::optional<ErrorCode> code;
    try {
        master->ConfigureDaqListsDynamic({huge});
        FAIL() << "超量建表必须失败（D4 内存核算）";
    } catch (const XcpException& e) {
        code = e.GetErrorCode();
        EXPECT_EQ(e.Category(), ErrorCategory::ProtocolError);
    }
    // Slave 的拒因以实然为准（内存溢出/越界/序列之一，B-16 不猜死单个码值）；
    // 本断言锁定"协议类失败"这一强性质，码值仅记录供 §4 回写。
    EXPECT_TRUE(code.has_value());
    EXPECT_TRUE(code == ErrorCode::MemoryOverflow ||
                code == ErrorCode::OutOfRange || code == ErrorCode::Sequence)
        << "实际错误码="
        << static_cast<int>(code.value_or(static_cast<ErrorCode>(0)));
    // master 的事务回滚：账本必须清空（半表不残留）
    EXPECT_TRUE(master->DaqLedger().empty()) << "失败事务后账本必须为空";

    // 失败后 Slave 仍可用：合法重建必须成功
    DaqListSpec ok;
    ok.daq_list = 0U;
    ok.event_channel = *event;
    ok.timestamp = true;
    ok.odts.push_back(MakeOdt(u8_info->address, u8_info->extension, 1U));
    ASSERT_NO_THROW(master->ConfigureDaqListsDynamic({ok}));
    EXPECT_EQ(master->DaqLedger().size(), 1u);
    master->Disconnect();
}

// ---------------------------------------------------------------------------
// 批次21 21-2：GET_ID(IDT_ASAM_UPLOAD) + UPLOAD 顺序分块拉取整份 A2L
// ---------------------------------------------------------------------------

TEST_F(XcpliteDaqTest, FetchA2lViaUploadMatchesDiskFile) {
    // 对端实然（xcplite.c:2169-2181 / xcpappl.c:546-596）：GET_ID(0x04) 以
    // fopen 打开 "<A2L名>.a2l"（相对 Slave 工作目录），上报 MODE=0 +
    // LENGTH；其后 UPLOAD 为 FILE MTA 纯顺序读（无 fseek）。拉回内容必须
    // 与盘上夹具记录的 A2lPath 文件逐字节一致。default 配置自带
    // OPTION_ENABLE_A2L_UPLOAD（xcplib_cfg.h:168），无需额外开关。
    const auto master = MakeConnectedMaster();
    const Bytes remote = master->FetchA2lViaUpload();

    std::ifstream in(slave_.A2lPath(), std::ios::binary);
    ASSERT_TRUE(in.is_open()) << slave_.A2lPath().string();
    const Bytes disk{std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>()};
    ASSERT_FALSE(disk.empty());
    ASSERT_EQ(remote.size(), disk.size())
        << "拉回 " << remote.size() << " 字节 vs 盘上 " << disk.size();
    EXPECT_TRUE(remote == disk)
        << "A2L 上传内容与盘文件首个差异位置="
        << [&remote, &disk] {
               std::size_t i = 0;
               while (i < remote.size() && i < disk.size() &&
                      remote[i] == disk[i]) {
                   ++i;
               }
               return i;
           }();
    master->Disconnect();
}

}  // namespace
}  // namespace calmcar::xcp
