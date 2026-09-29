/**
 * @file measurement_session_test.cpp
 * @brief 测量会话（MeasurementSession）L2 集成测试（v0.5）。
 *
 * 覆盖计划 §5.6 验收项：
 *   - 完整链路：Add → Prepare（规划 + 动态整表下发）→ Start → 注入 DTO →
 *     worker 线程解码 → 回调收到 MeasurementFrame；
 *   - 多事件分组 → 多 DAQ List 路由（daq_list/odt 精确匹配）；
 *   - 未 Bind / 未 Connect / 未 Prepare 的前置拒绝；
 *   - 运行中 Add/Remove 拒绝、重复 Bind/Start 拒绝；
 *   - 畸形帧 → decode_errors 计数且不回调；未规划 (daq,odt) 帧静默忽略；
 *   - ToPhysical 失败 → 仅该样本 valid=false（不整帧丢弃）；
 *   - 未运行时 OnDto 不入队不计数；Stop 幂等、析构 join；
 *   - 统计计数（received/dropped/decode_errors）。
 *
 * 识别字段模式用会话默认取证（RelativeWord：byte0=DAQ 号、byte1=相对 ODT，
 * 头长 2），时间戳 4 字节（GET_DAQ_RESOLUTION_INFO TIMESTAMP_MODE=0x0C），
 * 注入帧按该布局构造。
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
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
    // v0.6：时间戳单位必须显式取证注入（R13 无权威码表，不猜）。此处取
    // unit=1ns 让换算值与 raw 计数保持 1:1，既有断言（timestamp==raw）语义不变。
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

// ---------------------------------------------------------------------------
// 前置拒绝与生命周期
// ---------------------------------------------------------------------------

TEST(MeasurementSessionLifecycle, PrepareWithoutBindThrows) {
  FakeDb db;
  MeasurementSession session(db);  // 未 Bind
  EXPECT_THROW(session.Prepare(), XcpException);
}

TEST(MeasurementSessionLifecycle, DuplicateBindThrows) {
  Rig rig;
  EXPECT_THROW(rig.session.Bind(*rig.master), XcpException);
}

TEST(MeasurementSessionLifecycle, StartWithoutPrepareThrows) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  FrameCollector collector;
  EXPECT_THROW(
      rig.session.Start(
          MeasurementCallback([&collector](const MeasurementFrame& f) { collector(f); })),
      XcpException);
  EXPECT_FALSE(rig.session.Running());
}

TEST(MeasurementSessionLifecycle, AddDuringRunningThrows) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();
  FrameCollector collector;
  rig.session.Start(MeasurementCallback(
      [&collector](const MeasurementFrame& f) { collector(f); }));
  EXPECT_TRUE(rig.session.Running());
  EXPECT_THROW(rig.session.Add("Coolant"), XcpException);
  EXPECT_THROW(rig.session.Remove("EngineSpeed"), XcpException);
  rig.session.Stop();
  EXPECT_FALSE(rig.session.Running());
  // Stop 幂等
  rig.session.Stop();
}

TEST(MeasurementSessionLifecycle, DtoIgnoredBeforeStartAndAfterStop) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();
  // 未 Start：OnDto 不入队不计数
  Bytes payload{0x11, 0x22, 0x33, 0x44};
  rig.transport->InjectPacket(
      BytesView{MakeFrame(0, 0, 1, BytesView{payload})});
  EXPECT_EQ(rig.session.Statistics().dto_received, 0U);

  FrameCollector collector;
  rig.session.Start(MeasurementCallback(
      [&collector](const MeasurementFrame& f) { collector(f); }));
  rig.session.Stop();
  // Stop 之后再注入：不计数
  rig.transport->InjectPacket(
      BytesView{MakeFrame(0, 0, 2, BytesView{payload})});
  EXPECT_EQ(rig.session.Statistics().dto_received, 0U);
}

// ---------------------------------------------------------------------------
// 完整数据通路
// ---------------------------------------------------------------------------

TEST(MeasurementSessionPipeline, SingleVariableFullChain) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();

  FrameCollector collector;
  rig.session.Start(MeasurementCallback(
      [&collector](const MeasurementFrame& f) { collector(f); }));

  Bytes payload{0x04, 0x03, 0x02, 0x01};
  rig.transport->InjectPacket(
      BytesView{MakeFrame(/*daq=*/0, /*rel_odt=*/0, /*ts=*/1234,
                          BytesView{payload})});
  ASSERT_TRUE(collector.WaitFor(1));

  const auto frames = collector.Frames();
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0].daq_list, 0U);
  EXPECT_EQ(frames[0].odt, 0U);
  EXPECT_EQ(frames[0].timestamp, std::chrono::nanoseconds(1234));
  ASSERT_EQ(frames[0].samples.size(), 1U);
  EXPECT_EQ(frames[0].samples[0].name, "EngineSpeed");
  EXPECT_EQ(frames[0].samples[0].raw, payload);
  EXPECT_TRUE(frames[0].samples[0].valid);
  // FakeDb 换算公式：raw[0] + size = 0x04 + 4
  EXPECT_EQ(std::get<std::int64_t>(frames[0].samples[0].value), 8);

  const auto stats = rig.session.Statistics();
  EXPECT_EQ(stats.dto_received, 1U);
  EXPECT_EQ(stats.decode_errors, 0U);
  EXPECT_EQ(stats.dto_dropped, 0U);
  EXPECT_EQ(stats.timestamp_wraps, 0U);
  rig.session.Stop();
}

