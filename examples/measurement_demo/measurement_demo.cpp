// measurement_demo.cpp — 测量子系统端到端示例（v0.9）：
// 「拉起 Slave → UPLOAD 拉回 A2L → 桥接解析 → 适配器 → MeasurementSession →
//  物理值帧打印」全链路，展示 libxcp 测量 SDK 的推荐用法。
//
// 流程（与 tests/xcplite_measurement_test.cpp L3 同构，此处以交付示例形态
// 重写；对应 code-plan/libxcp_测量子系统代码增长计划.md v0.9 交付物 3）：
//   1) 启动 xcp_test_slave 子进程（工作目录 = run-dir），就绪探测 = CONNECT；
//   2) 经 GET_ID(IDT_ASAM_UPLOAD)+UPLOAD 从 Slave 拉回运行时 A2L（协议面取证，
//      不读盘捷径），落盘 run-dir/uploaded.a2l（与 Slave 写的 XCP_104.aml 同
//      目录，保证 include 链解析）；
//   3) libxcp::a2lbridge 加载上传副本；A2lMeasurementDatabase 包装为核心
//      IMeasurementDatabase；事件通道（"testev"）从上传 A2L 的 EVENT 段取证；
//   4) MeasurementSession：先构造（作 IEventListener）→ Bind → Connect →
//      SetEnvelopeMode(RelativeByte,4)+SetTimestampFirstOdtOnly(true)+
//      SetTimestampUnit(1)（XCPlite 实然取证值，生产环境应改从
//      GET_DAQ_PROCESSOR_INFO / GET_DAQ_RESOLUTION_INFO / A2L EVENT 取证）→
//      Add → Prepare → Start（回调收 MeasurementFrame）；
//   5) 采集约 2 秒：打印首批帧的符号/物理值/时间戳，断言 g_basic_u32 的
//      A2L 定标值 == 0xDEADBEEF、解码零错误；Stop → Disconnect → 终止 Slave。
//
// 退出码：0 = 全部断言通过；1 = 断言失败；2 = 环境错误（Slave 缺失/端口占用/
// 连接失败）。
//
// 约束：不修改 thirdparty；不引用 tests/ 源码（Slave 经 CMake 注入的可执行
// 文件路径寻址）；C++20。

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <libxcp/a2l/a2l_bridge.hpp>
#include <libxcp/a2l/ia2l_database.hpp>
#include <libxcp/command_executor.hpp>
#include <libxcp/daq/dto_envelope_types.hpp>
#include <libxcp/measurement/measurement_database.hpp>
#include <libxcp/measurement/measurement_result.hpp>
#include <libxcp/measurement/measurement_sample.hpp>
#include <libxcp/measurement/measurement_session.hpp>
#include <libxcp/protocol_types.hpp>
#include <libxcp/udp_transport.hpp>
#include <libxcp/udp_transport_config.hpp>
#include <libxcp/xcp_error.hpp>
#include <libxcp/xcp_master.hpp>

#include <a2l/a2l_measurement_database.hpp>

#include "demo_process.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace calmcar::xcp;

namespace {

/// @brief Slave 项目名（MODULE 前缀，来自 xcp_test_slave 的 XcpInit 约定）。
constexpr std::string_view kSlaveProject = "xcp_test_slave";

/// @brief Slave 默认监听端口（xcp_test_slave 无参启动时的缺省值）。
constexpr std::uint16_t kSlavePort = 5556;

/// @brief 命令行配置（全部有默认值，零参数即可运行）。
struct Options {
    fs::path runDir = "measurement_demo_run";  ///< Slave 工作目录。
    fs::path slaveExe = XCP_TEST_SLAVE_EXECUTABLE;  ///< Slave 可执行（注入）。
    std::string host = "127.0.0.1";                  ///< XCP 地址。
    std::uint16_t port = kSlavePort;                 ///< XCP 端口。
    int sampleSeconds = 2;                           ///< 采集时长（秒）。
    std::chrono::milliseconds commandTimeout{2000};  ///< 单命令超时。
};

/// @brief 轻量断言收集器：失败不中断，最后统一以退出码 1 收尾。
class Checker {
public:
    /// @brief 记录一条断言结果。
    void Check(bool ok, std::string_view what) {
        ++total_;
        if (ok) {
            std::printf("[ OK ] %.*s\n", static_cast<int>(what.size()), what.data());
        } else {
            ++failed_;
            std::printf("[FAIL] %.*s\n", static_cast<int>(what.size()), what.data());
        }
    }

