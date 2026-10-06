// xcp_master_udp.cpp — 独立示例：libxcp XcpMaster 通过 UDP 连接 XCPlite cpp_demo 原样对手端。
//
// 单命令自动两轮：
//   Run A：在本目录拉起 cpp_demo（工作目录 = run-dir），首次 CONNECT 触发 A2L 生成
//          （A2L_MODE_WRITE_ONCE | A2L_MODE_FINALIZE_ON_CONNECT），确认 cpp_demo_V201.a2l
//          落盘后终止 demo 进程。
//   桥接：用 libxcp::a2lbridge 加载该 A2L，建立符号地址表。
//   Run B：在同一 run-dir 重启 cpp_demo（WRITE_ONCE ⇒ 复用同一 A2L），连接、读取默认
//          符号集并做合理性断言；可选 --watch 秒级轮询动态量；默认执行 calseg 写回读。
//
// 退出码：0 = 全部断言通过；1 = 断言失败；2 = 环境错误（demo 缺失 / 端口占用 / 连接失败）。
//
// 约束：不修改 thirdparty/XCPlite；不依赖 tests/；C++20；绝对路径经编译定义注入。

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include <libxcp/a2l/a2l_bridge.hpp>
#include <libxcp/a2l/a2l_result.hpp>
#include <libxcp/a2l/a2l_types.hpp>
#include <libxcp/a2l/ia2l_database.hpp>
#include <libxcp/command_executor.hpp>
#include <libxcp/protocol_types.hpp>
#include <libxcp/udp_transport.hpp>
#include <libxcp/udp_transport_config.hpp>
#include <libxcp/xcp_error.hpp>
#include <libxcp/xcp_master.hpp>

#include "demo_process.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

/// @brief 命令行配置（全部有合理默认值，零参数即可运行）。
struct Options {
    std::string host = "127.0.0.1";                    ///< XCP slave 地址。
    uint16_t port = 5555;                              ///< XCP slave 端口（cpp_demo 固定监听值）。
    fs::path runDir = "xcp_master_udp_run";            ///< 运行目录（demo 的 cwd，A2L 落盘处）。
    fs::path demoExe = CPP_DEMO_EXECUTABLE;            ///< cpp_demo 可执行文件（编译定义注入）。
    int watchSeconds = 0;                              ///< >0 时追加轮询秒数。
    bool writeback = true;                             ///< 是否执行 calseg 写回读（--no-writeback 关闭）。
    std::chrono::milliseconds commandTimeout{2000};    ///< 单命令超时。
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

    /// @brief 打印汇总结论；有失败时返回 false（主程序据此置退出码 1）。
    bool Report() const {
        std::printf("\n==== 断言汇总：%d 项，失败 %d 项 ====\n", total_, failed_);
        return failed_ == 0;
    }

private:
    int total_ = 0;
    int failed_ = 0;
};

/// @brief 把字节序列格式化为大写十六进制串（用于打印原始读取值）。
std::string HexDump(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0xF]);
        out.push_back(' ');
    }
    if (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/// @brief 小端整数编码为 elementSize 字节（仅支持 1/2/4/8）。
std::vector<uint8_t> EncodeLittleEndian(uint64_t value, uint8_t elementSize) {
    std::vector<uint8_t> bytes(elementSize, 0);
    for (uint8_t i = 0; i < elementSize; ++i) {
        bytes[i] = static_cast<uint8_t>((value >> (8U * i)) & 0xFFU);
    }
    return bytes;
}

/// @brief 打印 PhysicalValue（variant）为可读文本。
void PrintPhysical(const calmcar::xcp::a2l::PhysicalValue& value) {
    std::visit([](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            std::printf("\"%s\"", v.c_str());
        } else if constexpr (std::is_same_v<T, bool>) {
            std::printf("%s", v ? "true" : "false");
        } else if constexpr (std::is_floating_point_v<T>) {
            std::printf("%.6g", static_cast<double>(v));
        } else {
            std::printf("%lld", static_cast<long long>(v));
        }
    }, value);
}

