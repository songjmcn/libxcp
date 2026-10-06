// measurement_demo.cpp — 测量子系统端到端示例（v0.9；L4 冒烟参数化扩展）：
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
//      IMeasurementDatabase；事件通道按符号从上传 A2L 的 MEASUREMENT 段取证
//      （多事件 Slave 如 cpp_demo 一机多通道同样适用），取不到时回退
//      --event 命名事件（默认 "testev"）的 EVENT 段通道；
//   4) MeasurementSession：先构造（作 IEventListener）→ Bind → Connect →
//      SetEnvelopeMode(RelativeByte,4)+SetTimestampFirstOdtOnly(true)+
//      SetTimestampUnit(1)（XCPlite 实然取证值，生产环境应改从
//      GET_DAQ_PROCESSOR_INFO / GET_DAQ_RESOLUTION_INFO / A2L EVENT 取证）→
//      Add → Prepare → Start（回调收 MeasurementFrame）；
//   5) 采集 --seconds 秒（默认 2）：mode=fixed（默认）断言首符号定标值 ==
//      --expect（默认 0xDEADBEEF）；mode=dynamic 面向物理值真实运动的 Slave
//      （如 cpp_demo，L4 冒烟口径）断言逐符号覆盖、变化性（窗口内 ≥2 个
//      不同原始值）与 --range 值域；两种模式都断言解码零错误；
//      Stop → Disconnect → 终止 Slave。
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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
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
#include <a2l/measurement_runtime_profile.hpp>

#include "demo_process.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace calmcar::xcp;

namespace {

/// @brief 命令行配置（全部有默认值，零参数即 v0.9 xcp_test_slave 旧行为）。
struct Options {
    fs::path runDir = "measurement_demo_run";       ///< Slave 工作目录。
    fs::path slaveExe = XCP_TEST_SLAVE_EXECUTABLE;  ///< Slave 可执行（注入）。
    std::string host = "127.0.0.1";                 ///< XCP 地址。
    std::uint16_t port = 5556;  ///< XCP 端口（slave 缺省）。
    int sampleSeconds = 2;      ///< 采集时长（秒）。
    std::chrono::milliseconds commandTimeout{2000};  ///< 单命令超时。
    std::string project =
        "xcp_test_slave";  ///< MODULE 限定名前缀（cpp_demo 即 "cpp_demo"）。
    std::string symbolsSpec =
        "g_basic_u32,g_basic_f32";         ///< 逗号分隔符号短名。
    std::string event = "testev";          ///< 回退命名事件（EVENT 段取证）。
    std::string a2lName = "uploaded.a2l";  ///< 上传 A2L 落盘文件名。
    bool dynamicMode = false;              ///< --mode dynamic（L4 冒烟口径）。
    bool calibrationProbe = false;         ///< 本地 cpp_demo 在线标定→DAQ 闭环。
    std::uint64_t expect = 0xDEADBEEFU;    ///< fixed 模式首符号期望定标值。
    /// short_name → [min, max]（物理值域，dynamic 模式可选断言）。
    std::map<std::string, std::pair<double, double>> ranges;

    /// @brief 符号短名列表（逗号分隔，去空白；保序）。
    std::vector<std::string> Symbols() const {
        std::vector<std::string> out;
        std::stringstream ss(symbolsSpec);
        std::string item;
        auto trim = [](std::string& s) {
            const auto b = s.find_first_not_of(" \t");
            if (b == std::string::npos) {
                s.clear();
                return;
            }
            const auto e = s.find_last_not_of(" \t");
            s = s.substr(b, e - b + 1);
        };
        while (std::getline(ss, item, ',')) {
            trim(item);
            if (!item.empty()) {
                out.push_back(item);
            }
        }
        return out;
    }
};

/// @brief 轻量断言收集器：失败不中断，最后统一以退出码 1 收尾。
class Checker {
public:
    /// @brief 记录一条断言结果。
    void Check(bool ok, std::string_view what) {
        ++total_;
        if (ok) {
            std::printf("[ OK ] %.*s\n", static_cast<int>(what.size()),
                        what.data());
        } else {
            ++failed_;
            std::printf("[FAIL] %.*s\n", static_cast<int>(what.size()),
                        what.data());
        }
    }