    /// @brief 打印汇总结论；有失败时返回 false。
    bool Report() const {
        std::printf("\n==== 断言汇总：%d 项，失败 %d 项 ====\n", total_, failed_);
        return failed_ == 0;
    }

private:
    int total_ = 0;
    int failed_ = 0;
};

/// @brief 打印 MeasurementValue（核心 variant）为可读文本。
void PrintValue(const MeasurementValue& value) {
    std::visit([](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            std::printf("\"%s\"", v.c_str());
        } else if constexpr (std::is_same_v<T, bool>) {
            std::printf("%s", v ? "true" : "false");
        } else if constexpr (std::is_same_v<T, double>) {
            std::printf("%.6g", v);
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            std::printf("%llu", static_cast<unsigned long long>(v));
        } else {
            std::printf("%lld", static_cast<long long>(v));
        }
    }, value);
}

/// @brief 线程安全帧收集器：worker 线程回调写入，主线程快照读取。
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

    /// @brief 轮询直到 pred 满足或超时。
    template <class Pred>
    bool WaitUntil(Pred pred, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred(Snapshot())) {
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return pred(Snapshot());
    }

private:
    mutable std::mutex m_mutex_;
    std::vector<MeasurementFrame> m_frames_;
};

/// @brief 建立指向 Slave 的 Master（UDP；listener 非拥有，可空）。
std::unique_ptr<XcpMaster> MakeMaster(const Options& opt, IEventListener* listener) {
    UdpTransportConfig cfg;
    cfg.remote_host = opt.host;
    cfg.remote_port = opt.port;
    cfg.local_host = "0.0.0.0";
    cfg.local_port = 0;
    cfg.receive_poll_interval_ms = 20;
    CommandTimeouts timeouts;
    timeouts.command_timeout = opt.commandTimeout;
    timeouts.synch_timeout = opt.commandTimeout;
    timeouts.max_retries = 1;
    return std::make_unique<XcpMaster>(
        std::make_unique<UdpTransport>(cfg), timeouts, listener);
}

/// @brief 等待 Slave 达到可 CONNECT 状态；超时返回 nullptr。
std::unique_ptr<XcpMaster> WaitConnected(const Options& opt, IEventListener* listener,
                                         std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        auto master = MakeMaster(opt, listener);
        try {
            master->Connect();
            return master;
        } catch (const std::exception&) {
            // Slave 尚未监听：短暂等待后重试（listener 非拥有，master 析构安全）
        }
        std::this_thread::sleep_for(200ms);
    }
    return nullptr;
}