TEST(MeasurementSessionPipeline, MultiEventRoutesToSeparateLists) {
  Rig rig;
  rig.session.Add("EngineSpeed");    // event 0 → list 0
  rig.session.Add("VehicleSpeed");   // event 2 → list 1
  rig.session.Add("Coolant");        // event 0 → list 0（组内保持顺序）
  rig.session.Prepare();

  // 动态整表：ALLOC_DAQ 一次给 2 个 List
  EXPECT_EQ(rig.slave.Count(CommandCode::AllocDaq), 1);
  EXPECT_EQ(rig.slave.Count(CommandCode::StartStopSynch), 0);  // 未 Start

  FrameCollector collector;
  rig.session.Start(MeasurementCallback(
      [&collector](const MeasurementFrame& f) { collector(f); }));
  EXPECT_EQ(rig.slave.Count(CommandCode::StartStopSynch), 1);

  // list0/odt0 含两个切片：EngineSpeed(偏移0,4B) + Coolant(偏移4,2B)
  Bytes payload_a{0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
  Bytes payload_b{0x01, 0x02, 0x03, 0x04};  // VehicleSpeed 4 字节
  rig.transport->InjectPacket(
      BytesView{MakeFrame(0, 0, 100, BytesView{payload_a})});
  rig.transport->InjectPacket(
      BytesView{MakeFrame(1, 0, 200, BytesView{payload_b})});
  ASSERT_TRUE(collector.WaitFor(2));

  const auto frames = collector.Frames();
  ASSERT_EQ(frames.size(), 2U);
  // 队列 FIFO：第一帧 list0，第二帧 list1（odt 相对还原为绝对 0）
  EXPECT_EQ(frames[0].daq_list, 0U);
  EXPECT_EQ(frames[0].odt, 0U);
  EXPECT_EQ(frames[0].timestamp, std::chrono::nanoseconds(100));
  ASSERT_EQ(frames[0].samples.size(), 2U);
  EXPECT_EQ(frames[0].samples[0].name, "EngineSpeed");
  EXPECT_EQ(frames[0].samples[0].raw,
            Bytes(payload_a.begin(), payload_a.begin() + 4));
  EXPECT_TRUE(frames[0].samples[0].valid);
  EXPECT_EQ(frames[0].samples[1].name, "Coolant");
  EXPECT_EQ(frames[0].samples[1].raw,
            Bytes(payload_a.begin() + 4, payload_a.end()));
  EXPECT_TRUE(frames[0].samples[1].valid);
  EXPECT_EQ(frames[1].daq_list, 1U);
  ASSERT_EQ(frames[1].samples.size(), 1U);
  EXPECT_EQ(frames[1].samples[0].name, "VehicleSpeed");
  EXPECT_TRUE(frames[1].samples[0].valid);
  rig.session.Stop();
}

TEST(MeasurementSessionPipeline, MalformedFrameCountsDecodeErrorWithoutCallback) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();
  FrameCollector collector;
  rig.session.Start(MeasurementCallback(
      [&collector](const MeasurementFrame& f) { collector(f); }));

  // 短于 2 字节识别头 + 4 字节时间戳 → MalformedPacket
  Bytes too_short{0x00, 0x00, 0x01};
  rig.transport->InjectPacket(BytesView{too_short});
  // 未规划的路由 (daq=5)：静默忽略，不计错误
  Bytes payload{0x01, 0x02, 0x03, 0x04};
  rig.transport->InjectPacket(
      BytesView{MakeFrame(5, 0, 1, BytesView{payload})});
  // 正常帧兜底：确认 worker 已处理完前两帧
  rig.transport->InjectPacket(
      BytesView{MakeFrame(0, 0, 2, BytesView{payload})});
  ASSERT_TRUE(collector.WaitFor(1));

  const auto stats = rig.session.Statistics();
  EXPECT_EQ(stats.dto_received, 3U);
  EXPECT_EQ(stats.decode_errors, 1U);  // 仅畸形帧
  const auto frames = collector.Frames();
  for (const auto& f : frames) {
    EXPECT_EQ(f.daq_list, 0U);
  }
  rig.session.Stop();
}

