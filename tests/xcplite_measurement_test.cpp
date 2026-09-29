/**
 * @file xcplite_measurement_test.cpp
 * @brief 测量子系统 L3 端到端：真实 XCPlite Slave 上的全链路采集
 *        （FetchA2lViaUpload → A2lBridge::Load → A2lMeasurementDatabase →
 *        MeasurementSession → 物理值帧）。
 *
 * 依据 code-plan/libxcp_测量子系统代码增长计划.md v0.9（:732-759）。
 * 仅在 LIBXCP_BUILD_XCPLITE_SLAVE + LIBXCP_BUILD_A2L 同时开启时编译。
 *
 * 链路取证（全程无测试专用手工拆包，v0.9 验收项）：
 *  1) 经 GET_ID(IDT_ASAM_UPLOAD)+UPLOAD 从 Slave 拉回运行时 A2L（协议面，
 *     非盘文件捷径），落盘后交桥接层解析；
 *  2) A2lMeasurementDatabase 把桥接层 IA2lDatabase 包装为核心
 *     IMeasurementDatabase（adapter/，唯一的桥接接触点）；
 *  3) 适配器 Find 的 event_channel 恒 0（A2L 静态面无事件绑定，见
 *     a2l_measurement_database.cpp :101-103）——运行时事件通道由
 *     测试本地装饰器注入（XCPlite 的 testev，从 Slave 运行时 A2L 的
 *     EVENT 段解析，D18 不硬编码），对应计划"上层 QueryDaqEventInfo
 *     取证后另行补充"的上层职责；
 *  4) MeasurementSession 按 XCPlite 实然取证：RelativeByte 识别字段
 *     （[ODTrel][0xAA][DAQ16]，头长 4，D13）+ 仅首 ODT 帧带 4 字节
 *     时间戳（SetTimestampFirstOdtOnly）+ 时间戳单位 1ns/tick
 *     （Slave OPTION 实然；SetTimestampUnit 显式注入，R13 不猜）。
 */

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "a2l/a2l_measurement_database.hpp"
#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/daq/dto_envelope_types.hpp"
#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/measurement/measurement_sample.hpp"
#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_master.hpp"

#include "xcplite_slave_fixture.hpp"
#include "xcp_test_slave/xcplite_test_types.hpp"

namespace calmcar::xcp {
namespace {

namespace a2l = calmcar::xcp::a2l;

/// @brief 全链路测量用例夹具（套件名对齐 ctest -R XcpliteMeasurement）
class XcpliteMeasurementTest : public test::XcpliteSlaveTest {};

/// @brief 限定符号名（XCPlite 的 MODULE 名即项目名，同 xcplite_a2l_read_test）
std::string Qualified(const std::string& symbol) {
    return std::string(test::kXcpliteSlaveProject) + "::" + symbol;
}

/**
 * @brief 从 Slave 运行时 A2L 的 EVENT 段解析事件通道号（D18 不硬编码）
 * @details 与 xcplite_daq_test.cpp 同法：匹配 `"testev" 0x%...` 形态取
 *          十六进制 id。此处独立副本（测试 TU 不共享匿名命名空间符号）。
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

/**
 * @brief 事件通道装饰器：Find 结果注入运行时取证到的事件通道号
 * @details A2L 静态面不含"该变量由哪个事件触发"的绑定（适配器恒回 0）；
 *          上层从 Slave 运行时 A2L EVENT 段取证 testev 通道后在此补充，
 *          使 MeasurementPlanner 把两个变量归入同一事件组（一条 DAQ
 *          List）。核心/适配器均零改动。
 */
class EventBoundDatabase final : public IMeasurementDatabase {
 public:
  EventBoundDatabase(IMeasurementDatabase& inner, std::uint16_t event_channel)
      : m_inner_(inner), m_channel_(event_channel) {}

  EventBoundDatabase(const EventBoundDatabase&) = delete;
  EventBoundDatabase& operator=(const EventBoundDatabase&) = delete;