    /// @brief 打印汇总结论；有失败时返回 false。
    bool Report() const {
        std::printf("\n==== 断言汇总：%d 项，失败 %d 项 ====\n", total_,
                    failed_);
        return failed_ == 0;
    }

private:
    int total_ = 0;
    int failed_ = 0;
};

/// @brief 打印 MeasurementValue（核心 variant）为可读文本。
void PrintValue(const MeasurementValue& value) {
    std::visit(
        [](const auto& v) {
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
        },
        value);
}

/// @brief 物理值取 double 视图（bool/string 视为不可比，返回 false）。
bool ToDoubleView(const MeasurementValue& v, double& out) {
    if (const auto* p = std::get_if<double>(&v)) {
        out = *p;
        return true;
    }
    if (const auto* p = std::get_if<std::int64_t>(&v)) {
        out = static_cast<double>(*p);
        return true;
    }
    if (const auto* p = std::get_if<std::uint64_t>(&v)) {
        out = static_cast<double>(*p);
        return true;
    }
    return false;
}

/// @brief 十进制/十六进制（0x 前缀）无符号整数解析。
bool ParseU64(std::string_view text, std::uint64_t& out) {
    try {
        std::size_t used = 0;
        out = std::stoull(std::string(text), &used, 0);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

/// @brief 解析 --range NAME=MIN:MAX（MIN/MAX 支持负号与小数）。
bool ParseRangeSpec(std::string_view spec, std::string& name,
                    std::pair<double, double>& out) {
    const auto eq = spec.find('=');
    if (eq == std::string_view::npos) {
        return false;
    }
    const auto rest = spec.substr(eq + 1);
    const auto colon = rest.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    name = std::string(spec.substr(0, eq));
    const std::string loStr(rest.substr(0, colon));
    const std::string hiStr(rest.substr(colon + 1));
    try {
        out = {std::stod(loStr), std::stod(hiStr)};
    } catch (...) {
        return false;
    }
    return !name.empty() && out.first <= out.second;
}

/**
 * @brief 线程安全帧收集器 + 有界聚合器（worker 线程回调写入，主线程读取）。
 * @details 保留前 kKeepFrames 帧供样例打印/谓词轮询；无论窗口多长，逐符号
 *          聚合（覆盖数、有效数、≤kMaxKeys 个不同原始值、物理值 min/max）
 *          滚动更新——10 分钟级 L4 冒烟若全帧驻留会到数百 MB，聚合器把
 *          内存钉在常数级，同时变化性/值域断言的信息量不减。
 */
class FrameSink {
public:
    static constexpr std::size_t kKeepFrames = 2000;
    static constexpr std::size_t kMaxKeys = 8;
    static constexpr std::size_t kMaxRawTrace = 100000;

    /// @brief 单符号滚动聚合（锁内更新）。
    struct SymbolAgg {
        std::uint64_t seen{0};       ///< 样本出现次数（含无效）
        std::uint64_t valid{0};      ///< 有效样本次数
        std::set<std::string> keys;  ///< 不同原始字节（截断到 kMaxKeys）
        std::vector<std::uint64_t> rawTrace;  ///< 有界的数值原始样本序列。
        bool hasRange{false};                 ///< 已有可比物理值
        double minV{0.0};
        double maxV{0.0};
    };

    void operator()(const MeasurementFrame& frame) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_frames_.size() < kKeepFrames) {
            m_frames_.push_back(frame);
        }
        m_lists_.insert(frame.daq_list);
        for (const auto& s : frame.samples) {
            SymbolAgg& agg = m_agg_[s.name];
            ++agg.seen;
            if (!s.valid) {
                continue;
            }
            ++agg.valid;
            if (agg.keys.size() < kMaxKeys) {
                agg.keys.emplace(reinterpret_cast<const char*>(s.raw.data()),
                                 s.raw.size());
            }
            if (!s.raw.empty() && s.raw.size() <= sizeof(std::uint64_t) &&
                agg.rawTrace.size() < kMaxRawTrace) {
                std::uint64_t raw = 0;
                for (std::size_t i = 0; i < s.raw.size(); ++i) {
                    raw |= static_cast<std::uint64_t>(
                               static_cast<unsigned char>(s.raw[i]))
                           << (8U * i);
                }
                agg.rawTrace.push_back(raw);
            }
            double v = 0.0;
            if (ToDoubleView(s.value, v)) {
                if (!agg.hasRange) {
                    agg.hasRange = true;
                    agg.minV = agg.maxV = v;
                } else {
                    agg.minV = std::min(agg.minV, v);
                    agg.maxV = std::max(agg.maxV, v);
                }
            }
        }
    }

    std::vector<MeasurementFrame> Snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_frames_;
    }

    /// @brief 轮询直到 pred 满足或超时（pred 自取聚合状态，线程安全）。
    template <class Pred>
    bool WaitUntil(Pred pred, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return pred();
    }