/// @brief 建立 master 实例（UDP transport，按值配置）。
std::unique_ptr<calmcar::xcp::XcpMaster> MakeMaster(const Options& opt) {
    calmcar::xcp::UdpTransportConfig cfg;
    cfg.remote_host = opt.host;
    cfg.remote_port = opt.port;
    cfg.local_host = "0.0.0.0";
    cfg.local_port = 0;
    cfg.receive_poll_interval_ms = 20;
    calmcar::xcp::CommandTimeouts timeouts;
    timeouts.command_timeout = opt.commandTimeout;
    timeouts.synch_timeout = opt.commandTimeout;
    timeouts.max_retries = 1;
    return std::make_unique<calmcar::xcp::XcpMaster>(
        std::make_unique<calmcar::xcp::UdpTransport>(cfg), timeouts);
}

/// @brief 等待 demo 达到可 CONNECT 状态；成功返回已连接的 master，超时返回 nullptr。
std::unique_ptr<calmcar::xcp::XcpMaster> WaitConnected(const Options& opt, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::string lastError;
    while (std::chrono::steady_clock::now() < deadline) {
        auto master = MakeMaster(opt);
        try {
            master->Connect();
            return master;
        } catch (const calmcar::xcp::XcpException& ex) {
            lastError = ex.what();
        } catch (const std::exception& ex) {
            lastError = ex.what();
        }
        std::this_thread::sleep_for(200ms);
    }
    std::printf("[info] 等待连接超时，最后错误：%s\n", lastError.c_str());
    return nullptr;
}

/// @brief 预检端口是否已被占用：能直接 CONNECT 上即视为被占。
bool PortLooksOccupied(const Options& opt) {
    Options probe = opt;
    probe.commandTimeout = 300ms;
    auto master = MakeMaster(probe);
    try {
        master->Connect();
        master->Disconnect();
        return true;
    } catch (...) {
        return false;
    }
}