/// @brief 在上传 A2L 文本中取证事件通道：匹配 `"testev" 0x%hex`（EVENT 段）。
std::optional<std::uint16_t> ResolveEventChannel(std::string_view a2l,
                                                 std::string_view short_name) {
    const std::string needle = std::string("\"") + std::string(short_name) + "\" 0x";
    const std::size_t pos = a2l.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t hex_begin = pos + needle.size();
    std::size_t hex_end = hex_begin;
    while (hex_end < a2l.size() &&
           (std::isxdigit(static_cast<unsigned char>(a2l[hex_end])) != 0)) {
        ++hex_end;
    }
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(a2l.data() + hex_begin,
                                           a2l.data() + hex_end, value, 16);
    if (ec != std::errc{} || ptr != a2l.data() + hex_end) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

/// @brief 全限定符号名（MODULE::symbol）。
std::string Qualified(std::string_view symbol) {
    return std::string(kSlaveProject) + "::" + std::string(symbol);
}

/// @brief 解析 --run-dir/--slave/--host/--port/--seconds/--help。
bool ParseArgs(int argc, char** argv, Options& opt) {
    auto needValue = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::printf("[usage] 参数 %s 缺少取值\n", argv[i]);
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::printf("用法：measurement_demo [--run-dir DIR] [--slave EXE] "
                        "[--host H] [--port N] [--seconds N]\n");
            return false;
        }
        if (arg == "--run-dir") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.runDir = v;
        } else if (arg == "--slave") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.slaveExe = v;
        } else if (arg == "--host") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.host = v;
        } else if (arg == "--port") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.port = static_cast<std::uint16_t>(std::strtoul(v, nullptr, 10));
        } else if (arg == "--seconds") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.sampleSeconds = std::max(1, std::atoi(v));
        } else {
            std::printf("[usage] 未知参数 %s（--help 查看用法）\n", argv[i]);
            return false;
        }
    }
    return true;
}

/**
 * @brief 会话停止守卫：析构时 Stop()（异常展开路径同样保证 worker join、
 *        DAQ 复位；声明顺序须在被守护对象之后、Master 之前——与 L3 测试的
 *        AutoStopSession 同一课）。
 */
