/**
 * @file measurement_session_hardening_test.cpp
 * @brief 测量会话 v0.6 硬化 L2 集成测试：时间戳流 / 统计连续性 / 错误恢复。
 *
 * 覆盖计划 §5.4 + v0.6 验收项（测量子系统代码增长计划 :689-743）：
 *   - 32 位时间戳经会话流回卷延展（raw 0xFFFFFFFF → 0 = 2^32 ns，unit=1）；
 *   - 每 DAQ List 一条独立换算流（不同事件通道 raw 互不污染回卷判定）；
 *   - 未知单位（SetTimestampUnit(0)，R13 无权威码表）→ timestamp_valid=false
 *     且仅保留 timestamp_raw（不猜，B-3）；
 *   - 运行中 SetTimestampUnit 拒绝；
 *   - Stop→Start 基线自然复位（不跨流误判回卷），统计计数跨流累计不清零；
 *   - 截断净荷 → 越界切片样本 valid=false（帧仍回调，不额外计 decode_errors）；
 *   - 畸形帧 + 未规划 ODT + 截断帧混合序列：错误只计畸形帧，后续正常帧恢复。
 *
 * 夹具（FakeDb/SessionSlave/Rig/MakeFrame/FrameCollector）与
 * measurement_session_test.cpp 同构：匿名命名空间内独立副本（测试 TU 互不
 * 共享符号，遵循既有测试组织方式）。
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libxcp/measurement/measurement_database.hpp"
#include "libxcp/measurement/measurement_result.hpp"
#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/protocol_types.hpp"
#include "libxcp/xcp_error.hpp"
#include "libxcp/xcp_master.hpp"
#include "mock_transport.hpp"

namespace calmcar::xcp {
namespace {

using test::MockTransport;

/// @brief 内存版测量数据库：符号表可注入，换算值可指定失败
class FakeDb final : public IMeasurementDatabase {
 public:
  FakeDb() = default;  // 基类删除拷贝构造，显式恢复默认构造

  MeasurementResult<MeasurementSymbolInfo> Find(
      std::string_view name) const override {
    const auto it = symbols.find(std::string(name));
    if (it == symbols.end()) {
      return detail::MakeMeasurementError<MeasurementSymbolInfo>(
          MeasurementErrorCode::NotFound, "未找到测量: " + std::string(name),
          std::string(name));
    }
    return detail::MakeMeasurementOk(it->second);
  }

  MeasurementResult<MeasurementValue> ToPhysical(
      std::string_view name, BytesView raw) const override {
    if (failing.count(std::string(name)) != 0) {
      return detail::MakeMeasurementError<MeasurementValue>(
          MeasurementErrorCode::UnsupportedConversion, "注入的换算失败",
          std::string(name));
    }
    // 确定性换算：首字节 + 长度（断言侧按同一公式核对）
    const std::int64_t v =
        static_cast<std::int64_t>(raw.size()) +
        (raw.empty() ? 0 : static_cast<std::int64_t>(raw[0]));
    return detail::MakeMeasurementOk(MeasurementValue{v});
  }

  std::map<std::string, MeasurementSymbolInfo> symbols;
  std::set<std::string> failing;  ///< 命中即 ToPhysical 报错的符号
};

/// @brief 构造符号信息（地址/扩展/事件通道/元素宽/元素数）
MeasurementSymbolInfo MakeInfo(std::uint32_t address,
                               std::uint16_t event_channel,
                               std::uint8_t element_size_bytes) {
  return MeasurementSymbolInfo{address, 0, event_channel, element_size_bytes, 1};
}

/**
 * @brief 会话用脚本化 Slave：静态应答动态整表通路 + 取证响应。
 * @details CONNECT 协商 MAX_DTO=256、Intel/Byte；
 *          GET_DAQ_PROCESSOR_INFO 声明 DYNAMIC+TIMESTAMP（0x11）；
 *          GET_DAQ_RESOLUTION_INFO 声明 4 字节 RAW/fixed 时间戳（0x0C）。
 */