/// @brief 轮询等待 run-dir 中出现指定文件（A2L 生成落盘）。
fs::path WaitForFile(const fs::path& dir, std::string_view fileName, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    do {
        std::error_code ec;
        fs::path candidate = dir / fileName;
        if (fs::exists(candidate, ec) && !ec && fs::file_size(candidate, ec) > 0 && !ec) {
            return candidate;
        }
        std::this_thread::sleep_for(100ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return {};
}

/// @brief 从 A2L 数据库解析符号，失败时打印原因（是否计 FAIL 由调用方按语义决定）。
std::optional<calmcar::xcp::a2l::SymbolInfo> Resolve(const calmcar::xcp::a2l::IA2lDatabase& db,
                                                    std::string_view name, std::string_view purpose) {
    auto res = db.Find(name);
    if (!res.HasValue()) {
        std::printf("[info] 解析 %.*s（用于 %.*s）失败：%s\n",
                    static_cast<int>(name.size()), name.data(),
                    static_cast<int>(purpose.size()), purpose.data(),
                    calmcar::xcp::a2l::ToString(res.ErrorInfo().code));
        return std::nullopt;
    }
    return res.Value();
}

/// @brief over-read 口径：把期望字节数 want 提升为 n′ = 4·⌈(1+want)/4⌉ − 1（即 n′≡3 mod 4），
/// 使 1+n′ 恰为 4 的倍数——xcplite 异步队列（queueAcquire）无 fill 可加，线长精确等于 n′；
/// 同步直发路径（xcp_test_slave 的 ext=1 符号）同样返回精确 n′。读取后截取前 want 字节。
/// 依据与实证见 ReadScalar 头注释（queue32.c:229-237 上游 TODO、xcp_cfg.h:174-176 DYN 区间）。
std::vector<uint8_t> ReadMemoryAligned(calmcar::xcp::XcpMaster& master, calmcar::xcp::Address addr,
                                       calmcar::xcp::AddressExtension ext, std::size_t want) {
    const auto request = static_cast<calmcar::xcp::ByteCount>(((want + 4) / 4) * 4 - 1);
    auto raw = master.ReadMemoryBytes(addr, ext, request);
    raw.resize(want);
    return raw;
}

/// @brief 读取一个标量符号：原始字节 + A2L 物理值 + ToPhysical 一致性。
/// 线长适配：cpp_demo 四测量量位于 DYN 扩展地址区间（a2l 实证 ADDRESS 0x800000、ext=3..6 ∈
/// [0x02,0x0F]，见 xcp_cfg.h:174-176 "asynchronous access"），其 SHORT_UPLOAD/UPLOAD 响应在
/// Slave 侧经异步队列，queueAcquire 把整包补齐到 4 字节且把 fill 计入线长
/// （queue32.c:229-237 为上游自认 TODO），主端精确长度校验因而不是抛异常。实证规律：
/// 响应数据段长度 = round4(1+n)−1（n=1/2/8/8 → 3/3/11/11）。请求 n′≡3 (mod 4) 时
/// 1+n′ 恰为 4 的倍数、队列无 fill 可加，异步（DYN）与同步直发（xcp_test_slave 之 ext=1）
/// 两条路径均返回精确 n′ 字节；示例统一按 n′ = 4·⌈(1+want)/4⌉ − 1 over-read 后截取 want。
void ReadScalar(Checker& check, calmcar::xcp::XcpMaster& master, const calmcar::xcp::a2l::IA2lDatabase& db,
                std::string_view name) {
    auto sym = Resolve(db, name, name);
    if (!sym) {
        check.Check(false, std::string(name) + " 可解析（默认符号集断言）");
        return;
    }
    const auto size = sym->element_size_bytes;
    std::vector<uint8_t> raw;
    try {
        raw = ReadMemoryAligned(master, static_cast<calmcar::xcp::Address>(sym->xcp_address),
                                static_cast<calmcar::xcp::AddressExtension>(sym->address_extension),
                                static_cast<std::size_t>(size));
    } catch (const std::exception& ex) {
        std::printf("[FAIL] 读取 %.*s 异常：%s\n", static_cast<int>(name.size()), name.data(), ex.what());
        check.Check(false, name);
        return;
    }
    std::string tag(name);
    tag += " 读取（";
    tag += HexDump(raw);
    tag += "）";
    check.Check(raw.size() == size, tag);
    auto phys = db.ToPhysical(name, std::span<const uint8_t>(raw));
    if (phys.HasValue()) {
        std::printf("[info] %.*s 物理值 = ", static_cast<int>(name.size()), name.data());
        PrintPhysical(phys.Value());
        std::printf("\n");
    } else {
        std::printf("[info] %.*s ToPhysical 失败：%s\n", static_cast<int>(name.size()), name.data(),
                    calmcar::xcp::a2l::ToString(phys.ErrorInfo().code));
    }
}

/// @brief 写回读（CalSeg 结构成员口径）：INSTANCE 基址 + 成员偏移直址——
/// 写入新值、读回校验、恢复原值、再读回确认。
///
/// 路线选择（批次22 实施记录·写回读定稿）：
/// - 叶子路径 Find 实然不可用：桥接叶子展开对标量成员只认 TYPEDEF_MEASUREMENT
///   （a2l_bridge.cpp:85-102 ListTypedefMeasurements + structure_layout.cpp:248-252/334-338
///   查不到即拒绝），而 cpp_demo 生成 A2L 的成员引用是 TYPEDEF_CHARACTERISTIC
///   （C_counter_max/C_delay_us，生成件 :106-107）→ kParameters 叶子不登记；
/// - 改走 INSTANCE+offset 直址，沿用批次18 写测试先例 tests/xcplite_write_test.cpp:191-217；
/// - 不以 sym->read_write 设门槛：桥接把 STRUCTURE/INSTANCE 标 false
///   （xcplite_a2l_bridge.cpp:910-914），与运行期 calseg 实际可写矛盾（计划 §1:20
///   已实证记录）——本写回读环节正是对该矛盾的现场检验，解析失败必须计 FAIL，
///   杜绝此前"仅打 info 静默跳过"的空转。
///
/// 成员偏移与宽度取生成 A2L 实证：STRUCTURE_COMPONENT counter_max 0x0 / delay_us 0x4
/// （生成件 :71-72），宽度 U16/U32 取 C_counter_max/C_delay_us（:106-107）。
///
/// 窗口右对齐读取（round11 实证）：over-read 若从成员起点直读会越出段尾被 Slave
/// 边界校验拒绝（`ERROR: out of bound calseg read access (addr=80010004, size=7)`
/// → ERR_ACCESS_DENIED；kParameters SIZE 0x8）。故读窗 n′=4·⌈(1+memberSize)/4⌉−1
/// 右对齐到成员末端（windowStart=max(0, offset+size−n′)，此处恒满足 windowStart+
/// n′ ≥ offset+size，窗口必覆盖成员），写命令仍按成员精确宽度直址。
void WriteBackMemberProbe(Checker& check, calmcar::xcp::XcpMaster& master,
                          const calmcar::xcp::a2l::IA2lDatabase& db, std::string_view instanceName,
                          std::string_view memberName, std::uint64_t memberOffset, std::size_t memberSize,
                          uint64_t probeValue, uint64_t expectedOriginal) {
    const std::string tag = std::string(instanceName) + "." + std::string(memberName);
    auto sym = Resolve(db, instanceName, "写回读（INSTANCE 基址）");
    if (!sym) {
        check.Check(false, "写回读前提：" + tag + " 所属实例可解析");
        return;
    }
    const auto base = static_cast<calmcar::xcp::Address>(sym->xcp_address);
    const auto addr = static_cast<calmcar::xcp::Address>(base + static_cast<calmcar::xcp::Address>(memberOffset));
    const auto ext = static_cast<calmcar::xcp::AddressExtension>(sym->address_extension);
    const std::size_t window = ((memberSize + 4) / 4) * 4 - 1; // ≡3 mod 4，队列为齐
    const std::uint64_t windowStart = (memberOffset + memberSize > window) ? (memberOffset + memberSize - window) : 0U;
    const std::size_t shift = static_cast<std::size_t>(memberOffset - windowStart);
    auto readMember = [&]() {
        auto win = ReadMemoryAligned(master, static_cast<calmcar::xcp::Address>(base + static_cast<calmcar::xcp::Address>(windowStart)),
                                     ext, window);
        return std::vector<uint8_t>(win.begin() + static_cast<std::ptrdiff_t>(shift),
                                    win.begin() + static_cast<std::ptrdiff_t>(shift + memberSize));
    };
    try {
        const auto original = readMember();
        uint64_t originalValue = 0;
        for (std::size_t i = 0; i < original.size(); ++i) {
            originalValue |= static_cast<uint64_t>(original[i]) << (8U * i);
        }
        std::printf("[info] %s 当前值 = %llu\n", tag.c_str(),
                    static_cast<unsigned long long>(originalValue));
        check.Check(originalValue == expectedOriginal,
                    tag + " 静态钉值 " + std::to_string(expectedOriginal));
        const auto probe = EncodeLittleEndian(probeValue, static_cast<uint8_t>(memberSize));
        master.WriteMemoryBytes(addr, ext, std::span<const uint8_t>(probe));
        auto back = readMember();
        uint64_t backValue = 0;
        for (std::size_t i = 0; i < back.size(); ++i) {
            backValue |= static_cast<uint64_t>(back[i]) << (8U * i);
        }
        check.Check(backValue == probeValue, tag + " 写入 " + std::to_string(probeValue) + " 后读回");
        const auto restore = EncodeLittleEndian(originalValue, static_cast<uint8_t>(memberSize));
        master.WriteMemoryBytes(addr, ext, std::span<const uint8_t>(restore));
        back = readMember();
        uint64_t restoredValue = 0;
        for (std::size_t i = 0; i < back.size(); ++i) {
            restoredValue |= static_cast<uint64_t>(back[i]) << (8U * i);
        }
        check.Check(restoredValue == originalValue, tag + " 恢复 " + std::to_string(originalValue) + " 后读回");
    } catch (const std::exception& ex) {
        std::printf("[FAIL] %s 写回读异常：%s\n", tag.c_str(), ex.what());
        check.Check(false, tag + " 写回读");
    }
}

/// @brief --watch：按秒轮询几个动态量并打印（演示连续采集，不做断言）。
void WatchLoop(calmcar::xcp::XcpMaster& master, const calmcar::xcp::a2l::IA2lDatabase& db, int seconds) {
    static constexpr std::string_view kWatchSymbols[] = {"counter", "temperature", "speed"};
    std::vector<calmcar::xcp::a2l::SymbolInfo> symbols;
    for (auto name : kWatchSymbols) {
        auto res = db.Find(name);
        if (res.HasValue()) {
            symbols.push_back(res.Value());
        }
    }
    std::printf("\n==== watch %d 秒（每 500ms 一轮，仅打印） ====\n", seconds);
    for (int round = 0; round < seconds * 2; ++round) {
        std::printf("t=%4.1fs", round * 0.5);
        for (const auto& sym : symbols) {
            try {
                const auto raw = ReadMemoryAligned(master, static_cast<calmcar::xcp::Address>(sym.xcp_address),
                                                   static_cast<calmcar::xcp::AddressExtension>(sym.address_extension),
                                                   static_cast<std::size_t>(sym.element_size_bytes));
                std::printf("  %s=[%s]", std::string(sym.name).c_str(), HexDump(raw).c_str());
            } catch (const std::exception& ex) {
                std::printf("  %s=<err %s>", std::string(sym.name).c_str(), ex.what());
            }
        }
        std::printf("\n");
        std::this_thread::sleep_for(500ms);
    }
}

/// @brief 解析 --host/--port/--run-dir/--watch/--no-writeback/--demo/--timeout/--help。
bool ParseArgs(int argc, char** argv, Options& opt) {
    auto needValue = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::printf("[usage] 参数 %s 缺少取值\n", argv[i]);
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::printf("用法: xcp_master_udp [--host IP] [--port N] [--run-dir DIR] [--demo PATH]\n"
                        "                      [--watch SECONDS] [--no-writeback] [--timeout-ms N]\n"
                        "零参数运行即自动完成两轮：Run A 生成 A2L，Run B 连接读取并写回读。\n");
            return false;
        } else if (arg == "--host") {
            if (const char* v = needValue(i)) opt.host = v; else return false;
        } else if (arg == "--port") {
            const char* v = needValue(i);
            if (!v) return false;
            unsigned long parsed = 0;
            auto [p, ec] = std::from_chars(v, v + std::strlen(v), parsed);
            if (ec != std::errc{} || parsed == 0 || parsed > 65535) {
                std::printf("[usage] 非法端口: %s\n", v);
                return false;
            }
            opt.port = static_cast<uint16_t>(parsed);
        } else if (arg == "--run-dir") {
            if (const char* v = needValue(i)) opt.runDir = v; else return false;
        } else if (arg == "--demo") {
            if (const char* v = needValue(i)) opt.demoExe = v; else return false;
        } else if (arg == "--watch") {
            const char* v = needValue(i);
            if (!v) return false;
            opt.watchSeconds = std::max(1, std::atoi(v));
        } else if (arg == "--no-writeback") {
            opt.writeback = false;
        } else if (arg == "--timeout-ms") {
            const char* v = needValue(i);
            if (!v) return false;
            int ms = std::atoi(v);
            if (ms <= 0) {
                std::printf("[usage] 非法超时: %s\n", v);
                return false;
            }
            opt.commandTimeout = std::chrono::milliseconds(ms);
        } else {
            std::printf("[usage] 未知参数: %s\n", std::string(arg).c_str());
            return false;
        }
    }
    return true;
}

} // namespace