    /// @brief 收到过帧的 DAQ List 号集合（= 独立时间戳流数，回绕自洽口径）。
    std::set<std::uint16_t> Lists() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_lists_;
    }

    bool HasAgg(std::string_view name) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_agg_.find(std::string(name)) != m_agg_.end();
    }

    std::uint64_t ValidCount(std::string_view name) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        return it == m_agg_.end() ? 0U : it->second.valid;
    }

    /// @brief 窗口内不同原始值个数（≥kMaxKeys 时按 kMaxKeys 计，判"≥2"足够）。
    std::size_t DistinctCount(std::string_view name) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        return it == m_agg_.end() ? 0U : it->second.keys.size();
    }

    std::optional<std::pair<double, double>> MinMax(
        std::string_view name) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        if (it == m_agg_.end() || !it->second.hasRange) {
            return std::nullopt;
        }
        return std::make_pair(it->second.minV, it->second.maxV);
    }

    /// @brief 窗口内前 ≤kMaxKeys 个不同原始字节的副本（线长/偏移取证用）。
    std::vector<std::string> KeysFor(std::string_view name) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        if (it == m_agg_.end()) {
            return {};
        }
        return {it->second.keys.begin(), it->second.keys.end()};
    }

    /**
     * @brief 是否出现过指定无符号定标值（按原始字节小端解读；本 Slave 族
     *        byte_order=Intel，物理定标为恒等映射时 raw≡value，与 v0.9 的
     *        variant 比较口径等价）。
     */
    bool SawU64(std::string_view name, std::uint64_t value) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        if (it == m_agg_.end()) {
            return false;
        }
        for (const auto& key : it->second.keys) {
            if (key.size() != 4U && key.size() != 8U) {
                continue;
            }
            std::uint64_t v = 0;
            for (std::size_t i = 0; i < key.size(); ++i) {
                v |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(key[i]))
                     << (8U * i);
            }
            if (v == value) {
                return true;
            }
        }
        return false;
    }

    /// @brief 检查同一帧样本的原始字节与 A2L 物理值满足线性公式。
    bool SawLinearPair(std::string_view name, double factor,
                       double offset) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        for (const MeasurementFrame& frame : m_frames_) {
            for (const MeasurementSample& sample : frame.samples) {
                if (sample.name != name || !sample.valid ||
                    sample.raw.size() != 1U) {
                    continue;
                }
                double physical = 0.0;
                if (!ToDoubleView(sample.value, physical)) {
                    continue;
                }
                const double raw = static_cast<unsigned char>(sample.raw[0]);
                const double expected = factor * raw + offset;
                if (std::abs(physical - expected) <= 1e-9) {
                    return true;
                }
            }
        }
        return false;
    }

    void ResetRawTrace(std::string_view name) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        if (it != m_agg_.end()) {
            it->second.rawTrace.clear();
        }
    }

    bool SawRawAbove(std::string_view name, std::uint64_t threshold) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        return it != m_agg_.end() &&
               std::any_of(
                   it->second.rawTrace.begin(), it->second.rawTrace.end(),
                   [threshold](std::uint64_t raw) { return raw > threshold; });
    }

    /// @brief 找到至少 N 次完整的 0..ceiling 周期，忽略写入前排队的旧 DTO。
    bool SawRepeatedWrapsWithin(std::string_view name, std::uint64_t ceiling,
                                std::size_t required_wraps) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        if (it == m_agg_.end()) {
            return false;
        }
        const auto& trace = it->second.rawTrace;
        for (std::size_t start = 0; start < trace.size(); ++start) {
            if (trace[start] != 0U) {
                continue;
            }
            std::uint64_t previous = 0U;
            std::size_t wraps = 0U;
            for (std::size_t i = start + 1U; i < trace.size(); ++i) {
                const std::uint64_t raw = trace[i];
                if (raw > ceiling) {
                    break;  // 此零点之后仍是旧队列，尝试更晚的零点。
                }
                if (raw < previous) {
                    ++wraps;
                }
                previous = raw;
                if (wraps >= required_wraps) {
                    return true;
                }
            }
        }
        return false;
    }

    /// @brief 取证转储：逐符号打印窗口内实际收到的原始字节序列（hex）与
    ///        物理值观测范围。用于把"线宽/定标口径"从推断变成证据——cpp_demo
    ///        快验中 temperature 值域越界即凭此定性（缺陷①）。
    void DumpRawKeys(std::string_view name) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        const auto it = m_agg_.find(std::string(name));
        if (it == m_agg_.end()) {
            std::printf("[raw ] %-32s (no agg)\n", std::string(name).c_str());
            return;
        }
        const SymbolAgg& agg = it->second;
        char buf[256];
        int used = std::snprintf(buf, sizeof(buf),
                                 "[raw ] %-32s seen=%llu valid=%llu keys(",
                                 std::string(name).c_str(),
                                 static_cast<unsigned long long>(agg.seen),
                                 static_cast<unsigned long long>(agg.valid));
        if (used < 0 || used >= static_cast<int>(sizeof(buf))) {
            used = 0;
        }
        for (const auto& key : agg.keys) {
            const int n = static_cast<int>(sizeof(buf)) - used - 4;
            if (n <= 0) {
                break;
            }
            std::string hex;
            for (unsigned char c : key) {
                char h[4];
                std::snprintf(h, sizeof(h), "%02X", c);
                hex += h;
            }
            const int w = std::snprintf(
                buf + used, static_cast<std::size_t>(n), "%s%s",
                agg.keys.size() > 1 && used > 0 ? "," : "", hex.c_str());
            // 上面首分隔符判断不可靠（used 恒 >0 于第二次起），补正：
            if (used > 0 && w > 0 && buf[used - 1] != ',') {
                // 在写入前已带逗号则跳过
            }
            if (w < 0) {
                break;
            }
            used += w;
        }
        std::snprintf(buf + (used < static_cast<int>(sizeof(buf)) - 2
                                 ? used
                                 : static_cast<int>(sizeof(buf)) - 2),
                      sizeof(buf) - static_cast<std::size_t>(
                                        used < static_cast<int>(sizeof(buf)) - 2
                                            ? used
                                            : sizeof(buf) - 2),
                      ") phys[%g,%g]", agg.minV, agg.maxV);
        std::printf("%s\n", buf);
    }

private:
    mutable std::mutex m_mutex_;
    std::vector<MeasurementFrame> m_frames_;
    std::set<std::uint16_t> m_lists_;
    std::map<std::string, SymbolAgg> m_agg_;
};

/// @brief 建立指向 Slave 的 Master（UDP；listener 非拥有，可空）。
std::unique_ptr<XcpMaster> MakeMaster(const Options& opt,
                                      IEventListener* listener) {
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
    return std::make_unique<XcpMaster>(std::make_unique<UdpTransport>(cfg),
                                       timeouts, listener);
}

/// @brief 等待 Slave 达到可 CONNECT 状态；超时返回 nullptr。
std::unique_ptr<XcpMaster> WaitConnected(const Options& opt,
                                         IEventListener* listener,
                                         std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        auto master = MakeMaster(opt, listener);
        try {
            master->Connect();
            return master;
        } catch (const std::exception&) {
            // Slave 尚未监听：短暂等待后重试（listener 非拥有，master
            // 析构安全）
        }
        std::this_thread::sleep_for(200ms);
    }
    return nullptr;
}

bool ParseUnsignedToken(std::string_view token, std::uint64_t& value) {
    int base = 10;
    if (token.starts_with("0x") || token.starts_with("0X")) {
        token.remove_prefix(2);
        base = 16;
    }
    if (token.empty()) {
        return false;
    }
    const auto [ptr, ec] =
        std::from_chars(token.data(), token.data() + token.size(), value, base);
    return ec == std::errc{} && ptr == token.data() + token.size();
}

/// @brief 从上传 A2L 的 EVENT 声明提取名字、通道、周期和单位；拒绝坏行。
std::optional<std::vector<A2lEventMetadata>> ResolveA2lEvents(
    std::string_view a2l) {
    std::vector<A2lEventMetadata> events;
    std::istringstream input{std::string(a2l)};
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string begin;
        std::string kind;
        if (!(row >> begin >> kind) || begin != "/begin" || kind != "EVENT") {
            continue;
        }
        std::string long_name;
        std::string short_name;
        std::string channel_token;
        std::string type;
        std::string max_daq;
        std::string cycle_token;
        std::string unit_token;
        std::string priority;
        if (!(row >> std::quoted(long_name) >> std::quoted(short_name) >>
              channel_token >> type >> max_daq >> cycle_token >> unit_token >>
              priority)) {
            return std::nullopt;
        }
        std::uint64_t channel = 0;
        std::uint64_t cycle = 0;
        std::uint64_t unit = 0;
        if (!ParseUnsignedToken(channel_token, channel) ||
            !ParseUnsignedToken(cycle_token, cycle) ||
            !ParseUnsignedToken(unit_token, unit) ||
            channel > std::numeric_limits<std::uint16_t>::max() ||
            cycle > std::numeric_limits<std::uint8_t>::max() ||
            unit > std::numeric_limits<std::uint8_t>::max()) {
            return std::nullopt;
        }
        events.push_back(A2lEventMetadata{
            std::move(long_name), std::move(short_name),
            static_cast<std::uint16_t>(channel),
            static_cast<std::uint8_t>(cycle), static_cast<std::uint8_t>(unit)});
    }
    return events;
}