class SessionGuard {
public:
    explicit SessionGuard(MeasurementSession& session) : m_session_(session) {}
    ~SessionGuard() {
        try {
            m_session_.Stop();
        } catch (...) {
        }
    }
    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;

private:
    MeasurementSession& m_session_;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        return 2;
    }
    Checker check;

    std::error_code ec;
    fs::create_directories(opt.runDir, ec);
    if (ec) {
        std::printf("[env] 无法创建 run-dir %s：%s\n", opt.runDir.string().c_str(),
                    ec.message().c_str());
        return 2;
    }
    if (!fs::exists(opt.slaveExe, ec) || ec) {
        std::printf("[env] Slave 可执行不存在：%s\n", opt.slaveExe.string().c_str());
        return 2;
    }

    // —— 1) 拉起 Slave（工作目录 = run-dir，A2L/AML 落盘于此）——
    xcp_example::DemoProcess slave;
    std::string startErr;
    if (!slave.Start(opt.slaveExe.string(), opt.runDir, &startErr)) {
        std::printf("[env] Slave 启动失败：%s\n", startErr.c_str());
        return 2;
    }
    std::printf("[info] Slave 已启动（pid=%u），目标 %s:%u\n", slave.ProcessId(),
                opt.host.c_str(), opt.port);

    int exitCode = 1;
    try {
        // —— 2) UPLOAD 拉回运行时 A2L（协议面取证，不读盘捷径）——
        auto probe = WaitConnected(opt, nullptr, 15s);
        if (!probe) {
            std::printf("[env] 15s 内无法连接 Slave（端口占用或启动失败）\n");
            slave.Stop();
            return 2;
        }
        const Bytes uploaded = probe->FetchA2lViaUpload();
        probe->Disconnect();
        probe.reset();
        std::printf("[info] UPLOAD 拉回 A2L：%zu 字节\n", uploaded.size());
        const fs::path uploadedPath = opt.runDir / "uploaded.a2l";
        {
            std::ofstream out(uploadedPath, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(uploaded.data()),
                      static_cast<std::streamsize>(uploaded.size()));
        }
        check.Check(uploaded.size() > 100U, "UPLOAD A2L 长度合理");

        // —— 3) 桥接解析 + 适配器包装（唯一桥接接触点在 adapter/）——
        auto loaded = a2l::A2lBridge::Load(uploadedPath.string());
        if (!loaded.HasValue()) {
            std::printf("[env] A2L 加载失败：%s\n", loaded.ErrorInfo().message.c_str());
            slave.Stop();
            return 2;
        }
        const a2l::IA2lDatabase* db = loaded.Value()->Database();
        A2lMeasurementDatabase adapter(*db);

        // 事件通道从上传 A2L 的 EVENT 段取证（适配器恒回 0：静态面无事件绑定）
        const std::string a2lText(reinterpret_cast<const char*>(uploaded.data()),
                                  uploaded.size());
        const auto event = ResolveEventChannel(a2lText, "testev");
        if (!event.has_value()) {
            std::printf("[env] 上传 A2L 中未取证到 testev 事件通道\n");
            slave.Stop();
            return 2;
        }
        // 注意：XCPlite 运行时 A2L 的 testev 通道实然为 0（DaqCreateEvent 首个
        // 事件即通道 0）；0 在规划器中按"无事件组"处理，同样能建表出流。
        // 通道号合法与否不以非零为判据——取证成功即可。
        std::printf("[info] testev 事件通道 = %u（运行时 A2L 实然取证值）\n",
                    static_cast<unsigned>(*event));

        // 装饰器：Find 结果补充运行时事件通道（示例内联最小实现）
        class EventBound final : public IMeasurementDatabase {
        public:
            EventBound(IMeasurementDatabase& inner, std::uint16_t channel)
                : m_inner_(inner), m_channel_(channel) {}
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
            IMeasurementDatabase& m_inner_;
            std::uint16_t m_channel_;
        };
        EventBound measurement_db(adapter, *event);

        // —— 4) 会话（先构造作监听器 → Bind → Connect → 取证注入）——
        MeasurementSession session(measurement_db);
        auto master = MakeMaster(opt, &session);
        session.Bind(*master);
        // CONNECT 已在探测阶段验证可用；正式会话重建并重连
        master->Connect();
        SessionGuard guard(session);  // 声明于 master 之后：展开时先 Stop 后毁 master

        // XCPlite 实然取证值（生产环境改从 GET_DAQ_PROCESSOR_INFO /
        // GET_DAQ_RESOLUTION_INFO / A2L EVENT 取证注入）：
        //   RelativeByte [ODTrel][0xAA][DAQ16]，头长 4；时间戳仅随事件首
        //   ODT 帧；1 ns/tick。
        session.SetEnvelopeMode(IdentificationFieldType::RelativeByte, 4U);
        session.SetTimestampFirstOdtOnly(true);
        session.SetTimestampUnit(1);

        const std::string u32Name = Qualified("g_basic_u32");
        const std::string f32Name = Qualified("g_basic_f32");
        session.Add(u32Name);
        session.Add(f32Name);
        try {
            session.Prepare();
            check.Check(true, "Prepare（规划+ConfigureDaqListsDynamic 落表）");
        } catch (const std::exception& ex) {
            std::printf("[FAIL] Prepare 异常：%s\n", ex.what());
            check.Check(false, "Prepare");
            slave.Stop();
            return 1;
        }

        FrameSink sink;
        const auto startAt = std::chrono::steady_clock::now();
        session.Start(MeasurementCallback(
            [&sink](const MeasurementFrame& f) { sink(f); }));

        // —— 5) 采集 sampleSeconds 秒并断言 ——
        const std::uint32_t kExpectU32 = 0xDEADBEEFU;
        const bool seen = sink.WaitUntil(
            [&](const std::vector<MeasurementFrame>& frames) {
                for (const auto& f : frames) {
                    for (const auto& s : f.samples) {
                        if (s.name != u32Name || !s.valid) {
                            continue;
                        }
                        if (const auto* p = std::get_if<std::uint64_t>(&s.value)) {
                            if (*p == kExpectU32) return true;
                        }
                        if (const auto* p = std::get_if<std::int64_t>(&s.value)) {
                            if (static_cast<std::uint64_t>(*p) == kExpectU32) return true;
                        }
                    }
                }
                return false;
            },
            std::chrono::seconds(opt.sampleSeconds) + 3s);
        check.Check(seen, "收到 g_basic_u32 物理值 == 0xDEADBEEF（A2L 定标）");

        // 采样窗口打满：--seconds 的语义是"至少采集这么长时间"（长稳取证口径），
        // 而非见到首帧即停——否则统计数字只反映毫秒级瞬时，不构成稳定性证据。
        const auto elapsed = std::chrono::steady_clock::now() - startAt;
        const auto wanted = std::chrono::seconds(opt.sampleSeconds);
        if (elapsed < wanted) {
            std::this_thread::sleep_for(wanted - elapsed);
        }

        const auto frames = sink.Snapshot();
        check.Check(!frames.empty(), "至少收到一帧 MeasurementFrame");
        bool hasTimestamped = false;
        for (const auto& f : frames) {
            if (f.odt == 0U && f.timestamp_valid) {
                hasTimestamped = true;
                break;
            }
        }
        check.Check(hasTimestamped, "首 ODT 帧带出有效时间戳（v0.6 链路）");

        std::printf("\n==== 首批帧样例（前 3 帧） ====\n");
        for (std::size_t i = 0; i < std::min<std::size_t>(3, frames.size()); ++i) {
            const auto& f = frames[i];
            std::printf("frame[%zu] list=%u odt=%u ts_valid=%d ts_raw=%llu\n", i,
                        f.daq_list, f.odt, f.timestamp_valid ? 1 : 0,
                        static_cast<unsigned long long>(f.timestamp_raw));
            for (const auto& s : f.samples) {
                std::printf("  %-40s ", s.name.c_str());
                if (s.valid) {
                    PrintValue(s.value);
                } else {
                    std::printf("<decode failed>");
                }
                std::printf("\n");
            }
        }

        const auto stats = session.Statistics();
        std::printf("\n[info] 统计：received=%llu dropped=%llu decode_err=%llu wraps=%llu\n",
                    static_cast<unsigned long long>(stats.dto_received),
                    static_cast<unsigned long long>(stats.dto_dropped),
                    static_cast<unsigned long long>(stats.decode_errors),
                    static_cast<unsigned long long>(stats.timestamp_wraps));
        check.Check(stats.decode_errors == 0U, "全链路零解码错误");
        check.Check(stats.dto_received > 0U, "有 DTO 入队");
        // 长稳口径（--seconds ≥ 30 视为长采窗口）：worker 跟得上事件流 ⇒ 丢包
        // 必须为零（Slave 每 1ms 触发 testev、每帧 2 个 ODT，30s 约 6 万 DTO，
        // 任何持续性掉队都会体现在 dropped 上）。短冒烟不做此断言：
        // 启动瞬态下首拍丢包属正常。
        if (opt.sampleSeconds >= 30) {
            check.Check(stats.dto_dropped == 0U, "长采窗口零丢包（worker 实时性）");
            // 时间戳回绕口径：Slave 的 32 位时间戳是相对 tick 计数（实测
            // 2^32 tick ≈ 4.3s 一圈），60s 长采窗口内回绕十余次属正常算术
            // 行为——converter 据此外推 +k·2^32 并累计 WrapCount（v0.6 语义）。
            // 判据：回绕确实发生（证明外推路径在真实流上被走过且未报错）、
            // 且 decode_errors 仍为零（回绕处理无异常）。
            check.Check(stats.timestamp_wraps > 0U,
                        "32 位时间戳长采窗口按实然回绕（外推路径生效）");
        }

        session.Stop();
        master->Disconnect();
        check.Check(!session.Running(), "Stop 后会话非运行态");
        slave.Stop();

        exitCode = check.Report() ? 0 : 1;
    } catch (const std::exception& ex) {
        std::printf("[FAIL] 未预期异常：%s\n", ex.what());
        check.Report();
        slave.Stop();
        exitCode = 1;
    }
    return exitCode;
}
