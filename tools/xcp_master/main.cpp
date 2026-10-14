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
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <ctime>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "calibration_policy.hpp"
#include "tui_model.hpp"
#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/measurement/measurement_planner.hpp"
#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/udp_transport.hpp"
#include "a2l/a2l_measurement_database.hpp"
#include "a2l/measurement_runtime_profile.hpp"
#include "libxcp/xcp_master.hpp"
#include "symbol_policy.hpp"

namespace {

/** @brief 判断标准输入和输出是否连接到交互终端。 */
bool IsInteractiveTerminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
}

using calmcar::xcp::A2lEventMetadata;
using calmcar::xcp::Address;
using calmcar::xcp::AddressExtension;
using calmcar::xcp::ApplyMeasurementRuntimeProfile;
using calmcar::xcp::BuildGenericMeasurementRuntimeProfile;
using calmcar::xcp::BuildXcpliteMeasurementRuntimeProfile;
using calmcar::xcp::ByteCount;
using calmcar::xcp::Bytes;
using calmcar::xcp::IdentificationFieldType;
using calmcar::xcp::MeasurementFrame;
using calmcar::xcp::MeasurementPlanner;
using calmcar::xcp::MeasurementSession;
using calmcar::xcp::XcpliteMeasurementRuntimeProfile;
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
    std::string mode{"poll"};
    std::string daq_profile{"generic"};
    bool interval_explicit{false};
    bool daq_profile_explicit{false};
};

void PrintUsage(std::ostream& out) {
    out << "Usage: xcp_master --a2l <path> --host <IPv4> --port <1..65535> "
           "[--module <name>] [--mode poll|daq] [--interval-ms <ms>] "
           "[--daq-profile generic|xcplite]\n"
           "       xcp_master --help\n"
           "poll: --interval-ms is the target polling cycle (default 500 ms).\n"
           "daq: event-driven; --interval-ms is invalid; profile defaults to "
           "generic.\n";
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) return false;
    std::uint64_t parsed = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (parsed >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
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
            if (!ParseUnsigned(value, parsed) || parsed == 0 ||
                parsed > 65535) {
                error = "--port must be in range 1..65535";
                return std::nullopt;
            }
            options.port = static_cast<std::uint16_t>(parsed);
        } else if (key == "--module") {
            options.module = value;
        } else if (key == "--interval-ms") {
            std::uint64_t parsed = 0;
            if (!ParseUnsigned(value, parsed) || parsed == 0 ||
                parsed > static_cast<std::uint64_t>(
                             std::chrono::milliseconds::max().count())) {
                error = "--interval-ms must be a positive integer";
                return std::nullopt;
            }
            options.interval = std::chrono::milliseconds(parsed);
            options.interval_explicit = true;
        } else if (key == "--mode") {
            if (value != "poll" && value != "daq") {
                error = "--mode must be poll or daq";
                return std::nullopt;
            }
            options.mode = value;
        } else if (key == "--daq-profile") {
            if (value != "generic" && value != "xcplite") {
                error = "--daq-profile must be generic or xcplite";
                return std::nullopt;
            }
            options.daq_profile = value;
            options.daq_profile_explicit = true;
        } else {
            error = "unknown option: " + std::string(key);
            return std::nullopt;
        }
    }
    if (options.mode == "daq" && options.interval_explicit) {
        error =
            "--interval-ms controls polling only and conflicts with --mode daq";
        return std::nullopt;
    }
    if (options.mode == "poll" && options.daq_profile_explicit) {
        error = "--daq-profile is valid only with --mode daq";
        return std::nullopt;
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
        case CharacteristicType::Value:
            return "CHARACTERISTIC/VALUE";
        case CharacteristicType::Curve:
            return "CHARACTERISTIC/CURVE";
        case CharacteristicType::Map:
            return "CHARACTERISTIC/MAP";
        case CharacteristicType::ValBlk:
            return "CHARACTERISTIC/VAL_BLK";
        case CharacteristicType::Ascii:
            return "CHARACTERISTIC/ASCII";
        default:
            return "CHARACTERISTIC";
    }
}

std::string FormatEvents(const std::vector<std::uint16_t>& events) {
    if (events.empty()) return "无";
    std::ostringstream out;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i != 0U) out << ',';
        out << events[i];
    }
    return out.str();
}

std::string PhysicalToString(const PhysicalValue& value) {
    return std::visit(
        [](const auto& item) {
            std::ostringstream out;
            if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                         std::string>) {
                out << item;
            } else if constexpr (std::is_same_v<std::decay_t<decltype(item)>,
                                                bool>) {
                out << (item ? "true" : "false");
            } else {
                out << std::setprecision(12) << item;
            }
            return out.str();
        },
        value);
}

bool ParsePhysical(std::string_view text, PhysicalValue& value) {
    if (text.empty()) return false;
    const bool integer_syntax =
        text.find_first_of(".eE") == std::string_view::npos;
    if (integer_syntax) {
        if (text.front() == '-') {
            std::int64_t parsed = 0;
            const auto result =
                std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc{} ||
                result.ptr != text.data() + text.size())
                return false;
            value = parsed;
            return true;
        }
        std::uint64_t parsed = 0;
        const auto result =
            std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            return false;
        value = parsed;
        return true;
    }
    std::string copy(text);
    char* end = nullptr;
    const double parsed = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

struct ListedSymbol {
    SymbolInfo info;
};

class CliApp {
public:
    CliApp(const Options& options,
           const calmcar::xcp::a2l::IA2lDatabase& database,
           calmcar::xcp::IMeasurementDatabase& measurement_database,
           calmcar::xcp::MeasurementSession& measurement_session,
           XcpMaster& master,
           std::vector<calmcar::xcp::A2lEventMetadata> events)
        : options_(options),
          database_(database),
          measurement_database_(measurement_database),
          measurement_session_(measurement_session),
          master_(master),
          events_(std::move(events)) {}

    ~CliApp() { StopWatch(); }