/// @brief 示例入口：返回 0 成功 / 1 断言失败 / 2 环境错误。
int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        return 2;
    }

    std::error_code ec;
    fs::create_directories(opt.runDir, ec);
    if (ec) {
        std::printf("[env] 无法创建运行目录 %s：%s\n", opt.runDir.string().c_str(), ec.message().c_str());
        return 2;
    }
    fs::path runDir = fs::absolute(opt.runDir, ec);

    if (!fs::exists(opt.demoExe, ec) || ec) {
        std::printf("[env] 未找到 cpp_demo 可执行文件：%s（可用 --demo 指定）\n", opt.demoExe.string().c_str());
        return 2;
    }

    // 预置 A2L 的 include 依赖：生成件含 `/include "XCP_104.aml"`
    // （thirdparty/XCPlite/src/a2l_writer.c:474；cpp_demo 未用 A2L_MODE_EMBED_AML_FILE
    // 内嵌，见 inc/a2l.h:66），路径由 CMake 注入 compile-def，禁硬编码。
#ifdef XCP_104_AML_FILE
    {
        std::error_code copyEc;
        fs::copy_file(fs::path(XCP_104_AML_FILE), runDir / "XCP_104.aml",
                      fs::copy_options::overwrite_existing, copyEc);
        if (copyEc) {
            std::printf("[env] 预置 XCP_104.aml 失败（来源 %s）：%s\n",
                        XCP_104_AML_FILE, copyEc.message().c_str());
            return 2;
        }
        std::printf("[info] 已预置 XCP_104.aml 到 run-dir（A2L include 解析前提）\n");
    }