class SessionSlave {
 public:
  Bytes operator()(BytesView packet) {
    if (packet.empty()) {
      return {};
    }
    const auto cmd = static_cast<CommandCode>(packet[0]);
    ++m_counts_[cmd];
    switch (cmd) {
      case CommandCode::Connect:
        // resource=0x15, comm_mode=0xC0(Intel/Byte), CTO=16, DTO=256,
        // 协议/传输版本 0x10
        return Res({0x15, 0xC0, 0x10, 0x00, 0x01, 0x10, 0x10});
      case CommandCode::Disconnect:
        return Res({});
      case CommandCode::GetStatus:
        return Res({0x00, 0x00, 0x01, 0x07, 0x00});
      case CommandCode::GetCommModeInfo:
        return Res({0x00, 0x0E, 0x00, 0x04, 0x02, 0x08, 0x13});
      case CommandCode::Synch:
        return Err(ErrorCode::CmdSynch);
      case CommandCode::GetDaqProcessorInfo:
        // props=0x11（DYNAMIC+TIMESTAMP），MAX_DAQ=4，MAX_EVENT=2，
        // MIN_DAQ=0，DAQ_KEY=0
        return Res({0x11, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00});
      case CommandCode::GetDaqResolutionInfo:
        // GRAN_DAQ=1, MAX_ODT_ENTRY=8, GRAN_STIM=1, MAX_SIZE_STIM=8,
        // TIMESTAMP_MODE=0x0C（size_code=4→4 字节, fixed）, TICKS=1
        return Res({0x01, 0x08, 0x01, 0x08, 0x0C, 0x01, 0x00});
      case CommandCode::FreeDaq:
      case CommandCode::AllocDaq:
      case CommandCode::AllocOdt:
      case CommandCode::AllocOdtEntry:
      case CommandCode::SetDaqPtr:
      case CommandCode::WriteDaq:
      case CommandCode::ClearDaqList:
      case CommandCode::StartStopSynch:
        return Res({});
      case CommandCode::SetDaqListMode:
        return Res({packet[1], packet[2], packet[3], packet[4], packet[5]});
      case CommandCode::StartStopDaqList:
        return Res({0x00});  // FIRST_PID=0（相对模式不参与推导）
      default:
        return Err(ErrorCode::CmdUnknown);
    }
  }

  /// @brief 某命令被收到的次数
  [[nodiscard]] int Count(CommandCode cmd) const {
    const auto it = m_counts_.find(cmd);
    return it == m_counts_.end() ? 0 : it->second;
  }

 private:
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

  std::map<CommandCode, int> m_counts_;
};