  MeasurementResult<MeasurementSymbolInfo> Find(
      std::string_view name) const override {
    MeasurementResult<MeasurementSymbolInfo> res = m_inner_.Find(name);
    if (!res.HasValue()) {
        return res;
    }
    MeasurementSymbolInfo info = res.Value();
    if (info.event_channel == 0U) {
        info.event_channel = m_channel_;
    }
    return detail::MakeMeasurementOk(info);
  }

  MeasurementResult<MeasurementValue> ToPhysical(
      std::string_view name, BytesView raw) const override {
    return m_inner_.ToPhysical(name, raw);
  }

 private:
  IMeasurementDatabase& m_inner_;      ///< 被包装的 A2L 适配器
  std::uint16_t m_channel_{0};         ///< 取证到的事件通道（testev）
};

/// @brief 线程安全帧收集器（worker 线程回调写入）
class FrameSink {
 public:
  void operator()(const MeasurementFrame& frame) {
    std::lock_guard<std::mutex> lock(m_mutex_);
    m_frames_.push_back(frame);
  }

  std::vector<MeasurementFrame> Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex_);
    return m_frames_;
  }

  /// @brief 轮询直到 pred 满足或超时
  template <class Pred>
  [[nodiscard]] bool WaitUntil(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred(Snapshot())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred(Snapshot());
  }

 private:
  mutable std::mutex m_mutex_;
  std::vector<MeasurementFrame> m_frames_;
};

/**
 * @brief RAII 守卫：测试体内任何提前返回（ASSERT 失败）都先 Stop 会话
 * @details 局部对象逆序析构：session 先于 master 构造，若不经 Stop 直接
 *          展开，master 先亡、session 析构里 Stop→StopDaq 触碰悬垂对象
 *          （0xc0000005）；且 worker 线程会在 sink 析构后继续回调。
 *          声明位置必须在 FrameSink 之后（先于 sink 析构）。
 */
class AutoStopSession {
 public:
    explicit AutoStopSession(MeasurementSession& session)
        : m_session_(session) {}

    AutoStopSession(const AutoStopSession&) = delete;
    AutoStopSession& operator=(const AutoStopSession&) = delete;

    ~AutoStopSession() { m_session_.Stop(); }

 private:
    MeasurementSession& m_session_;
};

/// @brief 超时诊断：会话统计 + 前若干帧的路由/样本概要（一次定位断点）
std::string DescribeFailure(
    const MeasurementSession& session,
    const std::vector<MeasurementFrame>& frames) {
    const MeasurementStatistics st = session.Statistics();
    std::string text = "stats[dto_received=" + std::to_string(st.dto_received) +
                       " dropped=" + std::to_string(st.dto_dropped) +
                       " decode_errors=" + std::to_string(st.decode_errors) +
                       "] frames=" + std::to_string(frames.size()) + ":";
    const std::size_t show = (frames.size() < 6U) ? frames.size() : 6U;
    for (std::size_t i = 0; i < show; ++i) {
        const MeasurementFrame& f = frames[i];
        text += " [l" + std::to_string(f.daq_list) + "/o" +
                std::to_string(f.odt) + " ts" +
                (f.timestamp_valid ? "v" : "-") + " n=" +
                std::to_string(f.samples.size()) + " ";
        for (const auto& s : f.samples) {
            text += s.name + (s.valid ? "=ok" : "=bad") + " ";
        }
        text += "]";
    }
    return text;
}

/// @brief 物理值是否为期望整数（int64/uint64 两分支都接受，桥接层按位宽取型）
bool PhysicalIs(const MeasurementSample& sample, std::uint64_t want) {
    if (!sample.valid) {
        return false;
    }
    if (const auto* p = std::get_if<std::int64_t>(&sample.value)) {
        return *p >= 0 && static_cast<std::uint64_t>(*p) == want;
    }
    if (const auto* p = std::get_if<std::uint64_t>(&sample.value)) {
        return *p == want;
    }
    if (const auto* p = std::get_if<double>(&sample.value)) {
        return *p == static_cast<double>(want);
    }
    return false;
}

// ---------------------------------------------------------------------------
// 主用例：协议拉取 A2L → 桥接 → 适配器 → 会话 → 物理值帧（全链路）
// ---------------------------------------------------------------------------