TEST(MeasurementSessionPipeline, ConversionFailureDegradesSingleSampleOnly) {
  Rig rig;
  rig.db.failing.insert("Coolant");  // 仅 Coolant 换算失败
  rig.session.Add("EngineSpeed");
  rig.session.Add("Coolant");
  rig.session.Prepare();  // 同 list 同 odt：offset 0 与 4

  FrameCollector collector;
  rig.session.Start(MeasurementCallback(
      [&collector](const MeasurementFrame& f) { collector(f); }));

  Bytes payload{0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
  rig.transport->InjectPacket(
      BytesView{MakeFrame(0, 0, 7, BytesView{payload})});
  ASSERT_TRUE(collector.WaitFor(1));

  const auto frames = collector.Frames();
  ASSERT_EQ(frames.size(), 1U);
  ASSERT_EQ(frames[0].samples.size(), 2U);
  EXPECT_TRUE(frames[0].samples[0].valid);  // EngineSpeed 正常
  EXPECT_EQ(frames[0].samples[0].name, "EngineSpeed");
  EXPECT_EQ(frames[0].samples[0].raw,
            Bytes((payload.begin()), payload.begin() + 4));
  EXPECT_FALSE(frames[0].samples[1].valid);  // Coolant 仅自身失效
  EXPECT_EQ(frames[0].samples[1].name, "Coolant");
  EXPECT_EQ(frames[0].samples[1].raw, Bytes(payload.begin() + 4, payload.end()));
  EXPECT_EQ(rig.session.Statistics().decode_errors, 1U);
  rig.session.Stop();
}

TEST(MeasurementSessionPipeline, MultipleFramesAllDeliveredInOrder) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();
  FrameCollector collector;
  rig.session.Start(MeasurementCallback(
      [&collector](const MeasurementFrame& f) { collector(f); }));

  constexpr int kFrames = 50;
  for (int i = 0; i < kFrames; ++i) {
    Bytes payload{static_cast<std::uint8_t>(i & 0xFF), 0x00, 0x00, 0x00};
    rig.transport->InjectPacket(
        BytesView{MakeFrame(0, 0, static_cast<std::uint32_t>(i),
                            BytesView{payload})});
  }
  ASSERT_TRUE(collector.WaitFor(kFrames));
  const auto frames = collector.Frames();
  ASSERT_EQ(frames.size(), static_cast<std::size_t>(kFrames));
  for (int i = 0; i < kFrames; ++i) {
    EXPECT_EQ(frames[static_cast<std::size_t>(i)].timestamp,
              std::chrono::nanoseconds(i));
  }
  const auto stats = rig.session.Statistics();
  EXPECT_EQ(stats.dto_received, static_cast<std::uint64_t>(kFrames));
  EXPECT_EQ(stats.dto_dropped, 0U);
  rig.session.Stop();
}

TEST(MeasurementSessionPipeline, PrepareIssuesDynamicDaqTransaction) {
  Rig rig;
  rig.session.Add("EngineSpeed");
  rig.session.Prepare();
  // 动态整表：FREE→ALLOC_DAQ→ALLOC_ODT→ALLOC_ODT_ENTRY→SET_DAQ_PTR+WRITE_DAQ
  EXPECT_EQ(rig.slave.Count(CommandCode::FreeDaq), 1);
  EXPECT_EQ(rig.slave.Count(CommandCode::AllocDaq), 1);
  EXPECT_GE(rig.slave.Count(CommandCode::AllocOdt), 1);
  EXPECT_GE(rig.slave.Count(CommandCode::AllocOdtEntry), 1);
  EXPECT_GE(rig.slave.Count(CommandCode::WriteDaq), 1);
  EXPECT_EQ(rig.slave.Count(CommandCode::SetDaqListMode), 1);
  // 规划期取证：分辨率（时间戳 4 字节）被查询
  EXPECT_GE(rig.slave.Count(CommandCode::GetDaqResolutionInfo), 1);
}

TEST(MeasurementSessionPipeline, PrepareFailureWithUnknownSymbolThrowsBeforeDaq) {
  Rig rig;
  rig.session.Add("NotInDatabase");
  EXPECT_THROW(rig.session.Prepare(), XcpException);
  // 规划失败发生在任何 DAQ 下发之前（不产生半份配置）
  EXPECT_EQ(rig.slave.Count(CommandCode::AllocDaq), 0);
  EXPECT_EQ(rig.slave.Count(CommandCode::FreeDaq), 0);
}

}  // namespace
}  // namespace calmcar::xcp