/// @brief 构造一个 RelativeWord 布局的 DTO 帧（2 字节识别头 + 4 字节 ts + 净荷）
Bytes MakeFrame(std::uint8_t daq, std::uint8_t rel_odt, std::uint32_t ts,
                BytesView payload) {
  Bytes frame;
  frame.push_back(daq);
  frame.push_back(rel_odt);
  for (std::size_t i = 0; i < 4U; ++i) {
    frame.push_back(static_cast<std::uint8_t>((ts >> (8U * i)) & 0xFFU));
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

/**
 * @brief 夹具：db → session → master（以 &session 为监听器）→ Bind → Connect。
 * @details 成员声明顺序决定析构顺序（逆序）：master 先声明、后析构，
 *          session 后声明、先析构（其 Stop/join 时 master 仍存活）。
 */
struct Rig {
  FakeDb db;
  std::unique_ptr<MockTransport> transport_owner;
  MockTransport* transport{nullptr};
  SessionSlave slave;
  std::unique_ptr<XcpMaster> master;
  MeasurementSession session{db};  ///< 必须先于 master 构造（作为监听器）

  Rig() {
    db.symbols["EngineSpeed"] = MakeInfo(0x1000U, 0, 4);
    db.symbols["Coolant"] = MakeInfo(0x1004U, 0, 2);
    db.symbols["VehicleSpeed"] = MakeInfo(0x2000U, 2, 4);

    transport_owner = std::make_unique<MockTransport>();
    transport = transport_owner.get();
    transport->SetResponse([this](BytesView p) { return slave(p); });
    master = std::make_unique<XcpMaster>(
        std::move(transport_owner),
        CommandTimeouts{std::chrono::milliseconds(150),
                        std::chrono::milliseconds(150), 2},
        &session);
    session.Bind(*master);
    master->Connect();
    // v0.6：时间戳单位显式注入（R13 无权威码表，不猜）。unit=1ns 让换算
    // 值与 raw（含回卷延展后的计数）保持 1:1。
    session.SetTimestampUnit(1);
  }
};

/// @brief 回调帧收集器（worker 线程写入，测试线程条件等待）
class FrameCollector {
 public:
  void operator()(const MeasurementFrame& frame) {
    std::lock_guard<std::mutex> lock(m_mutex_);
    m_frames_.push_back(frame);
    m_cv_.notify_all();
  }

  /// @brief 等待至少 n 帧；超时返回 false
  [[nodiscard]] bool WaitFor(std::size_t n,
                             std::chrono::milliseconds timeout =
                                 std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(m_mutex_);
    return m_cv_.wait_for(lock, timeout,
                          [&] { return m_frames_.size() >= n; });
  }

  std::vector<MeasurementFrame> Frames() {
    std::lock_guard<std::mutex> lock(m_mutex_);
    return m_frames_;
  }

 private:
  std::mutex m_mutex_;
  std::condition_variable m_cv_;
  std::vector<MeasurementFrame> m_frames_;
};

constexpr std::uint64_t kMod32 = 1ULL << 32U;

// ---------------------------------------------------------------------------
// 时间戳回卷：经完整会话流（信封解码 → 每流转换器 → 帧）
// ---------------------------------------------------------------------------

TEST(MeasurementSessionHardening, TimestampWrapExtendsThroughSession) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();
  FrameCollector collector;
  rig.session.Start(
      MeasurementCallback([&collector](const MeasurementFrame& f) { collector(f); }));

  Bytes payload{0x01, 0x02, 0x03, 0x04};
  const Bytes f1 = MakeFrame(0, 0, 0xFFFFFFFFU, BytesView{payload});
  rig.transport->InjectPacket(BytesView{f1});
  const Bytes f2 = MakeFrame(0, 0, 0U, BytesView{payload});
  rig.transport->InjectPacket(BytesView{f2});
  ASSERT_TRUE(collector.WaitFor(2));

  const auto frames = collector.Frames();
  ASSERT_EQ(frames.size(), 2U);
  // 首帧建基线：raw=2^32-1，无回卷
  EXPECT_TRUE(frames[0].timestamp_valid);
  EXPECT_EQ(frames[0].timestamp_raw, 0xFFFFFFFFULL);
  EXPECT_EQ(frames[0].timestamp, std::chrono::nanoseconds(0xFFFFFFFFLL));
  // 次帧 2^32-1 → 0：跨过一次 32 位边界，延展为 2^32 ns，raw 保留
  EXPECT_TRUE(frames[1].timestamp_valid);
  EXPECT_EQ(frames[1].timestamp_raw, 0ULL);
  EXPECT_EQ(frames[1].timestamp, std::chrono::nanoseconds(static_cast<std::int64_t>(kMod32)));
  EXPECT_EQ(rig.session.Statistics().timestamp_wraps, 1U);
  rig.session.Stop();
}

TEST(MeasurementSessionHardening, PerListStreamsWrapIndependently) {
  Rig rig;
  rig.session.Add("EngineSpeed");   // event 0 → list 0
  rig.session.Add("VehicleSpeed");  // event 2 → list 1
  rig.session.Prepare();
  FrameCollector collector;
  rig.session.Start(
      MeasurementCallback([&collector](const MeasurementFrame& f) { collector(f); }));

  // 交错注入：list0: MAX→0（回卷 1 次）；list1: 10→0（回卷 1 次）。
  // 若两路共用一条流，list1 的 10→MAX 序列会互相污染判定。
  Bytes payload{0x0A, 0x0B, 0x0C, 0x0D};
  const Bytes a1 = MakeFrame(0, 0, 0xFFFFFFFFU, BytesView{payload});
  const Bytes b1 = MakeFrame(1, 0, 10U, BytesView{payload});
  const Bytes a2 = MakeFrame(0, 0, 0U, BytesView{payload});
  const Bytes b2 = MakeFrame(1, 0, 0U, BytesView{payload});
  rig.transport->InjectPacket(BytesView{a1});
  rig.transport->InjectPacket(BytesView{b1});
  rig.transport->InjectPacket(BytesView{a2});
  rig.transport->InjectPacket(BytesView{b2});
  ASSERT_TRUE(collector.WaitFor(4));

  const auto frames = collector.Frames();
  ASSERT_EQ(frames.size(), 4U);
  EXPECT_EQ(frames[2].daq_list, 0U);
  EXPECT_EQ(frames[2].timestamp, std::chrono::nanoseconds(static_cast<std::int64_t>(kMod32)));
  EXPECT_EQ(frames[1].daq_list, 1U);
  EXPECT_EQ(frames[1].timestamp, std::chrono::nanoseconds(10));
  EXPECT_EQ(frames[3].daq_list, 1U);
  EXPECT_EQ(frames[3].timestamp, std::chrono::nanoseconds(static_cast<std::int64_t>(kMod32)));
  // 每流各 1 次 → 共 2
  EXPECT_EQ(rig.session.Statistics().timestamp_wraps, 2U);
  rig.session.Stop();
}

TEST(MeasurementSessionHardening, UnknownUnitKeepsRawOnly) {
  Rig rig;
  rig.session.SetTimestampUnit(0);  // 未知单位（R13）→ 不猜
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();
  FrameCollector collector;
  rig.session.Start(
      MeasurementCallback([&collector](const MeasurementFrame& f) { collector(f); }));

  Bytes payload{0x01, 0x02, 0x03, 0x04};
  const Bytes f1 = MakeFrame(0, 0, 999U, BytesView{payload});
  rig.transport->InjectPacket(BytesView{f1});
  ASSERT_TRUE(collector.WaitFor(1));

  const auto frames = collector.Frames();
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_FALSE(frames[0].timestamp_valid);
  EXPECT_EQ(frames[0].timestamp_raw, 999ULL);  // raw 保留（B-3）
  EXPECT_EQ(frames[0].timestamp, std::chrono::nanoseconds(0));
  EXPECT_TRUE(frames[0].samples[0].valid);  // 净荷链路不受影响
  rig.session.Stop();
}

TEST(MeasurementSessionHardening, SetTimestampUnitThrowsWhenRunning) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();
  FrameCollector collector;
  rig.session.Start(
      MeasurementCallback([&collector](const MeasurementFrame& f) { collector(f); }));
  EXPECT_THROW(rig.session.SetTimestampUnit(2), XcpException);
  rig.session.Stop();
  // 停止后可再设
  rig.session.SetTimestampUnit(2);
}