TEST_F(XcpliteMeasurementTest, FullChainSessionProducesPhysicalFrames) {
    // —— 1) UPLOAD 通路拉回运行时 A2L（协议面，不读盘捷径）——
    const auto fetch_master = MakeConnectedMaster();
    const Bytes uploaded = fetch_master->FetchA2lViaUpload();
    ASSERT_GT(uploaded.size(), 100U) << "UPLOAD 拉回的 A2L 过短";
    fetch_master->Disconnect();  // 释放会话，稍后以监听器形态重建

    const std::filesystem::path uploaded_a2l =
        slave_.WorkDir() / "uploaded_measurement.a2l";
    {
        std::ofstream out(uploaded_a2l, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(uploaded.data()),
                  static_cast<std::streamsize>(uploaded.size()));
    }

    // —— 2) 桥接层解析上传副本 ——
    auto loaded = a2l::A2lBridge::Load(uploaded_a2l.string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const a2l::IA2lDatabase* db = loaded.Value()->Database();
    ASSERT_NE(db, nullptr);

    // —— 3) 适配器 + 事件通道装饰（testev 从运行时 A2L EVENT 段取证）——
    const auto event = ResolveEventChannel(slave_.A2lPath(), "testev");
    ASSERT_TRUE(event.has_value()) << "A2L EVENT 段未找到 testev（D18）";
    A2lMeasurementDatabase adapter(*db);
    EventBoundDatabase measurement_db(adapter, *event);

    // FrameSink 先于 session/master 声明：析构顺序为逆序（sink 最后销毁），
    // 断言提前 return 时 session 析构先 join worker，回调绝不触碰悬 sink。
    FrameSink sink;

    // —— 4) 会话：先构造（作监听器）→ Bind → Connect → 取证注入 ——
    MeasurementSession session(measurement_db);
    auto master = MakeMaster(&session);
    session.Bind(*master);
    master->Connect();
    // 守卫：任何提前展开先 Stop（join worker + StopDaq），此时 master 尚存
    AutoStopSession guard(session);

    // XCPlite 实然取证：RelativeByte [ODTrel][0xAA][DAQ16]，头长 4（D13）；
    // 时间戳只随事件首 ODT 帧；单位 1ns/tick（SetTimestampUnit 显式注入）。
    session.SetEnvelopeMode(IdentificationFieldType::RelativeByte, 4U);
    session.SetTimestampFirstOdtOnly(true);
    session.SetTimestampUnit(1);
    const std::string u32_name = Qualified("g_basic_u32");
    const std::string u8_name = Qualified("g_basic_u8");
    session.Add(u32_name);
    session.Add(u8_name);
    ASSERT_NO_THROW(session.Prepare());

    ASSERT_NO_THROW(session.Start(
        MeasurementCallback([&sink](const MeasurementFrame& f) { sink(f); })));
    // 失败诊断转储：帧数、每帧 (daq,odt,ts_valid) 与各样本 name/valid/raw
    const auto dump = [&]() {
        std::ostringstream os;
        const auto fs = sink.Snapshot();
        const auto st = session.Statistics();
        os << " frames=" << fs.size() << " recv=" << st.dto_received
           << " dropped=" << st.dto_dropped << " decode_err="
           << st.decode_errors;
        for (std::size_t i = 0; i < fs.size() && i < 4U; ++i) {
            os << " | f" << i << " (list=" << fs[i].daq_list
               << ",odt=" << static_cast<int>(fs[i].odt)
               << ",tsv=" << fs[i].timestamp_valid << "):";
            for (const auto& s : fs[i].samples) {
                os << " " << s.name << "(v=" << s.valid << ",raw=";
                for (const auto b : s.raw) {
                    os << std::hex << static_cast<int>(b) << " ";
                }
                os << std::dec << ")";
            }
        }
        return os.str();
    };
    ASSERT_TRUE(sink.WaitUntil(
        [&](const std::vector<MeasurementFrame>& frames) {
            bool has_u32 = false;
            bool has_u8 = false;
            for (const auto& f : frames) {
                for (const auto& s : f.samples) {
                    if (s.name == u32_name && PhysicalIs(s, 0xDEADBEEFULL)) {
                        has_u32 = true;
                    }
                    if (s.name == u8_name && PhysicalIs(s, 0x42ULL)) {
                        has_u8 = true;
                    }
                }
            }
            return has_u32 && has_u8;
        },
        std::chrono::seconds(5)))
        << "5s 内未见 A2L 定标后的 g_basic_u32==0xDEADBEEF 与 g_basic_u8==0x42"
        << dump();

    // 首 ODT 帧必须带出有效时间戳（unit=1ns → 换算值即计数延展，v0.6 链路）
    EXPECT_TRUE(sink.WaitUntil(
        [](const std::vector<MeasurementFrame>& frames) {
            for (const auto& f : frames) {
                if (f.odt == 0U && f.timestamp_valid) {
                    return true;
                }
            }
            return false;
        },
        std::chrono::milliseconds(500)))
        << "odt==0 帧未出现 timestamp_valid（信封/取证链路断裂）";

    const auto stats = session.Statistics();
    EXPECT_EQ(stats.decode_errors, 0U) << "全链路不应有解码错误";
    EXPECT_GT(stats.dto_received, 0U);
    EXPECT_EQ(stats.dto_dropped, 0U);

    session.Stop();
    EXPECT_FALSE(session.Running());
    master->Disconnect();
}

// ---------------------------------------------------------------------------
// 实时性：会话通路同样能看到 WriteMemoryBytes 后的新值（刷新语义）
// ---------------------------------------------------------------------------

TEST_F(XcpliteMeasurementTest, SessionSeesLiveUpdates) {
    auto loaded = a2l::A2lBridge::Load(slave_.A2lPath().string());
    ASSERT_TRUE(loaded.HasValue()) << loaded.ErrorInfo().message;
    const a2l::IA2lDatabase* db = loaded.Value()->Database();
    ASSERT_NE(db, nullptr);

    const auto event = ResolveEventChannel(slave_.A2lPath(), "testev");
    ASSERT_TRUE(event.has_value());
    A2lMeasurementDatabase adapter(*db);
    EventBoundDatabase measurement_db(adapter, *event);

    FrameSink sink;  // 先于 session 声明：析构最后，worker 必先 join 后才可能触及其残骸
    MeasurementSession session(measurement_db);
    auto master = MakeMaster(&session);
    session.Bind(*master);
    master->Connect();
    AutoStopSession guard(session);  // 同主用例：失败展开不留悬垂回调
    session.SetEnvelopeMode(IdentificationFieldType::RelativeByte, 4U);
    session.SetTimestampFirstOdtOnly(true);
    session.SetTimestampUnit(1);
    const std::string u32_name = Qualified("g_basic_u32");
    session.Add(u32_name);
    ASSERT_NO_THROW(session.Prepare());

    session.Start(
        MeasurementCallback([&sink](const MeasurementFrame& f) { sink(f); }));

    // 经 Master 数据通路改写 ECU 变量 → 会话帧必须带出新值
    const auto sym = db->Find(u32_name);
    ASSERT_TRUE(sym.HasValue()) << sym.ErrorInfo().message;
    const std::uint32_t magic = 0x13572468U;
    Bytes magic_bytes{0x68U, 0x24U, 0x57U, 0x13U};  // 小端
    ASSERT_NO_THROW(
        master->WriteMemoryBytes(static_cast<Address>(sym.Value().xcp_address),
                                 sym.Value().address_extension, magic_bytes));

    const bool seen = sink.WaitUntil(
        [&](const std::vector<MeasurementFrame>& frames) {
            for (const auto& f : frames) {
                for (const auto& s : f.samples) {
                    if (s.name == u32_name && PhysicalIs(s, magic)) {
                        return true;
                    }
                }
            }
            return false;
        },
        std::chrono::seconds(5));
    EXPECT_TRUE(seen) << "会话通路未看到写回后的新值（采集非实时）";

    session.Stop();
    master->Disconnect();
}

}  // namespace
}  // namespace calmcar::xcp