#endif

    if (PortLooksOccupied(opt)) {
        std::printf("[env] 端口 %u 已被占用（疑似已有 XCP slave 在跑），请先停止\n", opt.port);
        return 2;
    }

    Checker check;

    // ---------- Run A：拉起 demo，触发 A2L 生成 ----------
    std::printf("\n==== Run A：生成 A2L ====\n");
    std::printf("[V1] 端口占用预检通过\n");
    xcp_example::DemoProcess demo;
    std::string startError;
    if (!demo.Start(opt.demoExe.string(), runDir, &startError)) {
        std::printf("[env] 启动 cpp_demo 失败：%s\n", startError.c_str());
        return 2;
    }
    std::printf("[info] cpp_demo 已启动（PID %llu）\n", static_cast<unsigned long long>(demo.ProcessId()));

    auto masterA = WaitConnected(opt, 15s);
    if (!masterA) {
        std::printf("[env] Run A 无法连接 cpp_demo（15 秒超时）\n");
        demo.Stop();
        return 2;
    }
    std::printf("[V2] Run A CONNECT 成功（FINALIZE_ON_CONNECT 已触发）\n");

    const std::string a2lName = "cpp_demo_V201.a2l";
    fs::path a2lPath = WaitForFile(runDir, a2lName, 10s);
    check.Check(!a2lPath.empty(), "Run A 后 " + a2lName + " 落盘");
    if (a2lPath.empty()) {
        std::printf("[env] run-dir=%s 中未发现 %s（V1 实证：A2L 未按预期生成）\n",
                    runDir.string().c_str(), a2lName.c_str());
        masterA->Disconnect();
        demo.Stop();
        return 2;
    }
    std::printf("[V1 实证] A2L 路径 = %s（%llu 字节）\n", a2lPath.string().c_str(),
                static_cast<unsigned long long>(fs::file_size(a2lPath)));

    // 全量清单一次 UPLOAD：验证 FetchA2lViaUpload 与磁盘文件一致。
    try {
        const auto uploaded = masterA->FetchA2lViaUpload();
        std::ifstream fin(a2lPath, std::ios::binary);
        std::vector<uint8_t> disk((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
        check.Check(uploaded == disk, "UPLOAD 全量 == 磁盘 A2L（" + std::to_string(uploaded.size()) + " 字节）");
        std::printf("[V3 实证] UPLOAD 与磁盘文件%s\n", uploaded == disk ? "逐字节一致" : "不一致！");
    } catch (const std::exception& ex) {
        std::printf("[info] FetchA2lViaUpload 失败（V3 实证：GET_ID/UPLOAD 路径异常）：%s\n", ex.what());
        check.Check(false, "FetchA2lViaUpload 可用");
    }
    masterA->Disconnect();
    demo.Stop();
    std::printf("[info] Run A 结束，cpp_demo 已终止\n");

    // ---------- 桥接加载 A2L ----------
    calmcar::xcp::a2l::LoadOptions loadOptions;
    loadOptions.require_if_data_xcp = false; // cpp_demo 的 A2L 不含 IF_DATA XCP（V3 实证前提）。
    auto bridgeRes = calmcar::xcp::a2l::A2lBridge::Load(a2lPath.string(), loadOptions);
    if (!bridgeRes.HasValue()) {
        std::printf("[env] A2L 加载失败：%s\n", bridgeRes.ErrorInfo().message.c_str());
        return 2;
    }
    const auto& db = *bridgeRes.Value()->Database();
    const auto countRes = db.Count();
    std::printf("[V3 实证] A2L 加载成功，符号总数 %zu\n",
                countRes.HasValue() ? static_cast<std::size_t>(countRes.Value()) : std::size_t{0});

    // ---------- Run B：同 run-dir 重启，连接读取 ----------
    std::printf("\n==== Run B：连接读取 ====\n");
    xcp_example::DemoProcess demoB;
    if (!demoB.Start(opt.demoExe.string(), runDir, &startError)) {
        std::printf("[env] Run B 启动 cpp_demo 失败：%s\n", startError.c_str());
        return 2;
    }
    auto masterB = WaitConnected(opt, 15s);
    if (!masterB) {
        std::printf("[env] Run B 无法连接 cpp_demo（V4 实证：WRITE_ONCE 复用失败）\n");
        demoB.Stop();
        return 2;
    }
    check.Check(true, "Run B 复用同一 A2L 并 CONNECT 成功（V4）");

    const auto session = masterB->GetSessionParameters();
    std::printf("[info] CONNECT 回参：MAX_CTO=%u MAX_DTO=%u byte_order=%d addr_gran=%d resources=0x%02X\n",
                static_cast<unsigned>(session.connect.max_cto),
                static_cast<unsigned>(session.connect.max_dto),
                static_cast<int>(session.connect.byte_order),
                static_cast<int>(session.connect.address_granularity),
                static_cast<unsigned>(session.connect.resource_mask));
    std::printf("[V5 实证] 传输层字节序与 A2L BYTE_ORDER（MSB_LAST=小端）比对见上\n");

    // 默认符号集：来自 cpp_demo main.cpp 注册清单（b16 实读 :33-45/:124-128/:144）。
    static constexpr std::string_view kScalarSymbols[] = {
        "temperature", // uint8 全局，线性转换 1.0/-50 → 物理 0..150
        "speed",       // double 全局，物理 0..250
        "counter",     // uint16 局部（STACK），0..counter_max
        "sum",         // double 局部
    };
    std::printf("\n---- 标量符号读取 ----\n");
    for (auto name : kScalarSymbols) {
        ReadScalar(check, *masterB, db, name);
    }

    // 动态性：隔 300ms 两次读 counter，应发生变化（主循环 1ms 级）。
    {
        auto sym = Resolve(db, "counter", "动态性验证");
        if (sym) {
            try {
                const auto first = ReadMemoryAligned(*masterB, static_cast<calmcar::xcp::Address>(sym->xcp_address),
                                                     static_cast<calmcar::xcp::AddressExtension>(sym->address_extension),
                                                     static_cast<std::size_t>(sym->element_size_bytes));
                std::this_thread::sleep_for(300ms);
                const auto second = ReadMemoryAligned(*masterB, static_cast<calmcar::xcp::Address>(sym->xcp_address),
                                                      static_cast<calmcar::xcp::AddressExtension>(sym->address_extension),
                                                      static_cast<std::size_t>(sym->element_size_bytes));
                check.Check(first != second, "counter 两次读取变化（动态采集通路可用）");
            } catch (const std::exception& ex) {
                std::printf("[FAIL] counter 动态性验证异常：%s\n", ex.what());
                check.Check(false, "counter 动态性验证");
            }
        }
    }

    // ---------- 写回读（默认执行，--no-writeback 跳过） ----------
    if (opt.writeback) {
        std::printf("\n---- CalSeg 结构成员写回读（kParameters INSTANCE+offset 直址） ----\n");
        WriteBackMemberProbe(check, *masterB, db, "kParameters", "counter_max", 0x0, 2, 1234, 1000);
        WriteBackMemberProbe(check, *masterB, db, "kParameters", "delay_us", 0x4, 4, 2000, 1000);
    } else {
        std::printf("[info] --no-writeback：跳过 calseg 写回读\n");
    }

    // ---------- 可选轮询 ----------
    if (opt.watchSeconds > 0) {
        WatchLoop(*masterB, db, opt.watchSeconds);
    }

    // ---------- 收尾 ----------
    std::printf("\n==== 收尾 ====\n");
    try {
        masterB->Disconnect();
        check.Check(true, "DISCONNECT 正常");
    } catch (const std::exception& ex) {
        std::printf("[info] DISCONNECT 异常（实然记录，不计失败）：%s\n", ex.what());
    }
    masterB.reset();
    demoB.Stop();
    std::printf("[info] cpp_demo 已终止；运行目录保留于 %s\n", runDir.string().c_str());

    const bool passed = check.Report();
    return passed ? 0 : 1;
}