TEST(MeasurementSessionHardening, StopStartResetsBaselineStatsCumulative) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();

  FrameCollector collector1;
  rig.session.Start(
      MeasurementCallback([&collector1](const MeasurementFrame& f) { collector1(f); }));
  Bytes payload{0x01, 0x02, 0x03, 0x04};
  const Bytes hi = MakeFrame(0, 0, 0xFFFFFFFFU, BytesView{payload});
  const Bytes lo = MakeFrame(0, 0, 0U, BytesView{payload});
  rig.transport->InjectPacket(BytesView{hi});
  rig.transport->InjectPacket(BytesView{lo});
  ASSERT_TRUE(collector1.WaitFor(2));
  EXPECT_EQ(rig.session.Statistics().timestamp_wraps, 1U);
  rig.session.Stop();

  // Stop→Start：worker 线程重建 → 每流转换器基线自然复位。新流首帧 raw=5
  // 不与旧流末帧 0 混判（若跨流续接，5 > 0 不判回卷；关键是基线已重建）。
  FrameCollector collector2;
  rig.session.Start(
      MeasurementCallback([&collector2](const MeasurementFrame& f) { collector2(f); }));
  const Bytes fresh = MakeFrame(0, 0, 5U, BytesView{payload});
  rig.transport->InjectPacket(BytesView{fresh});
  ASSERT_TRUE(collector2.WaitFor(1));

  const auto frames = collector2.Frames();
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_TRUE(frames[0].timestamp_valid);
  EXPECT_EQ(frames[0].timestamp, std::chrono::nanoseconds(5));  // 新基线，无周期
  // 统计跨 Stop→Start 累计不清零
  const auto stats = rig.session.Statistics();
  EXPECT_EQ(stats.timestamp_wraps, 1U);
  EXPECT_EQ(stats.dto_received, 3U);
  rig.session.Stop();
}