/**
 * @brief 从 XCPlite MEASUREMENT 行收集所有 EVENT 候选；不能解析时返回 nullopt。
 * 多个不同通道会由 EventBoundMeasurementDatabase 判为歧义，而不静默取首项。
 */
std::optional<std::vector<std::uint64_t>> ResolveSymbolEventChannels(
    std::string_view a2l, std::string_view short_name) {
    const std::string needle =
        "/begin MEASUREMENT " + std::string(short_name) + " ";
    std::vector<std::uint64_t> channels;
    std::size_t pos = 0;
    while ((pos = a2l.find(needle, pos)) != std::string::npos) {
        const std::size_t line_end = a2l.find('\n', pos);
        const std::string_view line = a2l.substr(
            pos, line_end == std::string_view::npos ? std::string_view::npos
                                                    : line_end - pos);
        std::size_t event_pos = 0;
        while ((event_pos = line.find("EVENT ", event_pos)) !=
               std::string_view::npos) {
            const bool standalone =
                event_pos == 0U || std::isspace(static_cast<unsigned char>(
                                       line[event_pos - 1U])) != 0;
            if (!standalone) {
                event_pos += 6U;
                continue;
            }
            const std::size_t token_begin = event_pos + 6U;
            std::size_t token_end = token_begin;
            while (token_end < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[token_end])) ==
                       0) {
                ++token_end;
            }
            std::uint64_t channel = 0;
            if (!ParseUnsignedToken(
                    line.substr(token_begin, token_end - token_begin),
                    channel)) {
                return std::nullopt;
            }
            channels.push_back(channel);
            break;
        }
        pos += needle.size();
    }
    return channels;
}