    /**
     * @brief 创建并运行 FTXUI 分栏界面，后台线程负责执行 XCP 命令。
     */
    void Run() {
        using namespace ftxui;
        auto screen = ScreenInteractive::Fullscreen();
        screen_ = &screen;

        InputOption command_options;
        command_options.placeholder = "输入命令，help 查看帮助";
        command_options.on_enter = [this] {
            const std::string command = command_text_;
            command_history_.Add(command);
            command_text_.clear();
            SubmitCommand(command);
        };
        auto command_input = Input(&command_text_, command_options);

        InputOption search_options;
        search_options.placeholder = "按名称筛选可写标量 CHARACTERISTIC/VALUE";
        search_options.on_enter = [this] {
            SubmitCommand("find " + calibration_filter_);
        };
        auto search_input = Input(&calibration_filter_, search_options);
        auto symbol_menu = Menu(&calibration_labels_, &calibration_selected_);

        InputOption target_options;
        target_options.placeholder = "目标物理值";
        auto target_input = Input(&calibration_target_, target_options);
        auto read_button =
            Button("读取所选", [this] { SubmitCalibrationRead(); });
        auto write_button =
            Button("写入所选…", [this] { SubmitCalibrationWrite(); });
        auto watch_button =
            Button("启动观测", [this] { SubmitCommand("watch"); });
        auto stop_button = Button("停止", [this] { SubmitCommand("stop"); });
        auto quit_button = Button("退出", [this] { SubmitCommand("quit"); });

        auto view_menu = Menu(&view_labels_, &view_selected_);
        auto root = Container::Vertical(
            {view_menu, search_input, symbol_menu, target_input, read_button,
             write_button, watch_button, stop_button, command_input,
             quit_button});
        root->SetActiveChild(command_input);
        auto renderer = Renderer(root, [&] {
            std::lock_guard lock(output_mutex_);
            std::size_t watched_count = 0;
            {
                std::lock_guard watched_lock(watched_mutex_);
                watched_count = watched_.size();
            }
            Elements rows;
            const auto& source = view_selected_ == 0
                                     ? observation_model_.Lines()
                                 : view_selected_ == 1 ? calibration_lines_
                                 : view_selected_ == 2 ? result_lines_.Lines()
                                                       : log_lines_.Lines();
            const std::size_t start =
                source.size() > 18U ? source.size() - 18U : 0U;
            for (std::size_t i = start; i < source.size(); ++i)
                rows.push_back(text(source[i]));
            if (rows.empty()) rows.push_back(text("等待操作…"));
            Elements footer{
                hbox({text("命令 > "), command_input->Render()}),
                hbox({watch_button->Render(), text("  "), stop_button->Render(),
                      text("  "), quit_button->Render()}),
                text("F2 切换视图 | ↑↓ 浏览命令历史 | F5 停止 | Tab 切换焦点 | "
                     "Enter 执行 | Esc 返回命令框")};
            if (confirmation_gate_.Pending()) {
                footer.push_back(text("确认操作：Y 确认，N / Esc 取消") | bold |
                                 color(Color::Yellow));
            }
            const std::string run_state = daq_running_.load() ? "DAQ 运行中"
                                          : watching_.load()  ? "POLL 运行中"
                                                              : "已停止";
            const std::string mode_state =
                options_.mode == "daq"
                    ? "DAQ/" + options_.daq_profile
                    : "POLL/目标 " + std::to_string(options_.interval.count()) +
                          " ms";
            return vbox(
                       {text("XCP Master | 观测 | 变量标定 | 命令结果 | 日志") |
                            bold,
                        text("UDP " + options_.host + ":" +
                             std::to_string(options_.port) + " | " +
                             mode_state + " | " + run_state),
                        text("A2L " + options_.a2l_path +
                             (options_.module.empty()
                                  ? ""
                                  : " | MODULE " + options_.module) +
                             " | 观测变量 " + std::to_string(watched_count)),
                        view_menu->Render(), separator(),
                        view_selected_ == 1
                            ? vbox({text("变量筛选：") | bold,
                                    search_input->Render(),
                                    text("可写标量变量：") | bold,
                                    symbol_menu->Render(),
                                    hbox({text("目标值："),
                                          target_input->Render()}),
                                    hbox({read_button->Render(), text("  "),
                                          write_button->Render()}),
                                    text(SelectedCalibrationSummary()),
                                    text("当前值：" + calibration_current_),
                                    text("状态：" + calibration_status_),
                                    separator(), vbox(std::move(rows))})
                            : vbox(std::move(rows)),
                        separator(), vbox(std::move(footer))}) |
                   border;
        });
        auto ui = CatchEvent(renderer, [this, command_input,
                                        view_menu](Event event) {
            if (confirmation_gate_.Pending()) {
                if (event == Event::F5) {
                    ResolveConfirmation(false);
                    SubmitCommand("stop");
                    return true;
                }
                if (event == Event::Escape) {
                    ResolveConfirmation(false);
                    return true;
                }
                if (event.is_character()) {
                    const auto decision =
                        calmcar::xcp::tools::ConfirmationDecision(
                            event.character());
                    if (decision.has_value()) {
                        ResolveConfirmation(*decision);
                        return true;
                    }
                }
                return true;
            }
            if (event == Event::F5) {
                SubmitCommand("stop");
                return true;
            }
            if (command_input->Focused() && event == Event::ArrowUp) {
                if (auto previous = command_history_.Previous(command_text_))
                    command_text_ = std::move(*previous);
                return true;
            }
            if (command_input->Focused() && event == Event::ArrowDown) {
                if (auto next = command_history_.Next())
                    command_text_ = std::move(*next);
                return true;
            }
            if (event == Event::F2) {
                view_selected_ = calmcar::xcp::tools::NextViewIndex(
                    view_selected_, view_labels_.size());
                view_menu->TakeFocus();
                return true;
            }
            if (event == Event::Escape) {
                command_input->TakeFocus();
                return true;
            }
            return false;
        });

        worker_stop_.store(false);
        command_worker_ = std::thread([this] { CommandLoop(); });
        screen_->Loop(ui);
        worker_stop_.store(true);
        command_cv_.notify_all();
        ResolveConfirmation(false);
        if (command_worker_.joinable()) command_worker_.join();
        StopWatch();
        screen_ = nullptr;
    }

private:
    /** @brief 将命令提交到后台队列，避免 XCP 操作阻塞界面线程。 */
    void SubmitCommand(const std::string& command) {
        if (command.empty()) return;
        const bool control =
            command == "stop" || command == "quit" || command == "exit";
        bool rejected = false;
        {
            std::lock_guard lock(command_queue_mutex_);
            if (!control && (worker_busy_.load() || !command_queue_.empty())) {
                rejected = true;
            } else if (control) {
                command_queue_.push_front(command);
            } else {
                command_queue_.push_back(command);
            }
        }
        if (rejected) {
            Say("操作正在执行；普通命令不排队，请稍后重试。");
            return;
        }
        if (command == "stop" || command == "quit" || command == "exit") {
            watching_.store(false);
            wait_cv_.notify_all();
            if (confirmation_gate_.Pending()) ResolveConfirmation(false);
        }
        command_cv_.notify_one();
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 后台串行执行命令和需要确认的操作。 */
    void CommandLoop() {
        while (true) {
            std::string command;
            {
                std::unique_lock lock(command_queue_mutex_);
                command_cv_.wait(lock, [this] {
                    return worker_stop_.load() || !command_queue_.empty();
                });
                if (command_queue_.empty() && worker_stop_.load()) return;
                command = std::move(command_queue_.front());
                command_queue_.pop_front();
                worker_busy_.store(true);
            }
            bool keep_running = true;
            try {
                keep_running = Execute(command);
            } catch (const std::exception& e) {
                Say("命令执行异常: " + std::string(e.what()));
            } catch (...) {
                Say("命令执行发生未知异常。");
            }
            worker_busy_.store(false);
            if (!keep_running) {
                if (screen_) screen_->Exit();
                return;
            }
        }
    }

    /** @brief 请求界面显示确认并等待用户明确选择。 */
    bool Confirm(const std::string& prompt) {
        Say(prompt);
        confirmation_gate_.Begin();
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
        return confirmation_gate_.Wait(worker_stop_);
    }

    /** @brief 将界面的确认/取消选择交回命令线程。 */
    void ResolveConfirmation(bool accepted) {
        confirmation_gate_.Resolve(accepted);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /**
     * @brief 返回当前选中的标定变量名称及保守写入资格。
     * @return 当前候选描述；未选择时返回提示文本。
     */
    std::string SelectedCalibrationSummary() const {
        if (calibration_selected_ >= calibration_labels_.size())
            return "尚未选择标定变量";
        return "已选：" + calibration_labels_[calibration_selected_];
    }

    /** @brief 更新标定视图的操作状态。 */
    void SetCalibrationStatus(const std::string& status) {
        std::lock_guard lock(output_mutex_);
        calibration_status_ = status;
    }

    /** @brief 读取当前标定视图所选符号并提交安全读操作。 */
    void SubmitCalibrationRead() {
        std::string name;
        {
            std::lock_guard lock(output_mutex_);
            if (calibration_selected_ < calibration_names_.size())
                name = calibration_names_[calibration_selected_];
        }
        if (!name.empty()) SubmitCommand("calread " + name);
    }

    /** @brief 将标定表单内容交给与命令行共用的写入流程。 */
    void SubmitCalibrationWrite() {
        std::string name;
        {
            std::lock_guard lock(output_mutex_);
            if (calibration_selected_ < calibration_names_.size())
                name = calibration_names_[calibration_selected_];
        }
        if (name.empty() || calibration_target_.empty()) {
            Say("请选择标定变量并填写目标物理值。");
            return;
        }
        SubmitCommand("calwrite " + name + "\t" + calibration_target_);
    }

    /** @brief 在 A2L 中筛选符合保守写入策略的标量标定项。 */
    void FindCalibrationCandidates(const std::string& pattern) {
        auto count = database_.Count();
        if (!count.HasValue()) {
            Say("A2L Count 失败: " + count.ErrorInfo().message);
            return;
        }
        std::string query = pattern.empty() ? "*" : pattern;
        if (query.find_first_of("*?") == std::string::npos)
            query = "*" + query + "*";
        auto result =
            database_.Search(query, std::max<std::size_t>(1, count.Value()));
        if (!result.HasValue()) {
            Say("A2L Search 失败: " + result.ErrorInfo().message);
            return;
        }
        std::vector<std::string> names;
        std::vector<std::string> labels;
        for (const auto& symbol : result.Value()) {
            if (symbol.kind != SymbolKind::Characteristic ||
                symbol.characteristic_type != CharacteristicType::Value ||
                (!options_.module.empty() &&
                 symbol.module_name != options_.module))
                continue;
            const auto reason =
                calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
            const auto name = QualifiedName(symbol);
            names.push_back(name);
            labels.push_back(
                name + (reason.empty() ? " | 可申请写入" : " | " + reason));
        }
        const std::size_t candidate_count = names.size();
        auto update_candidates = [this, names = std::move(names),
                                  labels = std::move(labels)]() mutable {
            std::lock_guard lock(output_mutex_);
            calibration_names_ = std::move(names);
            calibration_labels_ = std::move(labels);
            calibration_selected_ = 0;
            calibration_target_.clear();
            calibration_current_ = "尚未读取";
            calibration_status_ = calibration_names_.empty()
                                      ? "没有匹配项"
                                      : "请选择变量并读取当前值";
            calibration_lines_.clear();
            for (const auto& label : calibration_labels_)
                calibration_lines_.push_back(label);
        };
        if (screen_)
            screen_->Post(ftxui::Closure(std::move(update_candidates)));
        else
            update_candidates();
        Say("找到 " + std::to_string(candidate_count) +
            " 个标量 CHARACTERISTIC/VALUE；仅符合现有策略的项允许申请写入。");
    }

    bool Execute(const std::string& line) {
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command.empty()) return true;
        if (command == "quit" || command == "exit") return false;
        if (command == "find") {
            std::string pattern;
            input >> pattern;
            FindCalibrationCandidates(pattern);
        } else if (command == "calwrite") {
            const auto separator = line.find('\t');
            if (separator == std::string::npos) {
                Say("标定表单参数无效。");
            } else {
                Write(line.substr(9, separator - 9), line.substr(separator + 1),
                      true);
            }
        } else if (command == "calread") {
            std::string name;
            input >> name;
            ReadCalibration(name);
        } else if (command == "help") {
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
        Say("命令：list [pattern|all] | more | info <name> | add/remove <name> "
            "| read <name> | watch | stop | write <name> <value> | find "
            "[pattern] | quit");
        Say("mode=" + options_.mode +
            (options_.mode == "poll"
                 ? "；watch 按目标周期 " +
                       std::to_string(options_.interval.count()) +
                       " ms 主动读取。"
                 : "；watch 使用事件驱动 DAQ，profile=" + options_.daq_profile +
                       "。"));
        Say("DAQ 只支持唯一 FIXED_EVENT_LIST 的 MEASUREMENT；运行中禁止 "
            "read/write/add/remove。stop + Enter 停止。");
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
        auto result =
            database_.Search(query, std::max<std::size_t>(1, count.Value()));
        if (!result.HasValue()) {
            Say("A2L Search 失败: " + result.ErrorInfo().message);
            return;
        }
        listing_.clear();
        for (const auto& symbol : result.Value()) {
            if (!options_.module.empty() &&
                symbol.module_name != options_.module)
                continue;
            listing_.push_back({symbol});
        }
        page_ = 0;
        list_all_ = pattern == "all";
        Say("命中 " + std::to_string(listing_.size()) +
            " 个符号；list 不读取实时值。");
        ShowPage(list_all_);
    }

    void ShowPage(bool all) {
        constexpr std::size_t kPageSize = 40;
        if (listing_.empty()) {
            Say("没有可显示的结果；先执行 list [pattern|all]。");
            return;
        }
        const std::size_t end =
            all ? listing_.size()
                : std::min(listing_.size(), page_ + kPageSize);
        for (; page_ < end; ++page_) {
            const auto& symbol = listing_[page_].info;
            const auto read_reason =
                calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
            const auto write_reason =
                calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
            std::ostringstream row;
            row << QualifiedName(symbol) << " | " << KindName(symbol)
                << " | fixed-event=" << FormatEvents(symbol.daq_event_fixed)
                << " | read="
                << (read_reason.empty() ? "候选"
                                        : "不支持(" + read_reason + ")")
                << " | write="
                << (write_reason.empty() ? "候选"
                                         : "不支持(" + write_reason + ")");
            Say(row.str());
        }
        if (page_ < listing_.size())
            Say("还有结果；输入 more 翻页。");
        else
            Say("列表结束。");
    }

    void Info(const std::string& name) {
        auto found = database_.Find(name);
        if (!found.HasValue()) {
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        const auto& s = found.Value();
        std::ostringstream out;
        out << QualifiedName(s) << " | " << KindName(s) << " | address=0x"
            << std::hex << s.xcp_address << " ext=" << std::dec
            << static_cast<unsigned>(s.address_extension)
            << " bytes=" << static_cast<unsigned>(s.element_size_bytes)
            << " unit=" << s.conversion.unit
            << " fixed-event=" << FormatEvents(s.daq_event_fixed)
            << " default-event=" << FormatEvents(s.daq_event_default)
            << " available-event=" << FormatEvents(s.daq_event_available)
            << " read="
            << (calmcar::xcp::tools::SymbolReadUnsupportedReason(s).empty()
                    ? "候选"
                    : calmcar::xcp::tools::SymbolReadUnsupportedReason(s))
            << " write="
            << (calmcar::xcp::tools::SymbolWriteUnsupportedReason(s).empty()
                    ? "候选"
                    : calmcar::xcp::tools::SymbolWriteUnsupportedReason(s));
        Say(out.str());
    }

    void Add(const std::string& name) {
        if (DaqRunning()) {
            Say("DAQ 运行期间不能修改观测集；请先 stop。");
            return;
        }
        if (name.empty()) {
            Say("用法: add <module::name>");
            return;
        }
        auto found = database_.Find(name);
        if (!found.HasValue()) {
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        if (!calmcar::xcp::tools::SymbolReadUnsupportedReason(found.Value())
                 .empty()) {
            Say("不能观测: " + calmcar::xcp::tools::SymbolReadUnsupportedReason(
                                   found.Value()));
            return;
        }
        auto canonical = QualifiedName(found.Value());
        bool added = false;
        {
            std::lock_guard lock(watched_mutex_);
            if (std::find(watched_.begin(), watched_.end(), canonical) ==
                watched_.end()) {
                watched_.push_back(canonical);
                added = true;
            }
        }
        if (added) SayObservation(canonical, canonical + " | 待读取");
        Say("已加入观测: " + canonical);
    }

    void Remove(const std::string& name) {
        if (DaqRunning()) {
            Say("DAQ 运行期间不能修改观测集；请先 stop。");
            return;
        }
        auto found = database_.Find(name);
        if (!found.HasValue()) {
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        const auto canonical = QualifiedName(found.Value());
        bool removed = false;
        {
            std::lock_guard lock(watched_mutex_);
            const auto old_size = watched_.size();
            std::erase(watched_, canonical);
            removed = old_size != watched_.size();
        }
        if (removed) {
            {
                std::lock_guard lock(output_mutex_);
                observation_model_.Remove(canonical);
            }
            if (screen_) screen_->PostEvent(ftxui::Event::Custom);
        }
        Say(removed ? "已移除: " + canonical : "该变量不在观测列表。");
    }

    /** @brief 执行标定表单读取并更新其状态显示。 */
    void ReadCalibration(const std::string& name) {
        if (DaqRunning()) {
            {
                std::lock_guard lock(output_mutex_);
                calibration_status_ = "DAQ 运行期间禁止读取，请先 stop";
            }
            Say("DAQ 运行期间不能手动 read；请先 stop。");
            return;
        }
        auto found = database_.Find(name);
        if (!found.HasValue()) {
            {
                std::lock_guard lock(output_mutex_);
                calibration_current_ = "读取失败";
                calibration_status_ = "A2L 查找失败";
            }
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        auto value = ReadPhysical(found.Value());
        {
            std::lock_guard lock(output_mutex_);
            calibration_current_ = value ? PhysicalToString(*value) + " " +
                                               found.Value().conversion.unit
                                         : "读取失败";
            calibration_status_ = value ? "读取成功" : "读取失败；当前值未知";
        }
        Say(value ? QualifiedName(found.Value()) + " = " +
                        PhysicalToString(*value) + " " +
                        found.Value().conversion.unit
                  : "读取失败或不支持: " + QualifiedName(found.Value()));
    }

    /** @brief 执行命令行读操作并显示结果。 */
    void Read(const std::string& name) {
        if (DaqRunning()) {
            Say("DAQ 运行期间不能手动 read；请先 stop。");
            return;
        }
        auto found = database_.Find(name);
        if (!found.HasValue()) {
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        auto value = ReadPhysical(found.Value());
        const auto canonical = QualifiedName(found.Value());
        Say(value ? canonical + " = " + PhysicalToString(*value) + " " +
                        found.Value().conversion.unit
                  : "读取失败或不支持: " + canonical);
    }

    std::optional<PhysicalValue> ReadPhysical(const SymbolInfo& symbol) {
        const auto session = master_.GetSessionParameters();
        const auto ag =
            static_cast<std::size_t>(session.connect.address_granularity);
        const auto reason = calmcar::xcp::tools::ScalarLayoutUnsupportedReason(
            symbol, ag, session.connect.byte_order);
        if (!reason.empty()) {
            Say("不支持读取 " + QualifiedName(symbol) + ": " + reason);
            return std::nullopt;
        }
        try {
            Bytes raw;
            {
                std::lock_guard command_lock(command_mutex_);
                raw = master_.ReadMemoryBytes(
                    static_cast<Address>(symbol.xcp_address),
                    static_cast<AddressExtension>(symbol.address_extension),
                    static_cast<ByteCount>(symbol.element_size_bytes));
            }
            auto physical = database_.ToPhysical(QualifiedName(symbol), raw);
            if (!physical.HasValue()) {
                Say("物理值换算失败: " + physical.ErrorInfo().message);
                return std::nullopt;
            }
            return physical.Value();
        } catch (const std::exception& e) {
            Say("XCP 读取错误: " + std::string(e.what()));
            return std::nullopt;
        }
    }

    void Write(const std::string& name, const std::string& value_text,
               bool from_calibration_view = false) {
        if (DaqRunning()) {
            if (from_calibration_view) {
                std::lock_guard lock(output_mutex_);
                calibration_status_ = "DAQ 运行期间禁止写入，请先 stop";
            }
            Say("DAQ 运行期间不能 write；请先 stop。");
            return;
        }
        if (name.empty() || value_text.empty()) {
            if (from_calibration_view)
                SetCalibrationStatus("目标变量或物理值为空");
            Say("用法: write <module::name> <physical-value>");
            return;
        }
        auto found = database_.Find(name);
        if (!found.HasValue()) {
            if (from_calibration_view) SetCalibrationStatus("A2L 查找失败");
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        const auto symbol = found.Value();
        const auto reason =
            calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
        if (!reason.empty()) {
            if (from_calibration_view)
                SetCalibrationStatus("拒绝写入：" + reason);
            Say("拒绝写入: " + reason);
            return;
        }
        PhysicalValue target;
        if (!ParsePhysical(value_text, target)) {
            if (from_calibration_view)
                SetCalibrationStatus("目标值不是有限数值");
            Say("目标值必须是有限数值。");
            return;
        }
        auto encoded = database_.FromPhysical(QualifiedName(symbol), target);
        if (!encoded.HasValue()) {
            if (from_calibration_view)
                SetCalibrationStatus("目标值无法换算或编码");
            Say("目标值不可编码: " + encoded.ErrorInfo().message);
            return;
        }
        const auto session = master_.GetSessionParameters();
        const auto ag =
            static_cast<std::size_t>(session.connect.address_granularity);
        const auto encoded_reason =
            calmcar::xcp::tools::EncodedScalarUnsupportedReason(
                symbol, encoded.Value().size(), ag);
        if (!encoded_reason.empty()) {
            if (from_calibration_view)
                SetCalibrationStatus("拒绝写入：" + encoded_reason);
            Say(encoded_reason + "，拒绝写入。");
            return;
        }
        auto current = ReadPhysical(symbol);
        if (!current) {
            if (from_calibration_view) SetCalibrationStatus("预读失败，未写入");
            return;
        }
        Say("当前值: " + PhysicalToString(*current) + " " +
            symbol.conversion.unit);
        Say("目标值: " + PhysicalToString(target) + " " +
            symbol.conversion.unit);
        if (from_calibration_view) {
            std::lock_guard lock(output_mutex_);
            calibration_current_ =
                PhysicalToString(*current) + " " + symbol.conversion.unit;
            calibration_status_ = "等待确认；写入默认取消";
        }
        if (!Confirm("A2L read_write 不是 ECU 写入许可；确认写入? [Y/N]")) {
            if (from_calibration_view) {
                std::lock_guard lock(output_mutex_);
                calibration_status_ = "已取消，未发送写入命令";
            }
            Say("已取消写入。");
            return;
        }
        try {
            {
                std::lock_guard command_lock(command_mutex_);
                master_.WriteMemoryBytes(
                    static_cast<Address>(symbol.xcp_address),
                    static_cast<AddressExtension>(symbol.address_extension),
                    encoded.Value());
            }
            auto actual = ReadPhysical(symbol);
            if (from_calibration_view) {
                std::lock_guard lock(output_mutex_);
                calibration_current_ = actual ? PhysicalToString(*actual) +
                                                    " " + symbol.conversion.unit
                                              : "回读失败；实际值未知";
                calibration_status_ = actual ? "写入命令成功，回读已完成"
                                             : "写入命令成功，但回读失败";
            }
            if (actual)
                Say("写入后回读: " + PhysicalToString(*actual) + " " +
                    symbol.conversion.unit);
            else
                Say("写入命令成功，但回读失败；实际值未知。");
        } catch (const std::exception& e) {
            if (from_calibration_view)
                SetCalibrationStatus("写入失败：" + std::string(e.what()));
            Say("XCP 写入被拒绝或失败: " + std::string(e.what()));
        }
    }

    bool DaqRunning() const noexcept { return daq_running_.load(); }

    /** @brief 在观测集合锁内检查规范变量是否仍被选择。 */
    bool IsWatched(const std::string& name) {
        std::lock_guard lock(watched_mutex_);
        return std::find(watched_.begin(), watched_.end(), name) !=
               watched_.end();
    }

    void StartWatch() {
        if (options_.mode == "daq") {
            StartDaqWatch();
            return;
        }
        if (watching_.load()) {
            Say("轮询已经运行。");
            return;
        }
        bool watch_list_empty = false;
        {
            std::lock_guard lock(watched_mutex_);
            watch_list_empty = watched_.empty();
        }
        if (watch_list_empty) {
            Say("观测列表为空；先用 add <name> 添加变量。");
            return;
        }
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
                    if (!found.HasValue()) {
                        if (IsWatched(name))
                            SayObservation(name, name + " | 查找失败");
                        continue;
                    }
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
                    line << std::put_time(&tm, "%H:%M:%S") << " " << name
                         << " = ";
                    if (value)
                        line << PhysicalToString(*value) << " "
                             << found.Value().conversion.unit;
                    else
                        line << "<读取失败>";
                    if (IsWatched(name)) SayObservation(name, line.str());
                }
                std::unique_lock lock(wait_mutex_);
                wait_cv_.wait_until(lock, cycle_start + options_.interval,
                                    [this] { return !watching_.load(); });
            }
            Say("轮询已停止。");
        });
        Say("轮询开始（周期目标 " + std::to_string(options_.interval.count()) +
            " ms）。");
    }

    void StartDaqWatch() {
        if (DaqRunning()) {
            Say("DAQ 已运行。");
            return;
        }
        std::vector<std::string> names;
        {
            std::lock_guard lock(watched_mutex_);
            names = watched_;
        }
        if (names.empty()) {
            Say("观测列表为空；先用 add <name> 添加变量。");
            return;
        }

        std::vector<A2lEventMetadata> selected_events;
        std::vector<SymbolInfo> selected_symbols;
        for (const auto& name : names) {
            auto found = database_.Find(name);
            if (!found.HasValue()) {
                Say(name + " | A2L 查找失败: " + found.ErrorInfo().message);
                return;
            }
            const auto& symbol = found.Value();
            if (symbol.kind != SymbolKind::Measurement) {
                Say(name + " | DAQ 仅支持 MEASUREMENT，不支持标定项。");
                return;
            }
            const auto reason =
                calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
            if (!reason.empty()) {
                Say(name + " | DAQ 不支持: " + reason);
                return;
            }
            auto fixed = symbol.daq_event_fixed;
            std::sort(fixed.begin(), fixed.end());
            fixed.erase(std::unique(fixed.begin(), fixed.end()), fixed.end());
            if (fixed.size() != 1U) {
                Say(name +
                    " | 需要唯一 FIXED_EVENT_LIST；不回退到 "
                    "DEFAULT/AVAILABLE。");
                return;
            }
            const auto event = std::find_if(
                events_.begin(), events_.end(), [&](const auto& item) {
                    return item.channel == fixed.front();
                });
            if (event == events_.end()) {
                Say(name + " | A2L 缺少对应 EVENT_CHANNEL 元数据。");
                return;
            }
            if (std::none_of(selected_events.begin(), selected_events.end(),
                             [&](const auto& item) {
                                 return item.channel == event->channel;
                             }))
                selected_events.push_back(*event);
            selected_symbols.push_back(symbol);
        }

        const auto session = master_.GetSessionParameters();
        std::size_t timestamp_bytes = 0;
        std::size_t identification_bytes = 1;
        std::size_t max_entry = 0;
        std::size_t entry_granularity = 0;
        std::optional<XcpliteMeasurementRuntimeProfile> xcplite_profile;
        if (options_.daq_profile == "generic") {
            auto profile =
                BuildGenericMeasurementRuntimeProfile(master_, selected_events);
            if (!profile.HasValue()) {
                Say("通用 DAQ 能力校验失败: " + profile.ErrorInfo().message);
                return;
            }
            identification_bytes = profile.Value().identification_header_bytes;
            max_entry = profile.Value().max_daq_entry_size_bytes;
            entry_granularity = profile.Value().daq_entry_granularity_bytes;
        } else {
            auto profile =
                BuildXcpliteMeasurementRuntimeProfile(master_, selected_events);
            if (!profile.HasValue()) {
                Say("XCPlite DAQ 能力校验失败: " + profile.ErrorInfo().message);
                return;
            }
            xcplite_profile = profile.Value();
            timestamp_bytes = 4;
            identification_bytes = profile.Value().identification_header_bytes;
        }
        const auto ag =
            static_cast<std::size_t>(session.connect.address_granularity);
        for (const auto& symbol : selected_symbols) {
            if (symbol.element_size_bytes > max_entry && max_entry != 0U) {
                Say(QualifiedName(symbol) + " | 数据宽度超过 DAQ entry 上限。");
                return;
            }
            if (entry_granularity != 0U &&
                (symbol.element_size_bytes % entry_granularity != 0U ||
                 symbol.element_size_bytes % std::max<std::size_t>(ag, 1U) !=
                     0U)) {
                Say(QualifiedName(symbol) +
                    " | 数据宽度与 DAQ/地址粒度不兼容。");
                return;
            }
        }
        MeasurementPlanner planner(measurement_database_,
                                   session.connect.max_dto,
                                   session.connect.address_granularity,
                                   timestamp_bytes, identification_bytes);
        auto planned = planner.Build(names);
        if (!planned.HasValue()) {
            Say("DAQ 本地规划失败: " + planned.ErrorInfo().message);
            return;
        }
        if (options_.daq_profile == "generic" &&
            planned.Value().daq_lists.size() != 1U) {
            Say("通用 Absolute-PID profile 仅支持一个 DAQ list。");
            return;
        }

        Say("警告：开始 DAQ 将发送 FREE_DAQ，清除 Slave 当前全部动态 DAQ "
            "配置（不可恢复）。");
        if (!Confirm("能力查询和本地布局检查已通过。继续? [Y/N]")) {
            Say("已取消；没有发送 FREE_DAQ。");
            return;
        }

        try {
            if (!profile_configured_) {
                if (xcplite_profile) {
                    ApplyMeasurementRuntimeProfile(measurement_session_,
                                                   *xcplite_profile);
                } else {
                    measurement_session_.SetEnvelopeMode(
                        IdentificationFieldType::Absolute, 1);
                    measurement_session_.SetTimestampEnabled(false);
                }
                profile_configured_ = true;
            }
            measurement_session_.Clear();
            for (const auto& name : names) measurement_session_.Add(name);
            measurement_session_.Prepare();
            {
                std::lock_guard lock(frame_mutex_);
                frame_queue_.clear();
                frame_output_stop_ = false;
            }
            frame_output_thread_ = std::thread([this] { FrameOutputLoop(); });
            measurement_session_.Start(
                [this](const MeasurementFrame& frame) { EnqueueFrame(frame); });
            daq_running_.store(true);
            Say("DAQ 已启动；输入 stop + Enter 停止。");
        } catch (const std::exception& e) {
            daq_running_.store(false);
            StopFrameOutput();
            Say("DAQ 启动失败: " + std::string(e.what()));
        }
    }

    void EnqueueFrame(const MeasurementFrame& frame) {
        std::lock_guard lock(frame_mutex_);
        constexpr std::size_t kMaxQueuedFrames = 256;
        if (frame_queue_.size() == kMaxQueuedFrames) frame_queue_.pop_front();
        frame_queue_.push_back(frame);
        frame_cv_.notify_one();
    }

    void FrameOutputLoop() {
        while (true) {
            MeasurementFrame frame;
            {
                std::unique_lock lock(frame_mutex_);
                frame_cv_.wait(lock, [this] {
                    return frame_output_stop_ || !frame_queue_.empty();
                });
                if (frame_queue_.empty() && frame_output_stop_) return;
                frame = std::move(frame_queue_.front());
                frame_queue_.pop_front();
            }
            for (const auto& sample : frame.samples) {
                std::ostringstream line;
                line << "DAQ list=" << frame.daq_list
                     << " ODT=" << static_cast<unsigned>(frame.odt);
                if (frame.timestamp_valid)
                    line << " t=" << frame.timestamp.count() << "ns";
                else if (frame.timestamp_raw != 0U)
                    line << " timestamp_raw=" << frame.timestamp_raw;
                line << " | " << sample.name << '=';
                if (!sample.valid) {
                    line << "<invalid>";
                } else {
                    std::visit(
                        [&](const auto& value) {
                            using T = std::decay_t<decltype(value)>;
                            if constexpr (std::is_same_v<T, std::string>)
                                line << value;
                            else
                                line << std::setprecision(15) << value;
                        },
                        sample.value);
                }
                SayObservation(sample.name, line.str());
            }
        }
    }

    void StopFrameOutput() {
        {
            std::lock_guard lock(frame_mutex_);
            frame_output_stop_ = true;
        }
        frame_cv_.notify_all();
        if (frame_output_thread_.joinable()) frame_output_thread_.join();
    }

    /** @brief 停止采集后保留最后值并更新显示状态。 */
    void MarkObservationsStopped() {
        {
            std::lock_guard lock(output_mutex_);
            observation_model_.MarkAllStopped();
        }
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    void StopWatch() {
        if (options_.mode == "daq") {
            if (measurement_session_.Running()) {
                try {
                    measurement_session_.Stop();
                } catch (const std::exception& e) {
                    Say("DAQ 停止错误: " + std::string(e.what()));
                }
            }
            daq_running_.store(false);
            StopFrameOutput();
            MarkObservationsStopped();
            return;
        }
        if (!watching_.exchange(false)) {
            if (watch_thread_.joinable()) watch_thread_.join();
            MarkObservationsStopped();
            return;
        }
        wait_cv_.notify_all();
        if (watch_thread_.joinable()) watch_thread_.join();
        MarkObservationsStopped();
    }

    /** @brief 将命令反馈写入结果与日志面板，并通知界面重绘。 */
    void Say(const std::string& text) const {
        {
            std::lock_guard lock(output_mutex_);
            result_lines_.Append(text);
            log_lines_.Append(text);
        }
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 按规范变量名更新观测快照并通知界面重绘。 */
    void SayObservation(const std::string& symbol,
                        const std::string& text) const {
        {
            std::lock_guard lock(output_mutex_);
            observation_model_.Update(symbol, text);
        }
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    const Options& options_;
    const calmcar::xcp::a2l::IA2lDatabase& database_;
    calmcar::xcp::IMeasurementDatabase& measurement_database_;
    MeasurementSession& measurement_session_;
    XcpMaster& master_;
    std::vector<A2lEventMetadata> events_;
    mutable std::mutex output_mutex_;
    std::mutex command_mutex_;
    std::mutex watched_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::atomic<bool> watching_{false};
    std::atomic<bool> daq_running_{false};
    bool profile_configured_{false};
    std::thread watch_thread_;
    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    std::deque<MeasurementFrame> frame_queue_;
    bool frame_output_stop_{true};
    std::thread frame_output_thread_;
    std::vector<std::string> watched_;
    std::vector<ListedSymbol> listing_;
    std::size_t page_{0};
    bool list_all_{false};
    mutable calmcar::xcp::tools::LatestObservationModel observation_model_{
        512}; /**< 按符号名保存的最新观测快照。 */
    mutable calmcar::xcp::tools::BoundedLineBuffer result_lines_{
        512}; /**< 命令结果视图的有界显示行。 */
    mutable calmcar::xcp::tools::BoundedLineBuffer log_lines_{
        512}; /**< 日志视图的有界显示行。 */
    mutable std::vector<std::string>
        calibration_lines_; /**< 标定视图的候选摘要行。 */
    std::vector<std::string>
        calibration_names_; /**< 标定菜单对应的规范符号名。 */
    std::vector<std::string>
        calibration_labels_;          /**< 显示名称及保守写入资格说明。 */
    std::string calibration_current_; /**< 当前所选符号最近一次读取的值。 */
    std::string calibration_status_;  /**< 标定读写流程的当前状态。 */
    std::vector<std::string> view_labels_{
        "观测", "变量标定", "命令结果", "日志"}; /**< 顶部显示区的视图标签。 */
    int view_selected_{0};           /**< 当前选中的显示视图索引。 */
    int calibration_selected_{0};    /**< 当前选中的标定候选索引。 */
    std::string calibration_filter_; /**< 标定候选搜索文本。 */
    std::string calibration_target_; /**< 标定目标物理值文本。 */
    std::string command_text_;       /**< 固定命令输入框内容。 */
    calmcar::xcp::tools::CommandHistory command_history_{
        200}; /**< 最多保留 200 条命令历史。 */
    ftxui::ScreenInteractive* screen_{
        nullptr};                    /**< 当前交互界面，供后台事件唤醒使用。 */
    std::mutex command_queue_mutex_; /**< 串行命令队列互斥量。 */
    std::condition_variable command_cv_;    /**< 命令线程等待通知。 */
    std::deque<std::string> command_queue_; /**< 待执行命令队列。 */
    std::thread command_worker_;            /**< 串行执行命令的后台线程。 */
    std::atomic<bool> worker_stop_{false};  /**< 命令线程停止标志。 */
    std::atomic<bool> worker_busy_{
        false}; /**< 当前是否正在处理一条业务命令。 */
    calmcar::xcp::tools::ConfirmationGate
        confirmation_gate_; /**< 单次危险操作的确认状态。 */
};

}  // namespace

int main(int argc, char** argv) {
    bool help = false;
    std::string error;
    auto options = ParseOptions(argc, argv, help, error);
    if (help) {
        PrintUsage(std::cout);
        return 0;
    }
    if (!options) {
        std::cerr << "参数错误: " << error << '\n';
        PrintUsage(std::cerr);
        return 2;
    }
    if (!IsInteractiveTerminal()) {
        std::cerr
            << "xcp_master 交互界面需要交互式终端；当前输入或输出已重定向。\n";
        return 1;
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
    std::cout << "A2L 加载成功: " << options->a2l_path << " | " << count.Value()
              << " 个符号\n";
    for (const auto& warning : bridge->ListLoadWarnings()) {
        std::cout << "A2L 告警: " << warning.message << '\n';
    }

    calmcar::xcp::A2lMeasurementDatabase a2l_measurement_database(*database);
    calmcar::xcp::MeasurementEventBindingCandidates event_bindings;
    auto all_symbols =
        database->Search("*", std::max<std::size_t>(1, count.Value()));
    if (!all_symbols.HasValue()) {
        std::cerr << "A2L DAQ event binding 查询失败: "
                  << all_symbols.ErrorInfo().message << '\n';
        return 1;
    }
    for (const auto& symbol : all_symbols.Value()) {
        if (symbol.kind != SymbolKind::Measurement) continue;
        auto& candidates = event_bindings[QualifiedName(symbol)];
        candidates.assign(symbol.daq_event_fixed.begin(),
                          symbol.daq_event_fixed.end());
    }
    calmcar::xcp::EventBoundMeasurementDatabase event_bound_database(
        a2l_measurement_database, std::move(event_bindings));
    calmcar::xcp::MeasurementSession measurement_session(event_bound_database);

    std::vector<A2lEventMetadata> event_metadata;
    if (const auto* xcp_info = bridge->XcpInfo();
        xcp_info != nullptr && !xcp_info->ambiguous) {
        event_metadata.reserve(xcp_info->event_channels.size());
        for (const auto& event : xcp_info->event_channels) {
            event_metadata.push_back(A2lEventMetadata{
                event.name, event.short_name, event.channel_number,
                event.time_cycle, event.time_unit, event.type,
                event.max_daq_list});
        }
    }

    calmcar::xcp::UdpTransportConfig config;
    config.remote_host = options->host;
    config.remote_port = options->port;
    try {
        auto transport =
            std::make_unique<calmcar::xcp::UdpTransport>(std::move(config));
        XcpMaster master(std::move(transport), {}, &measurement_session);
        master.Connect();
        measurement_session.Bind(master);
        std::cout << "XCP on UDP 已连接: " << options->host << ':'
                  << options->port << '\n';
        CliApp app(*options, *database, event_bound_database,
                   measurement_session, master, std::move(event_metadata));
        app.Run();
        if (measurement_session.Running()) measurement_session.Stop();
        master.Disconnect();
    } catch (const std::exception& e) {
        std::cerr << "XCP 连接/运行失败: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