// ---------------------------------------------------------------------------
// 错误恢复：畸形 / 未规划 ODT / 截断净荷混合序列
// ---------------------------------------------------------------------------

TEST(MeasurementSessionHardening, MixedBadFramesRecoverAndSliceDegrades) {
  Rig rig;
  rig.session.Add("EngineSpeed");  // 4 字节，list0/odt0
  rig.session.Prepare();
  FrameCollector collector;
  rig.session.Start(
      MeasurementCallback([&collector](const MeasurementFrame& f) { collector(f); }));

  Bytes payload{0x11, 0x22, 0x33, 0x44};
  // 1) 畸形：短于识别头+时间戳 → decode_errors++
  Bytes too_short{0x00, 0x00, 0x01};
  rig.transport->InjectPacket(BytesView{too_short});
  // 2) 已规划 list、未规划 odt=1 → 静默忽略（不计错误、不回调）
  const Bytes wrong_odt = MakeFrame(0, 1, 5U, BytesView{payload});
  rig.transport->InjectPacket(BytesView{wrong_odt});
  // 3) 截断净荷：只有 2 字节（规划 4 字节）→ 越界切片 valid=false，仍回调
  Bytes truncated_payload{0xAA, 0xBB};
  const Bytes truncated = MakeFrame(0, 0, 77U, BytesView{truncated_payload});
  rig.transport->InjectPacket(BytesView{truncated});
  // 4) 正常帧：确认 worker 恢复正常
  const Bytes good = MakeFrame(0, 0, 88U, BytesView{payload});
  rig.transport->InjectPacket(BytesView{good});
  ASSERT_TRUE(collector.WaitFor(2));

  const auto stats = rig.session.Statistics();
  EXPECT_EQ(stats.dto_received, 4U);
  EXPECT_EQ(stats.decode_errors, 1U);  // 仅畸形帧计入

  const auto frames = collector.Frames();
  ASSERT_EQ(frames.size(), 2U);
  // 截断帧：样本降级，时间戳链路完好
  EXPECT_TRUE(frames[0].timestamp_valid);
  EXPECT_EQ(frames[0].timestamp, std::chrono::nanoseconds(77));
  ASSERT_EQ(frames[0].samples.size(), 1U);
  EXPECT_FALSE(frames[0].samples[0].valid);
  EXPECT_TRUE(frames[0].samples[0].raw.empty());
  // 正常帧完整
  EXPECT_EQ(frames[1].timestamp, std::chrono::nanoseconds(88));
  ASSERT_EQ(frames[1].samples.size(), 1U);
  EXPECT_TRUE(frames[1].samples[0].valid);
  EXPECT_EQ(frames[1].samples[0].raw, payload);
  rig.session.Stop();
}

}  // namespace
}  // namespace calmcar::xcp