/// @brief 解析 --run-dir/--slave/--host/--port/--seconds/--project/--symbols/
///        --event/--a2l-name/--mode/--expect/--range/--help。
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
            std::printf(
                "用法：measurement_demo [--run-dir DIR] [--slave EXE]\n"
                "          [--host H] [--port N] [--seconds N]\n"
                "          [--project NAME] [--symbols a,b,...] [--event "
                "NAME]\n"
                "          [--a2l-name FILE] [--mode fixed|dynamic]\n"
                "          [--expect 0xHEX] [--range NAME=min:max]...\n"
                "          [--calibration-probe]（仅本地 cpp_demo）\n"
                "零参数 = v0.9 xcp_test_slave 旧行为（fixed/2s/0xDEADBEEF）。\n"
                "cpp_demo L4 冒烟示例：\n"
                "  measurement_demo --slave <cpp_demo.exe> --port 5555 \\\n"
                "      --project cpp_demo --event mainloop --mode dynamic \\\n"
                "      --symbols speed,sum,counter,temperature,SigGen1.value_ "
                "\\\n"
                "      --range speed=0:250 --range counter=0:65535 --seconds "
                "600\n");
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
        } else if (arg == "--project") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.project = v;
        } else if (arg == "--symbols") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.symbolsSpec = v;
        } else if (arg == "--event") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.event = v;
        } else if (arg == "--a2l-name") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.a2lName = v;
        } else if (arg == "--mode") {
            const char* v = needValue(i);
            if (!v) return false;
            const std::string_view mode = v;
            if (mode == "fixed") {
                opt.dynamicMode = false;
            } else if (mode == "dynamic") {
                opt.dynamicMode = true;
            } else {
                std::printf("[usage] --mode 只支持 fixed|dynamic（得到 %s）\n",
                            v);
                return false;
            }
        } else if (arg == "--expect") {
            const char* v = needValue(i);
            if (!v) return false;
            if (!ParseU64(v, opt.expect)) {
                std::printf("[usage] --expect 非法数值：%s\n", v);
                return false;
            }
        } else if (arg == "--range") {
            const char* v = needValue(i);
            if (!v) return false;
            std::string name;
            std::pair<double, double> range;
            if (!ParseRangeSpec(v, name, range)) {
                std::printf("[usage] --range 需为 NAME=min:max：%s\n", v);
                return false;
            }
            opt.ranges[name] = range;
        } else if (arg == "--calibration-probe") {
            opt.calibrationProbe = true;
        } else {
            std::printf("[usage] 未知参数 %s（--help 查看用法）\n", argv[i]);
            return false;
        }
    }
    if (opt.calibrationProbe) {
        const auto symbols = opt.Symbols();
        if (opt.project != "cpp_demo" || !opt.dynamicMode ||
            std::find(symbols.begin(), symbols.end(), "counter") ==
                symbols.end()) {
            std::printf(
                "[usage] --calibration-probe 仅支持 cpp_demo dynamic 且包含 "
                "counter\n");
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

std::uint64_t DecodeLittleEndian(const Bytes& bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size() && i < 8U; ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
    }
    return value;
}

Bytes EncodeU16LittleEndian(std::uint16_t value) {
    return {static_cast<std::uint8_t>(value & 0xFFU),
            static_cast<std::uint8_t>((value >> 8U) & 0xFFU)};
}

/// @brief cpp_demo 本地 CAL 写入→算法行为→恢复闭环（只操作本次 A2L 确认的
/// U16）。
bool RunCounterCalibrationProbe(XcpMaster& master,
                                A2lMeasurementDatabase& database,
                                FrameSink& sink, Checker& check,
                                std::string_view project_prefix) {
    constexpr std::uint16_t kOriginal = 1000U;
    constexpr std::uint16_t kReduced = 20U;
    const std::string counter_name = std::string(project_prefix) + "counter";
    const std::string calibration_name =
        std::string(project_prefix) + "kParameters.counter_max";

    const auto calibration = database.Find(calibration_name);
    if (!calibration.HasValue()) {
        std::printf("[cal] A2L 未解析成员叶 %s：%s\n", calibration_name.c_str(),
                    calibration.ErrorInfo().message.c_str());
        check.Check(false, "A2L 找到 counter_max 成员叶");
        return false;
    }
    const MeasurementSymbolInfo& symbol = calibration.Value();
    const bool layout_ok =
        symbol.element_size_bytes == 2U && symbol.element_count == 1U;
    check.Check(layout_ok, "A2L 核实 counter_max 为标量 U16（2 字节）");
    if (!layout_ok) {
        return false;
    }

    const Address address = symbol.address;
    const AddressExtension extension = symbol.extension;
    Bytes original_bytes;
    try {
        original_bytes = master.ReadMemoryBytes(address, extension, 2U);
    } catch (const std::exception& ex) {
        std::printf("[cal] 读取 counter_max 原值失败：%s\n", ex.what());
        check.Check(false, "读取 counter_max 原值");
        return false;
    }
    if (original_bytes.size() != 2U) {
        check.Check(false, "counter_max 原值读取长度为 2");
        return false;
    }
    const std::uint64_t original_value = DecodeLittleEndian(original_bytes);
    check.Check(original_value == kOriginal,
                "独立 run-dir 初始 counter_max == 1000");
    if (original_value != kOriginal) {
        std::printf(
            "[cal] 初始值为 %llu，不符合干净 cpp_demo 默认参数；不写入\n",
            static_cast<unsigned long long>(original_value));
        return false;
    }

    sink.ResetRawTrace(counter_name);
    const bool baseline_above_limit = sink.WaitUntil(
        [&] { return sink.SawRawAbove(counter_name, kReduced); }, 3s);
    check.Check(baseline_above_limit,
                "CAL 基线：counter 在默认上限下观测到 >20");
    if (!baseline_above_limit) {
        return false;
    }

    const Bytes reduced_bytes = EncodeU16LittleEndian(kReduced);
    bool write_returned = false;
    try {
        master.WriteMemoryBytes(address, extension, reduced_bytes);
        write_returned = true;
    } catch (const std::exception& ex) {
        // 写超时可能已执行；不重发。先通过独立 UPLOAD 查询当前值。
        std::printf("[cal] threshold 写命令异常（不重放，先读回）：%s\n",
                    ex.what());
    }

    Bytes current_bytes;
    try {
        current_bytes = master.ReadMemoryBytes(address, extension, 2U);
    } catch (const std::exception& ex) {
        std::printf("[cal] threshold 写后无法查询状态：%s\n", ex.what());
        check.Check(false, "threshold 写后可查询状态");
        return false;
    }
    if (current_bytes.size() != 2U) {
        std::printf("[cal] threshold 写后读回长度异常；状态不确定，终止\n");
        check.Check(false, "threshold 写后状态长度为 2");
        return false;
    }
    const bool reduced_applied = current_bytes == reduced_bytes;
    check.Check(reduced_applied,
                write_returned
                    ? "写入 counter_max=20 且 UPLOAD 读回一致"
                    : "写响应异常后 UPLOAD 证明 counter_max=20 已执行");

    bool behavior_ok = false;
    if (reduced_applied) {
        sink.ResetRawTrace(counter_name);
        behavior_ok = sink.WaitUntil(
            [&] {
                return sink.SawRepeatedWrapsWithin(counter_name, kReduced, 2U);
            },
            3s);
        check.Check(behavior_ok, "DAQ 证明 counter 进入 0..20 并至少两次回绕");
    } else if (current_bytes != original_bytes &&
               current_bytes.size() == original_bytes.size()) {
        std::printf(
            "[cal] threshold 读回为非预期值 %llu；状态已知，将恢复原始字节\n",
            static_cast<unsigned long long>(DecodeLittleEndian(current_bytes)));
    }

    // 对任何已知且不同于原值的状态执行一次补偿恢复；若 WRITE 响应未知，
    // 先前的 UPLOAD 查询已证明当前状态，绝不盲目重发同一 threshold 值。
    bool restore_needed = current_bytes != original_bytes &&
                          current_bytes.size() == original_bytes.size();
    bool restored = !restore_needed;
    if (restore_needed) {
        bool restore_returned = false;
        try {
            master.WriteMemoryBytes(address, extension, original_bytes);
            restore_returned = true;
        } catch (const std::exception& ex) {
            std::printf("[cal] 恢复写命令异常（不重放，查询原值）：%s\n",
                        ex.what());
        }
        try {
            const Bytes verified =
                master.ReadMemoryBytes(address, extension, 2U);
            restored = verified == original_bytes;
            check.Check(restored,
                        restore_returned
                            ? "恢复 counter_max 原始字节并读回验证"
                            : "恢复响应异常后 UPLOAD 证明原始字节已恢复");
        } catch (const std::exception& ex) {
            std::printf("[cal] 恢复状态无法确认：%s\n", ex.what());
            check.Check(false, "恢复后可查询并证明原始字节");
        }
    } else {
        check.Check(true, "counter_max 保持原始字节（低阈值未生效）");
    }

    if (!restored) {
        std::printf("[cal] 恢复未获证明；终止本次 Slave 测试进程\n");
        return false;
    }

    sink.ResetRawTrace(counter_name);
    const bool restored_behavior = sink.WaitUntil(
        [&] { return sink.SawRawAbove(counter_name, kReduced); }, 3s);
    check.Check(restored_behavior,
                "恢复原值后 DAQ counter 再次观测到 >20");
    return reduced_applied && behavior_ok && restored && restored_behavior;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        return 2;
    }
    Checker check;

    const std::vector<std::string> symbols = opt.Symbols();
    if (symbols.empty()) {
        std::printf("[usage] --symbols 为空\n");
        return 2;
    }
    const std::string projectPrefix = opt.project + "::";
    auto Qualified = [&](std::string_view symbol) {
        return projectPrefix + std::string(symbol);
    };
    auto ShortName = [&](std::string_view qualified) {
        return qualified.substr(projectPrefix.size());
    };

    std::error_code ec;
    fs::create_directories(opt.runDir, ec);
    if (ec) {
        std::printf("[env] 无法创建 run-dir %s：%s\n",
                    opt.runDir.string().c_str(), ec.message().c_str());
        return 2;
    }
    if (opt.calibrationProbe) {
        ec.clear();
        fs::remove(opt.runDir / "cpp_demo_V201.bin", ec);
        if (ec) {
            std::printf("[env] 无法清理测试专用 cpp_demo 持久化文件：%s\n",
                        ec.message().c_str());
            return 2;
        }
        std::printf("[cal] 已清理本测试 run-dir 的 cpp_demo_V201.bin\n");
    }
    if (!fs::exists(opt.slaveExe, ec) || ec) {
        std::printf("[env] Slave 可执行不存在：%s\n",
                    opt.slaveExe.string().c_str());
        return 2;
    }
#ifdef XCP_104_AML_FILE
    // XCPlite 生成 A2L 含 `/include "XCP_104.aml"`（a2l_writer.c:474）。为
    // cpp_demo 一类不内嵌 AML 的 Slave 预置同目录副本，保证桥接 include 链
    // 可解析；已存在则不动（xcp_test_slave 自写的以 Slave 为准）。
    {
        const fs::path aml = opt.runDir / "XCP_104.aml";
        if (!fs::exists(aml, ec) && !ec) {
            std::error_code cepy;
            fs::copy_file(XCP_104_AML_FILE, aml,
                          fs::copy_options::skip_existing, cepy);
            if (cepy) {
                std::printf("[env] 预置 XCP_104.aml 失败（源 %s）：%s\n",
                            static_cast<const char*>(XCP_104_AML_FILE),
                            cepy.message().c_str());
                return 2;
            }
        }
    }
#endif

    // 预检：端口必须初始无人应答——连到上一轮残留 Slave 会让整场冒烟变成
    // 假证据（口径同 xcp_master_udp::PortLooksOccupied）。
    {
        Options probeOpt = opt;
        probeOpt.commandTimeout = 500ms;
        auto early = MakeMaster(probeOpt, nullptr);
        bool occupied = false;
        try {
            early->Connect();
            occupied = true;
            try {
                early->Disconnect();
            } catch (...) {
            }
        } catch (...) {
        }
        early.reset();
        if (occupied) {
            std::printf(
                "[env] 端口 %u 已有 XCP 应答（残留 "
                "Slave？）——拒绝在脏端口上取证\n",
                opt.port);
            return 2;
        }
    }

    // —— 1) 拉起 Slave（工作目录 = run-dir，A2L/AML 落盘于此）——
    xcp_example::DemoProcess slave;
    std::string startErr;
    if (!slave.Start(opt.slaveExe.string(), opt.runDir, &startErr)) {
        std::printf("[env] Slave 启动失败：%s\n", startErr.c_str());
        return 2;
    }
    std::printf("[info] Slave 已启动（pid=%u），目标 %s:%u，项目 %s，模式 %s\n",
                slave.ProcessId(), opt.host.c_str(), opt.port,
                opt.project.c_str(), opt.dynamicMode ? "dynamic" : "fixed");

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
        const fs::path uploadedPath = opt.runDir / opt.a2lName;
        {
            std::ofstream out(uploadedPath, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(uploaded.data()),
                      static_cast<std::streamsize>(uploaded.size()));
        }
        check.Check(uploaded.size() > 100U, "UPLOAD A2L 长度合理");

        // —— 3) 桥接解析 + 适配器包装（唯一桥接接触点在 adapter/）——
        auto loaded = a2l::A2lBridge::Load(uploadedPath.string());
        if (!loaded.HasValue()) {
            std::printf("[env] A2L 加载失败：%s\n",
                        loaded.ErrorInfo().message.c_str());
            slave.Stop();
            return 2;
        }
        const a2l::IA2lDatabase* db = loaded.Value()->Database();
        A2lMeasurementDatabase adapter(*db);

        // 建立 A2L EVENT 原始事实表；解析失败不得回退或猜测。
        const std::string a2lText(
            reinterpret_cast<const char*>(uploaded.data()), uploaded.size());
        const auto parsed_events = ResolveA2lEvents(a2lText);
        if (!parsed_events.has_value()) {
            std::printf("[env] 无法可靠解析上传 A2L 的 EVENT 元数据\n");
            slave.Stop();
            return 2;
        }

        std::optional<A2lEventMetadata> fallback_event;
        for (const A2lEventMetadata& event : *parsed_events) {
            if (event.name != opt.event && event.short_name != opt.event) {
                continue;
            }
            if (fallback_event.has_value() &&
                (fallback_event->channel != event.channel ||
                 fallback_event->time_cycle != event.time_cycle ||
                 fallback_event->time_unit != event.time_unit)) {
                std::printf("[env] --event %s 在 A2L 中存在歧义\n",
                            opt.event.c_str());
                slave.Stop();
                return 2;
            }
            fallback_event = event;
        }

        MeasurementEventBindingCandidates bindings;
        std::set<std::string> fallback_symbols;
        for (const auto& sym : symbols) {
            const auto candidates = ResolveSymbolEventChannels(a2lText, sym);
            if (!candidates.has_value()) {
                std::printf("[env] MEASUREMENT 行事件通道格式无效：%s\n",
                            sym.c_str());
                slave.Stop();
                return 2;
            }
            if (!candidates->empty()) {
                bindings[Qualified(sym)] = *candidates;
            } else if (fallback_event.has_value()) {
                // --event 是用户显式指定的回退绑定，不是隐式默认通道。
                bindings[Qualified(sym)] = {fallback_event->channel};
                fallback_symbols.insert(Qualified(sym));
            } else {
                std::printf(
                    "[env] %s 无逐符号 EVENT 绑定，且 A2L 无可用 --event "
                    "回退\n",
                    Qualified(sym).c_str());
                slave.Stop();
                return 2;
            }
        }

        std::map<std::uint16_t, A2lEventMetadata> events_by_channel;
        for (const A2lEventMetadata& event : *parsed_events) {
            const auto [it, inserted] =
                events_by_channel.emplace(event.channel, event);
            if (!inserted && (it->second.time_cycle != event.time_cycle ||
                              it->second.time_unit != event.time_unit)) {
                std::printf("[env] A2L 同一事件通道声明了冲突的周期/单位：%u\n",
                            event.channel);
                slave.Stop();
                return 2;
            }
        }
        std::set<std::uint16_t> bound_channels;
        for (const auto& [symbol, candidates] : bindings) {
            std::set<std::uint64_t> distinct(candidates.begin(),
                                             candidates.end());
            if (distinct.size() != 1U ||
                *distinct.begin() > std::numeric_limits<std::uint16_t>::max()) {
                std::printf("[env] %s 的事件绑定缺失、歧义或超出范围\n",
                            symbol.c_str());
                slave.Stop();
                return 2;
            }
            bound_channels.insert(
                static_cast<std::uint16_t>(*distinct.begin()));
        }
        std::vector<A2lEventMetadata> bound_events;
        for (const std::uint16_t channel : bound_channels) {
            const auto event = events_by_channel.find(channel);
            if (event == events_by_channel.end()) {
                std::printf("[env] 事件通道 %u 缺少对应 A2L EVENT 声明\n",
                            channel);
                slave.Stop();
                return 2;
            }
            bound_events.push_back(event->second);
        }
        for (const auto& sym : symbols) {
            const auto& candidates = bindings.at(Qualified(sym));
            std::printf("[info] 事件通道：%s = %llu%s\n",
                        Qualified(sym).c_str(),
                        static_cast<unsigned long long>(candidates.front()),
                        fallback_symbols.contains(Qualified(sym))
                            ? ("（显式回退 " + opt.event + "）").c_str()
                            : "");
        }
        EventBoundMeasurementDatabase measurement_db(adapter,
                                                     std::move(bindings));

        // —— 4) 会话（先构造作监听器 → Bind → Connect → 取证注入）——
        MeasurementSession session(measurement_db);
        auto master = MakeMaster(opt, &session);
        session.Bind(*master);
        // CONNECT 已在探测阶段验证可用；正式会话重建并重连
        master->Connect();
        SessionGuard guard(
            session);  // 声明于 master 之后：展开时先 Stop 后毁 master

        // 由适配层查询 GET_DAQ_PROCESSOR_INFO / GET_DAQ_RESOLUTION_INFO /
        // GET_DAQ_EVENT_INFO，并与上传 A2L EVENT 元数据逐通道交叉校验。
        const auto profile =
            BuildXcpliteMeasurementRuntimeProfile(*master, bound_events);
        if (!profile.HasValue()) {
            std::printf("[env] Slave profile 取证失败：%s\n",
                        profile.ErrorInfo().message.c_str());
            slave.Stop();
            return 2;
        }
        ApplyMeasurementRuntimeProfile(session, profile.Value());

        for (const auto& sym : symbols) {
            session.Add(Qualified(sym));
        }
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
        if (opt.calibrationProbe &&
            !RunCounterCalibrationProbe(*master, adapter, sink, check,
                                        projectPrefix)) {
            try {
                session.Stop();
                master->Disconnect();
            } catch (const std::exception& ex) {
                std::printf("[cal] 失败清理会话异常：%s\n", ex.what());
            }
            slave.Stop();
            return 1;
        }

        // —— 5) 采集 sampleSeconds 秒并断言 ——
        const std::string firstSymbol = Qualified(symbols.front());
        char expectMsg[256];
        std::snprintf(expectMsg, sizeof(expectMsg),
                      "收到 %s 物理值 == 0x%llX（A2L 定标）",
                      firstSymbol.c_str(),
                      static_cast<unsigned long long>(opt.expect));
        if (!opt.dynamicMode) {
            const bool seen = sink.WaitUntil(
                [&] { return sink.SawU64(firstSymbol, opt.expect); },
                std::chrono::seconds(opt.sampleSeconds) + 3s);
            check.Check(seen, expectMsg);
        }

        // 采样窗口打满：--seconds
        // 的语义是"至少采集这么长时间"（长稳取证口径），
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
        for (std::size_t i = 0; i < std::min<std::size_t>(3, frames.size());
             ++i) {
            const auto& f = frames[i];
            std::printf("frame[%zu] list=%u odt=%u ts_valid=%d ts_raw=%llu\n",
                        i, f.daq_list, f.odt, f.timestamp_valid ? 1 : 0,
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

        // dynamic 模式（L4 冒烟口径）：物理值真实运动的 Slave 不做定值断言，
        // 改为逐符号覆盖 + 变化性（窗口内 ≥2 个不同原始值）+ 可选值域。
        if (opt.dynamicMode) {
            std::printf("\n==== dynamic 逐符号断言 ====\n");
            for (const auto& sym : symbols) {
                const std::string q = Qualified(sym);
                char buf[256];
                std::snprintf(buf, sizeof(buf), "覆盖：%s 有效样本 ≥1",
                              q.c_str());
                check.Check(sink.ValidCount(q) >= 1U, buf);
                std::snprintf(buf, sizeof(buf),
                              "变化性：%s 窗口内 ≥2 个不同原始值", q.c_str());
                check.Check(sink.DistinctCount(q) >= 2U, buf);
                const auto mm = sink.MinMax(q);
                // 取证：dynamic 模式逐符号转储窗口内实际原始字节（hex）。
                // 线宽/定标口径争议（如 cpp_demo DYN 区 4 字节对齐）凭此定性。
                sink.DumpRawKeys(q);
                const auto rng = opt.ranges.find(sym);
                if (rng != opt.ranges.end()) {
                    const double tol =
                        1e-9 + std::abs(rng->second.first) * 1e-9;
                    char msg[320];
                    std::snprintf(msg, sizeof(msg),
                                  "值域：%s ∈ [%.6g,%.6g]（观测 [%.6g,%.6g]）",
                                  q.c_str(), rng->second.first,
                                  rng->second.second, mm ? mm->first : 0.0,
                                  mm ? mm->second : 0.0);
                    check.Check(mm.has_value() &&
                                    mm->first >= rng->second.first - tol &&
                                    mm->second <= rng->second.second + tol,
                                msg);
                }
            }
            if (opt.project == "cpp_demo" &&
                std::find(symbols.begin(), symbols.end(), "temperature") !=
                    symbols.end()) {
                check.Check(
                    sink.SawLinearPair(Qualified("temperature"), 1.0, -50.0),
                    "temperature 同帧 raw/physical 满足 PHYS=1*INT-50");
            }
        }

        const auto stats = session.Statistics();
        std::printf(
            "\n[info] 统计：received=%llu dropped=%llu decode_err=%llu "
            "wraps=%llu\n",
            static_cast<unsigned long long>(stats.dto_received),
            static_cast<unsigned long long>(stats.dto_dropped),
            static_cast<unsigned long long>(stats.decode_errors),
            static_cast<unsigned long long>(stats.timestamp_wraps));
        check.Check(stats.decode_errors == 0U, "全链路零解码错误");
        check.Check(stats.dto_received > 0U, "有 DTO 入队");
        // 长稳口径（--seconds ≥ 30 视为长采窗口）：worker 跟得上事件流 ⇒ 丢包
        // 必须为零（Slave 每 1ms 触发、每帧多 ODT，长窗口下任何持续性掉队都
        // 会体现在 dropped 上）。短冒烟不做此断言：启动瞬态下首拍丢包属正常。
        if (opt.sampleSeconds >= 30) {
            check.Check(stats.dto_dropped == 0U,
                        "长采窗口零丢包（worker 实时性）");
            // 时间戳回绕口径：Slave 的 32 位时间戳是相对 tick 计数（实测
            // 2^32 tick ≈ 4.3s 一圈），长采窗口内回绕十余次~数百次属正常算术
            // 行为——converter 据此外推 +k·2^32 并累计 WrapCount（v0.6 语义）。
            // 判据：回绕确实发生（外推路径在真实流上被走过且未报错）；
            // dynamic 模式再加自洽界：总回绕 ≈ 流数（每 daq_list 一条独立
            // converter 流）× 窗口秒数 ÷ 4.295s（±60%/±2 圈裕量）。
            check.Check(stats.timestamp_wraps > 0U,
                        "32 位时间戳长采窗口按实然回绕（外推路径生效）");
            if (opt.dynamicMode) {
                const std::size_t streams =
                    std::max<std::size_t>(1U, sink.Lists().size());
                const double expected = static_cast<double>(streams) *
                                        opt.sampleSeconds / 4.294967296;
                const auto lo = static_cast<std::uint64_t>(expected * 0.4);
                const auto hi =
                    static_cast<std::uint64_t>(expected * 1.6) + 2U * streams;
                char msg[256];
                std::snprintf(
                    msg, sizeof(msg),
                    "回绕自洽：wraps=%llu ∈ [%llu,%llu]（流数=%zu×%.0fs/4.3s）",
                    static_cast<unsigned long long>(stats.timestamp_wraps),
                    static_cast<unsigned long long>(lo),
                    static_cast<unsigned long long>(hi), streams,
                    static_cast<double>(opt.sampleSeconds));
                check.Check(
                    stats.timestamp_wraps >= lo && stats.timestamp_wraps <= hi,
                    msg);
            }
        }

        session.Stop();
        check.Check(!session.Running(), "Stop 后会话非运行态");

        // Stop→复 Start（L4 第 8 步）：回绕计数跨 Stop→Start 连续累计，
        // 复跑再收一窗流并验证统计只增不清零。此块必须在断连与 Slave
        // 终止**之前**——快验时曾把 slave.Stop() 排在这里，复 Start 对着
        // 已死的 Slave 收流，整步变成假失败。
        if (opt.dynamicMode) {
            const auto statsBefore = stats;
            session.Start(MeasurementCallback(
                [&sink](const MeasurementFrame& f) { sink(f); }));
            std::this_thread::sleep_for(3s);
            session.Stop();
            const auto after = session.Statistics();
            check.Check(after.timestamp_wraps >= statsBefore.timestamp_wraps,
                        "复 Start 后回绕计数连续（不跨 Stop 清零）");
            check.Check(after.dto_received > statsBefore.dto_received,
                        "复 Start 后继续收流（Stop→Start 可重复性）");
            std::printf(
                "[info] 复 Start 3s：received %llu→%llu，wraps %llu→%llu\n",
                static_cast<unsigned long long>(statsBefore.dto_received),
                static_cast<unsigned long long>(after.dto_received),
                static_cast<unsigned long long>(statsBefore.timestamp_wraps),
                static_cast<unsigned long long>(after.timestamp_wraps));
        }

        // 第 9 步口径：断连（复 Start 之后才做）。断连后无 DAQ 残留由 Stop
        // 路径的 DAQ 复位保证；见 v0.9 §6。
        master->Disconnect();
        check.Check(!master->IsConnected(),
                    "Disconnect 完成（L4 第 9 步：断连口径）");
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
