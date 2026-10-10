/**
 * @file main.cpp
 * @brief 通用 XCP on UDP Master 命令行工具。
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/udp_transport.hpp"
#include "libxcp/xcp_master.hpp"
#include "symbol_policy.hpp"

namespace {

using calmcar::xcp::Address;
using calmcar::xcp::AddressExtension;
using calmcar::xcp::ByteCount;
using calmcar::xcp::Bytes;
using calmcar::xcp::XcpMaster;
using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::CharacteristicType;
using calmcar::xcp::a2l::PhysicalValue;
using calmcar::xcp::a2l::SymbolInfo;
using calmcar::xcp::a2l::SymbolKind;

struct Options {
    std::string a2l_path;
    std::string host;
    std::uint16_t port{0};
    std::string module;
    std::chrono::milliseconds interval{500};
};

void PrintUsage(std::ostream& out) {
    out << "Usage: xcp_master --a2l <path> --host <IPv4> --port <1..65535> "
           "[--module <name>] [--interval-ms <ms>]\n"
           "       xcp_master --help\n";
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    std::uint64_t parsed = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    value = parsed;
    return true;
}

std::optional<Options> ParseOptions(int argc, char** argv, bool& help,
                                    std::string& error) {
    Options options;
    help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key(argv[i]);
        if (key == "--help" || key == "-h") {
            help = true;
            return options;
        }
        if (i + 1 >= argc) {
            error = "missing value for " + std::string(key);
            return std::nullopt;
        }
        const std::string value(argv[++i]);
        if (key == "--a2l") {
            options.a2l_path = value;
        } else if (key == "--host") {
            options.host = value;
        } else if (key == "--port") {
            std::uint64_t parsed = 0;
            if (!ParseUnsigned(value, parsed) || parsed == 0 || parsed > 65535) {
                error = "--port must be in range 1..65535";
                return std::nullopt;
            }
            options.port = static_cast<std::uint16_t>(parsed);
        } else if (key == "--module") {
            options.module = value;
        } else if (key == "--interval-ms") {
            std::uint64_t parsed = 0;
            if (!ParseUnsigned(value, parsed) || parsed == 0 ||
                parsed > static_cast<std::uint64_t>(std::chrono::milliseconds::max().count())) {
                error = "--interval-ms must be a positive integer";
                return std::nullopt;
            }
            options.interval = std::chrono::milliseconds(parsed);
        } else {
            error = "unknown option: " + std::string(key);
            return std::nullopt;
        }
    }
    if (options.a2l_path.empty() || options.host.empty() || options.port == 0) {
        error = "--a2l, --host and --port are required";
        return std::nullopt;
    }
    return options;
}

std::string QualifiedName(const SymbolInfo& symbol) {
    return symbol.module_name + "::" + symbol.name;
}

std::string KindName(const SymbolInfo& symbol) {
    if (symbol.kind == SymbolKind::Measurement) return "MEASUREMENT";
    if (symbol.kind == SymbolKind::Structure) return "STRUCTURE";
    switch (symbol.characteristic_type) {
        case CharacteristicType::Value: return "CHARACTERISTIC/VALUE";
        case CharacteristicType::Curve: return "CHARACTERISTIC/CURVE";
        case CharacteristicType::Map: return "CHARACTERISTIC/MAP";
        case CharacteristicType::ValBlk: return "CHARACTERISTIC/VAL_BLK";
        case CharacteristicType::Ascii: return "CHARACTERISTIC/ASCII";
        default: return "CHARACTERISTIC";
    }
}

std::string PhysicalToString(const PhysicalValue& value) {
    return std::visit([](const auto& item) {
        std::ostringstream out;
        if constexpr (std::is_same_v<std::decay_t<decltype(item)>, std::string>) {
            out << item;
        } else if constexpr (std::is_same_v<std::decay_t<decltype(item)>, bool>) {
            out << (item ? "true" : "false");
        } else {
            out << std::setprecision(12) << item;
        }
        return out.str();
    }, value);
}

bool ParsePhysical(std::string_view text, PhysicalValue& value) {
    if (text.empty()) return false;
    const bool integer_syntax = text.find_first_of(".eE") == std::string_view::npos;
    if (integer_syntax) {
        if (text.front() == '-') {
            std::int64_t parsed = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
            value = parsed;
            return true;
        }
        std::uint64_t parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
        value = parsed;
        return true;
    }
    std::string copy(text);
    char* end = nullptr;
    const double parsed = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}

struct ListedSymbol {
    SymbolInfo info;
};

class CliApp {
public:
    CliApp(const Options& options, const calmcar::xcp::a2l::IA2lDatabase& database,
           XcpMaster& master)
        : options_(options), database_(database), master_(master) {}

    ~CliApp() { StopWatch(); }

    void Run() {
        std::string line;
        while (true) {
            {
                std::lock_guard lock(output_mutex_);
                std::cout << "xcp> " << std::flush;
            }
            if (!std::getline(std::cin, line)) break;
            if (!Execute(line)) break;
        }
        StopWatch();
    }

private:
    bool Execute(const std::string& line) {
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command.empty()) return true;
        if (command == "quit" || command == "exit") return false;
        if (command == "help") {
            PrintHelp();
        } else if (command == "list") {
            std::string pattern;
            input >> pattern;
            List(pattern);
        } else if (command == "more") {
            ShowPage(false);
        } else if (command == "info") {
            std::string name;
            input >> name;
            Info(name);
        } else if (command == "add") {
            std::string name;
            input >> name;
            Add(name);
        } else if (command == "remove") {
            std::string name;
            input >> name;
            Remove(name);
        } else if (command == "read") {
            std::string name;
            input >> name;
            Read(name);
        } else if (command == "watch") {
            StartWatch();
        } else if (command == "stop") {
            StopWatch();
        } else if (command == "write") {
            std::string name, physical;
            input >> name >> physical;
            Write(name, physical);
        } else {
            Say("未知命令；输入 help 查看用法。");
        }
        return true;
    }

    void PrintHelp() const {
        Say("命令：list [pattern|all] | more | info <name> | add/remove <name> | read <name> | watch | stop | write <name> <value> | quit");
        Say("list 只浏览 A2L；watch 读取已添加变量，stop + Enter 停止。");
    }

    void List(const std::string& pattern) {
        auto count = database_.Count();
        if (!count.HasValue()) {
            Say("A2L Count 失败: " + count.ErrorInfo().message);
            return;
        }
        std::string query = pattern.empty() || pattern == "all" ? "*" : pattern;
        if (query != "*" && query.find_first_of("*?") == std::string::npos) {
            query = "*" + query + "*";
        }
        auto result = database_.Search(query, std::max<std::size_t>(1, count.Value()));
        if (!result.HasValue()) {
            Say("A2L Search 失败: " + result.ErrorInfo().message);
            return;
        }
        listing_.clear();
        for (const auto& symbol : result.Value()) {
            if (!options_.module.empty() && symbol.module_name != options_.module) continue;
            listing_.push_back({symbol});
        }
        page_ = 0;
        list_all_ = pattern == "all";
        Say("命中 " + std::to_string(listing_.size()) + " 个符号；list 不读取实时值。");
        ShowPage(list_all_);
    }

    void ShowPage(bool all) {
        constexpr std::size_t kPageSize = 40;
        if (listing_.empty()) {
            Say("没有可显示的结果；先执行 list [pattern|all]。");
            return;
        }
        const std::size_t end = all ? listing_.size() : std::min(listing_.size(), page_ + kPageSize);
        for (; page_ < end; ++page_) {
            const auto& symbol = listing_[page_].info;
            const auto read_reason = calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
            const auto write_reason = calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
            std::ostringstream row;
            row << QualifiedName(symbol) << " | " << KindName(symbol)
                << " | read=" << (read_reason.empty() ? "候选" : "不支持(" + read_reason + ")")
                << " | write=" << (write_reason.empty() ? "候选" : "不支持(" + write_reason + ")");
            Say(row.str());
        }
        if (page_ < listing_.size()) Say("还有结果；输入 more 翻页。");
        else Say("列表结束。");
    }

    void Info(const std::string& name) {
        auto found = database_.Find(name);
        if (!found.HasValue()) { Say("Find 失败: " + found.ErrorInfo().message); return; }
        const auto& s = found.Value();
        std::ostringstream out;
        out << QualifiedName(s) << " | " << KindName(s) << " | address=0x"
            << std::hex << s.xcp_address << " ext=" << std::dec
            << static_cast<unsigned>(s.address_extension) << " bytes="
            << static_cast<unsigned>(s.element_size_bytes) << " unit=" << s.conversion.unit
            << " read=" << (calmcar::xcp::tools::SymbolReadUnsupportedReason(s).empty() ? "候选" : calmcar::xcp::tools::SymbolReadUnsupportedReason(s))
            << " write=" << (calmcar::xcp::tools::SymbolWriteUnsupportedReason(s).empty() ? "候选" : calmcar::xcp::tools::SymbolWriteUnsupportedReason(s));
        Say(out.str());
    }

    void Add(const std::string& name) {
        if (name.empty()) { Say("用法: add <module::name>"); return; }
        auto found = database_.Find(name);
        if (!found.HasValue()) { Say("Find 失败: " + found.ErrorInfo().message); return; }
        if (!calmcar::xcp::tools::SymbolReadUnsupportedReason(found.Value()).empty()) { Say("不能观测: " + calmcar::xcp::tools::SymbolReadUnsupportedReason(found.Value())); return; }
        auto canonical = QualifiedName(found.Value());
        {
            std::lock_guard lock(watched_mutex_);
            if (std::find(watched_.begin(), watched_.end(), canonical) == watched_.end()) {
                watched_.push_back(canonical);
            }
        }
        Say("已加入轮询: " + canonical);
    }

    void Remove(const std::string& name) {
        auto found = database_.Find(name);
        if (!found.HasValue()) { Say("Find 失败: " + found.ErrorInfo().message); return; }
        const auto canonical = QualifiedName(found.Value());
        bool removed = false;
        {
            std::lock_guard lock(watched_mutex_);
            const auto old_size = watched_.size();
            std::erase(watched_, canonical);
            removed = old_size != watched_.size();
        }
        Say(removed ? "已移除: " + canonical : "该变量不在观测列表。");
    }

    void Read(const std::string& name) {
        auto found = database_.Find(name);
        if (!found.HasValue()) { Say("Find 失败: " + found.ErrorInfo().message); return; }
        auto value = ReadPhysical(found.Value());
        Say(value ? QualifiedName(found.Value()) + " = " + PhysicalToString(*value) + " " + found.Value().conversion.unit
                  : "读取失败或不支持: " + QualifiedName(found.Value()));
    }

    std::optional<PhysicalValue> ReadPhysical(const SymbolInfo& symbol) {
        const auto reason = calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
        if (!reason.empty()) { Say("不支持读取 " + QualifiedName(symbol) + ": " + reason); return std::nullopt; }
        const auto session = master_.GetSessionParameters();
        const auto ag = static_cast<std::size_t>(session.connect.address_granularity);
        if (ag == 0 || symbol.element_size_bytes % ag != 0) {
            Say("地址粒度与符号宽度不兼容: " + QualifiedName(symbol)); return std::nullopt;
        }
        if (symbol.byte_order != (session.connect.byte_order == calmcar::xcp::ByteOrder::Intel
                ? calmcar::xcp::a2l::ByteOrder::MsbLast
                : calmcar::xcp::a2l::ByteOrder::MsbFirst)) {
            Say("A2L 与 Slave 字节序不一致，拒绝读写: " + QualifiedName(symbol)); return std::nullopt;
        }
        try {
            Bytes raw;
            {
                std::lock_guard command_lock(command_mutex_);
                raw = master_.ReadMemoryBytes(static_cast<Address>(symbol.xcp_address),
                    static_cast<AddressExtension>(symbol.address_extension),
                    static_cast<ByteCount>(symbol.element_size_bytes));
            }
            auto physical = database_.ToPhysical(QualifiedName(symbol), raw);
            if (!physical.HasValue()) {
                Say("物理值换算失败: " + physical.ErrorInfo().message); return std::nullopt;
            }
            return physical.Value();
        } catch (const std::exception& e) {
            Say("XCP 读取错误: " + std::string(e.what()));
            return std::nullopt;
        }
    }

    void Write(const std::string& name, const std::string& value_text) {
        if (name.empty() || value_text.empty()) { Say("用法: write <module::name> <physical-value>"); return; }
        auto found = database_.Find(name);
        if (!found.HasValue()) { Say("Find 失败: " + found.ErrorInfo().message); return; }
        const auto symbol = found.Value();
        const auto reason = calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
        if (!reason.empty()) { Say("拒绝写入: " + reason); return; }
        PhysicalValue target;
        if (!ParsePhysical(value_text, target)) { Say("目标值必须是有限数值。"); return; }
        auto encoded = database_.FromPhysical(QualifiedName(symbol), target);
        if (!encoded.HasValue()) { Say("目标值不可编码: " + encoded.ErrorInfo().message); return; }
        const auto session = master_.GetSessionParameters();
        const auto ag = static_cast<std::size_t>(session.connect.address_granularity);
        if (ag == 0 || encoded.Value().size() != symbol.element_size_bytes || encoded.Value().size() % ag != 0) {
            Say("编码宽度或地址粒度不兼容，拒绝写入。"); return;
        }
        auto current = ReadPhysical(symbol);
        if (!current) return;
        Say("当前值: " + PhysicalToString(*current) + " " + symbol.conversion.unit);
        Say("目标值: " + PhysicalToString(target) + " " + symbol.conversion.unit);
        Say("A2L read_write 不是 ECU 写入许可；确认写入? [y/N]");
        {
            std::lock_guard lock(output_mutex_);
            std::cout << "确认> " << std::flush;
        }
        std::string confirm;
        if (!std::getline(std::cin, confirm) || (confirm != "y" && confirm != "Y")) {
            Say("已取消写入。"); return;
        }
        try {
            {
                std::lock_guard command_lock(command_mutex_);
                master_.WriteMemoryBytes(static_cast<Address>(symbol.xcp_address),
                    static_cast<AddressExtension>(symbol.address_extension), encoded.Value());
            }
            auto actual = ReadPhysical(symbol);
            if (actual) Say("写入后回读: " + PhysicalToString(*actual) + " " + symbol.conversion.unit);
            else Say("写入命令成功，但回读失败；实际值未知。");
        } catch (const std::exception& e) {
            Say("XCP 写入被拒绝或失败: " + std::string(e.what()));
        }
    }

    void StartWatch() {
        if (watching_.load()) { Say("轮询已经运行。"); return; }
        if (watched_.empty()) { Say("观测列表为空；先用 add <name> 添加变量。"); return; }
        watching_.store(true);
        watch_thread_ = std::thread([this] {
            Say("轮询已启动；输入 stop 并按 Enter 停止。");
            while (watching_.load()) {
                const auto cycle_start = std::chrono::steady_clock::now();
                std::vector<std::string> current_watch;
                {
                    std::lock_guard lock(watched_mutex_);
                    current_watch = watched_;
                }
                for (const auto& name : current_watch) {
                    if (!watching_.load()) break;
                    auto found = database_.Find(name);
                    if (!found.HasValue()) { Say(name + " | 查找失败"); continue; }
                    auto value = ReadPhysical(found.Value());
                    const auto now = std::chrono::system_clock::now();
                    const auto time = std::chrono::system_clock::to_time_t(now);
                    std::tm tm{};
#if defined(_WIN32)
                    localtime_s(&tm, &time);
#else
                    localtime_r(&time, &tm);
#endif
                    std::ostringstream line;
                    line << std::put_time(&tm, "%H:%M:%S") << " " << name << " = ";
                    if (value) line << PhysicalToString(*value) << " " << found.Value().conversion.unit;
                    else line << "<读取失败>";
                    Say(line.str());
                }
                std::unique_lock lock(wait_mutex_);
                wait_cv_.wait_until(lock, cycle_start + options_.interval,
                                   [this] { return !watching_.load(); });
            }
            Say("轮询已停止。");
        });
        Say("轮询开始（周期目标 " + std::to_string(options_.interval.count()) + " ms）。");
    }

    void StopWatch() {
        if (!watching_.exchange(false)) {
            if (watch_thread_.joinable()) watch_thread_.join();
            return;
        }
        wait_cv_.notify_all();
        if (watch_thread_.joinable()) watch_thread_.join();
    }

    void Say(const std::string& text) const {
        std::lock_guard lock(output_mutex_);
        std::cout << text << '\n' << std::flush;
    }

    const Options& options_;
    const calmcar::xcp::a2l::IA2lDatabase& database_;
    XcpMaster& master_;
    mutable std::mutex output_mutex_;
    std::mutex command_mutex_;
    std::mutex watched_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::atomic<bool> watching_{false};
    std::thread watch_thread_;
    std::vector<std::string> watched_;
    std::vector<ListedSymbol> listing_;
    std::size_t page_{0};
    bool list_all_{false};
};

}  // namespace

int main(int argc, char** argv) {
    bool help = false;
    std::string error;
    auto options = ParseOptions(argc, argv, help, error);
    if (help) { PrintUsage(std::cout); return 0; }
    if (!options) {
        std::cerr << "参数错误: " << error << '\n';
        PrintUsage(std::cerr);
        return 2;
    }

    auto loaded = A2lBridge::Load(options->a2l_path);
    if (!loaded.HasValue()) {
        std::cerr << "A2L 加载失败: " << loaded.ErrorInfo().message << '\n';
        return 1;
    }
    auto bridge = std::move(loaded).Value();
    const auto* database = bridge->Database();
    if (database == nullptr) {
        std::cerr << "A2L 数据库未就绪。\n";
        return 1;
    }
    auto count = database->Count();
    if (!count.HasValue()) {
        std::cerr << "A2L 符号计数失败: " << count.ErrorInfo().message << '\n';
        return 1;
    }
    std::cout << "A2L 加载成功: " << options->a2l_path << " | " << count.Value() << " 个符号\n";
    for (const auto& warning : bridge->ListLoadWarnings()) {
        std::cout << "A2L 告警: " << warning.message << '\n';
    }

    calmcar::xcp::UdpTransportConfig config;
    config.remote_host = options->host;
    config.remote_port = options->port;
    try {
        auto transport = std::make_unique<calmcar::xcp::UdpTransport>(std::move(config));
        XcpMaster master(std::move(transport));
        master.Connect();
        std::cout << "XCP on UDP 已连接: " << options->host << ':' << options->port << '\n';
        CliApp app(*options, *database, master);
        app.Run();
        master.Disconnect();
    } catch (const std::exception& e) {
        std::cerr << "XCP 连接/运行失败: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
