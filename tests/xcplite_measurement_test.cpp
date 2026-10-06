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
 *  3) A2L EVENT 的 event id/cycle/unit 与 QueryDaqEventInfo 交叉核验；
 *     A2L 不声明 symbol→event 关联，因此绑定表由调用点显式提供，缺失/多值
 *     均报 InvalidLayout，不把 adapter 的中性 event_channel=0 当成证据；
 *  4) DTO identification field 与时间戳宽度/单位/TICKS 从运行时查询读取；
 *     XCPlite 专有 4-byte RelativeByte header 与首 ODT timestamp 仅作为
 *     单独 profile 配置，不推广成通用 XCP 默认值。
 */

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
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

/// @brief A2L EVENT 声明中可用于运行时交叉核验的元数据。
struct RuntimeEventInfo {
    std::string name;
    std::uint16_t channel{0};
    std::uint8_t time_cycle{0};
    std::uint8_t time_unit{0};
};

/// @brief 按 A2L 标准 EVENT 声明读取唯一事件；拒绝缺失、重复和越界字段。
std::optional<RuntimeEventInfo> ResolveRuntimeEvent(
    const std::filesystem::path& a2l_path, const std::string& event_name) {
    std::ifstream file(a2l_path);
    if (!file) {
        return std::nullopt;
    }

    std::optional<RuntimeEventInfo> found;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream row(line);
        std::string begin;
        std::string kind;
        std::string name;
        std::string short_name;
        std::string channel;
        std::string type;
        std::string max_daq;
        std::string time_cycle;
        std::string time_unit;
        std::string priority;
        if (!(row >> begin >> kind) || begin != "/begin" || kind != "EVENT") {
            continue;
        }
        if (!(row >> std::quoted(name) >> std::quoted(short_name) >> channel >>
              type >> max_daq >> time_cycle >> time_unit >> priority)) {
            return std::nullopt;
        }
        if (name != event_name && short_name != event_name) {
            continue;
        }
        try {
            std::size_t used = 0;
            const unsigned long channel_value = std::stoul(channel, &used, 0);
            if (used != channel.size() ||
                channel_value > std::numeric_limits<std::uint16_t>::max()) {
                return std::nullopt;
            }
            const unsigned long cycle_value = std::stoul(time_cycle, &used, 10);
            if (used != time_cycle.size() || cycle_value > 255UL) {
                return std::nullopt;
            }
            const unsigned long unit_value = std::stoul(time_unit, &used, 10);
            if (used != time_unit.size() || unit_value > 255UL || found) {
                return std::nullopt;
            }
            found = RuntimeEventInfo{name,
                                     static_cast<std::uint16_t>(channel_value),
                                     static_cast<std::uint8_t>(cycle_value),
                                     static_cast<std::uint8_t>(unit_value)};
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return found;
}

/// @brief 明确记录来源的 DTO envelope/timestamp 配置（本测试 TU 的 profile）。
struct MeasurementRuntimeProfile {
    IdentificationFieldType identification{IdentificationFieldType::Absolute};
    std::size_t identification_bytes{0};
    std::uint64_t timestamp_unit_ns{0};
    bool timestamp_first_odt_only{false};
};

/// @brief 从 XCP 查询取证 XCPlite profile；无来源或 A2L/Slave 不一致即拒绝。
std::optional<MeasurementRuntimeProfile> QueryXcpliteProfile(
    XcpMaster& master, const RuntimeEventInfo& event, std::string& error) {
    const auto processor = master.QueryDaqProcessorInfo();
    const auto resolution = master.QueryDaqResolutionInfo();
    const auto runtime_event = master.QueryDaqEventInfo(event.channel);
    if (!processor || !resolution || !runtime_event) {
        error = "缺少 DAQ processor/resolution/event 运行时取证";
        return std::nullopt;
    }
    // XCPlite advertises DAQ_HDR_ODT_FIL_DAQW (key-byte code 3): its
    // four-byte vendor envelope is mapped to the decoder's RelativeByte mode.
    // Source: thirdparty/XCPlite/src/xcp.h:384-388 and observed DTO dialect.
    if (processor->key_byte.identification_field_type != 3U) {
        error = "Slave DAQ_KEY_BYTE 未声明 XCPlite ODT_FIL_DAQW (code 3)";
        return std::nullopt;
    }
    if (runtime_event->time_cycle != event.time_cycle ||
        runtime_event->time_unit != event.time_unit) {
        error = "A2L EVENT 周期与 GET_DAQ_EVENT_INFO 不一致";
        return std::nullopt;
    }

    // TIMESTAMP_MODE size_code 4 = 4 bytes (docs §7.5.4.10; same mapping
    // enforced by XcpMaster::DaqTimestampBytesCached during Prepare).
    if (resolution->timestamp_mode.size_code != 4U ||
        !resolution->timestamp_mode.fixed ||
        resolution->timestamp_ticks == 0U) {
        error = "XCPlite profile 要求固定 32-bit timestamp 且 TICKS 非零";
        return std::nullopt;
    }

    // 来源：thirdparty/XCPlite/src/xcplite.c:2520-2530 与
    // thirdparty/XCPlite/src/xcp_cfg.h:426-433；标准单位码 0..6 分别为
    // 1ns、10ns、100ns、1us、10us、100us、1ms。乘运行时 TICKS 得每计数 ns。
    constexpr std::uint64_t kUnitNs[] = {1U, 10U, 100U, 1000U,
                                         10000U, 100000U, 1000000U};
    const std::uint8_t unit_code = resolution->timestamp_mode.unit_code;
    if (unit_code >= std::size(kUnitNs) ||
        resolution->timestamp_ticks >
            std::numeric_limits<std::uint64_t>::max() / kUnitNs[unit_code]) {
        error = "timestamp unit code 未知或单位换算溢出";
        return std::nullopt;
    }

    // XCPlite profile dialect: RelativeByte wire header is
    // [ODTrel][0xAA][DAQ16 LE] (4 bytes), and only ODT0 carries timestamp.
    // These two quirks are profile-specific, not inferred from generic XCP.
    return MeasurementRuntimeProfile{
        IdentificationFieldType::RelativeByte, 4U,
        kUnitNs[unit_code] * resolution->timestamp_ticks, true};
}

/// @brief 将已取证的 profile 写入会话；配置必须早于 Prepare。
void ApplyProfile(MeasurementSession& session,
                  const MeasurementRuntimeProfile& profile) {
    session.SetEnvelopeMode(profile.identification,
                            profile.identification_bytes);
    session.SetTimestampFirstOdtOnly(profile.timestamp_first_odt_only);
    session.SetTimestampUnit(profile.timestamp_unit_ns);
}

/// @brief 明确变量→事件绑定的数据库装饰器；缺失/多值绑定均失败，不默认 0。
class EventBoundDatabase final : public IMeasurementDatabase {
 public:
  using Binding = std::pair<std::string, std::uint16_t>;

  EventBoundDatabase(IMeasurementDatabase& inner,
                     std::vector<Binding> bindings)
      : m_inner_(inner) {
    for (const Binding& binding : bindings) {
        m_bindings_[binding.first].insert(binding.second);
    }
  }

  EventBoundDatabase(const EventBoundDatabase&) = delete;
  EventBoundDatabase& operator=(const EventBoundDatabase&) = delete;

  MeasurementResult<MeasurementSymbolInfo> Find(
      std::string_view name) const override {
    MeasurementResult<MeasurementSymbolInfo> res = m_inner_.Find(name);
    if (!res.HasValue()) {
        return res;
    }
    const auto binding = m_bindings_.find(std::string(name));
    if (binding == m_bindings_.end() || binding->second.empty()) {
        return detail::MakeMeasurementError<MeasurementSymbolInfo>(
            MeasurementErrorCode::InvalidLayout,
            "测量符号缺少显式事件通道绑定", std::string(name));
    }
    if (binding->second.size() != 1U) {
        return detail::MakeMeasurementError<MeasurementSymbolInfo>(
            MeasurementErrorCode::InvalidLayout,
            "测量符号存在歧义事件通道绑定", std::string(name));
    }
    MeasurementSymbolInfo info = res.Value();
    info.event_channel = *binding->second.begin();
    return detail::MakeMeasurementOk(info);
  }

  MeasurementResult<MeasurementValue> ToPhysical(
      std::string_view name, BytesView raw) const override {
    return m_inner_.ToPhysical(name, raw);
  }

 private:
  IMeasurementDatabase& m_inner_;  ///< 被包装的 A2L 适配器
  std::map<std::string, std::set<std::uint16_t>> m_bindings_;
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

TEST_F(XcpliteMeasurementTest, RuntimeEventQueryRejectsInvalidChannel) {
    auto master = MakeConnectedMaster();
    EXPECT_THROW((void)master->QueryDaqEventInfo(0xFFFFU), XcpException);
}

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

    // —— 3) EVENT 来自实际 UPLOAD 的 A2L；变量→事件关联由调用点显式提供 ——
    const auto event = ResolveRuntimeEvent(uploaded_a2l, "testev");
    ASSERT_TRUE(event.has_value()) << "上传 A2L 中缺少唯一、有效的 testev EVENT";
    const std::string u32_name = Qualified("g_basic_u32");
    const std::string u8_name = Qualified("g_basic_u8");
    A2lMeasurementDatabase adapter(*db);

    EventBoundDatabase unbound(adapter, {});
    const auto missing_binding = unbound.Find(u32_name);
    ASSERT_FALSE(missing_binding.HasValue());
    EXPECT_EQ(missing_binding.ErrorInfo().code,
              MeasurementErrorCode::InvalidLayout);
    EventBoundDatabase ambiguous(
        adapter, {{u32_name, event->channel},
                  {u32_name, static_cast<std::uint16_t>(event->channel ^ 1U)}});
    const auto ambiguous_binding = ambiguous.Find(u32_name);
    ASSERT_FALSE(ambiguous_binding.HasValue());
    EXPECT_EQ(ambiguous_binding.ErrorInfo().code,
              MeasurementErrorCode::InvalidLayout);

    EventBoundDatabase measurement_db(
        adapter, {{u32_name, event->channel}, {u8_name, event->channel}});

    // FrameSink 先于 session/master 声明：析构顺序为逆序（sink 最后销毁），
    // 断言提前 return 时 session 析构先 join worker，回调绝不触碰悬 sink。
    FrameSink sink;

    // —— 4) 会话：Bind → Connect → 查询运行时配置并应用 XCPlite profile ——
    MeasurementSession session(measurement_db);
    auto master = MakeMaster(&session);
    session.Bind(*master);
    master->Connect();
    AutoStopSession guard(session);
    std::string profile_error;
    const auto profile = QueryXcpliteProfile(*master, *event, profile_error);
    ASSERT_TRUE(profile.has_value()) << profile_error;
    ApplyProfile(session, *profile);
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

    // 首 ODT 帧必须带有效时间戳；物理单位来自 profile 取证而非本测试猜值。
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

    const auto event = ResolveRuntimeEvent(slave_.A2lPath(), "testev");
    ASSERT_TRUE(event.has_value());
    const std::string u32_name = Qualified("g_basic_u32");
    A2lMeasurementDatabase adapter(*db);
    EventBoundDatabase measurement_db(
        adapter, {{u32_name, event->channel}});

    FrameSink sink;  // 先于 session 声明：析构最后，worker 必先 join 后才可能触及其残骸
    MeasurementSession session(measurement_db);
    auto master = MakeMaster(&session);
    session.Bind(*master);
    master->Connect();
    AutoStopSession guard(session);  // 同主用例：失败展开不留悬垂回调
    std::string profile_error;
    const auto profile = QueryXcpliteProfile(*master, *event, profile_error);
    ASSERT_TRUE(profile.has_value()) << profile_error;
    ApplyProfile(session, *profile);
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
