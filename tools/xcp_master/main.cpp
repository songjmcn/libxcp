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
#include <cstddef>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

/** @brief 数组详情每页读取的连续元素数量。 */
constexpr std::uint64_t kArrayPageSize = 8U;

/** @brief 判断标准输入和输出是否连接到交互终端。 */
bool IsInteractiveTerminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
}

/** @brief 返回本地时分秒时间戳，用于变量预览中的最后更新时间。 */
std::string LocalTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%H:%M:%S");
    return output.str();
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
using calmcar::xcp::tools::TuiLayoutMetrics;

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
     * @brief 创建四层 FTXUI 界面，后台线程串行执行 XCP 命令。
     */
    void Run() {
        using namespace ftxui;
        auto screen = ScreenInteractive::Fullscreen();
        screen_ = &screen;

        InputOption command_options;
        command_options.placeholder = "输入命令，help 查看帮助";
        command_options.on_enter = [this] {
            const std::string command = command_text_;
            std::istringstream input(command);
            std::string command_name;
            input >> command_name;
            const auto preview_revision =
                command_name == "find" ? std::optional<std::uint64_t>(
                                             preview_input_revision_.Capture())
                                       : std::nullopt;
            const bool accepted =
                SubmitCommand(command, std::nullopt, {}, preview_revision);
            static_cast<void>(command_history_.Commit(command_text_, accepted));
        };
        auto command_input = Input(&command_text_, command_options);

        InputOption search_options;
        search_options.placeholder = "搜索测量量/标量标定量；Enter 搜索";
        search_options.on_change = [this] {
            preview_input_revision_.MarkEdited();
        };
        search_options.on_enter = [this] {
            static_cast<void>(SubmitCommand(
                "find " + (preview_filter_.empty() ? "*" : preview_filter_),
                std::nullopt, {}, preview_input_revision_.Capture()));
        };
        auto search_input = Input(&preview_filter_, search_options);
        MenuOption scope_options = MenuOption::Horizontal();
        scope_options.on_change = [this] { OnPreviewScopeChanged(); };
        auto scope_menu = Menu(&preview_scope_labels_, &preview_scope_selected_,
                               scope_options);
        MenuOption preview_options;
        preview_options.on_enter = [this] { OpenPreviewDetails(); };
        auto preview_menu =
            Menu(&preview_labels_, &preview_selected_, preview_options);
        auto preview_view =
            Container::Vertical({search_input, scope_menu, preview_menu});

        auto result_view = Renderer([this] { return RenderPanel(1U); });
        auto log_view = Renderer([this] { return RenderPanel(2U); });
        auto view_content = Container::Tab(
            {preview_view, result_view, log_view}, &view_selected_);
        MenuOption view_options = MenuOption::Horizontal();
        auto view_menu = Menu(&view_labels_, &view_selected_, view_options);

        InputOption element_index_options;
        element_index_options.placeholder = "0基数组元素/曲线点索引";
        element_index_options.on_change = [this] { OnElementIndexChanged(); };
        auto element_index_input =
            Input(&modal_element_index_text_, element_index_options);
        InputOption target_options;
        target_options.placeholder = "目标物理值（仅VALUE或单曲线点）";
        auto target_input = Input(&calibration_form_.target, target_options);
        auto read_button =
            Button("读取当前值", [this] { SubmitCalibrationRead(); });
        auto previous_array_page_button =
            Button("数组上一页", [this] { NavigateArrayPage(-1); });
        auto read_array_page_button =
            Button("读取本页", [this] { SubmitCalibrationArrayPage(); });
        auto next_array_page_button =
            Button("数组下一页", [this] { NavigateArrayPage(1); });
        auto watch_button =
            Button("切换采样", [this] { TogglePreviewWatch(); });
        auto write_button =
            Button("申请写入…", [this] { SubmitCalibrationWrite(); });
        auto close_button =
            Button("关闭详情", [this] { CloseVariableModal(); });
        auto element_index_label =
            Renderer([] { return text("元素索引(0基)："); });
        auto element_index_control = Maybe(
            Container::Horizontal({element_index_label, element_index_input}),
            [this] {
                return !terminal_too_small_.load() &&
                       modal_symbol_info_.has_value() &&
                       RequiresElementIndex(*modal_symbol_info_);
            });
        auto target_label = Renderer([] { return text("目标值："); });
        auto target_control =
            Maybe(Container::Horizontal({target_label, target_input}), [this] {
                return !terminal_too_small_.load() && !DaqRunning() &&
                       modal_symbol_info_.has_value() &&
                       ModalWriteUnsupportedReason().empty();
            });
        auto read_control = Maybe(read_button, [this] {
            return !terminal_too_small_.load() && !DaqRunning() &&
                   modal_symbol_info_.has_value() &&
                   ModalReadUnsupportedReason().empty();
        });
        auto array_page_control =
            Maybe(Container::Horizontal({previous_array_page_button,
                                         read_array_page_button,
                                         next_array_page_button}),
                  [this] {
                      return !terminal_too_small_.load() && !DaqRunning() &&
                             ModalArrayPageUnsupportedReason().empty();
                  });
        auto array_page_display = Maybe(
            Renderer([this] {
                if (!modal_symbol_info_.has_value()) return text("");
                const auto extent =
                    modal_symbol_info_->dimensions.front().extent;
                const auto count =
                    modal_array_page_start_ < extent
                        ? std::min<std::uint64_t>(
                              kArrayPageSize, extent - modal_array_page_start_)
                        : 0U;
                const auto end = modal_array_page_start_ + count;
                return vbox({text("数组页：[" +
                                  std::to_string(modal_array_page_start_) +
                                  ", " + std::to_string(end) + ") / " +
                                  std::to_string(extent)),
                             paragraph(modal_array_page_lines_)});
            }),
            [this] { return ModalArrayPageUnsupportedReason().empty(); });
        auto watch_control = Maybe(watch_button, [this] {
            return !terminal_too_small_.load() && !DaqRunning() &&
                   modal_symbol_info_.has_value() &&
                   modal_symbol_info_->kind == SymbolKind::Measurement &&
                   calmcar::xcp::tools::SymbolReadUnsupportedReason(
                       *modal_symbol_info_)
                       .empty();
        });
        auto write_control = Maybe(write_button, [this] {
            return !terminal_too_small_.load() && !DaqRunning() &&
                   modal_symbol_info_.has_value() &&
                   ModalWriteUnsupportedReason().empty();
        });
        auto details_container = Container::Vertical(
            {element_index_control, array_page_control, array_page_display,
             target_control, read_control, watch_control, write_control,
             close_button});
        auto details_renderer = Renderer(details_container, [&] {
            const auto layout = current_layout_;
            std::string name;
            std::string current;
            std::string form_status;
            std::optional<calmcar::xcp::tools::ObservationSnapshot> sample;
            bool watched = false;
            {
                std::lock_guard lock(output_mutex_);
                name = modal_symbol_info_.has_value()
                           ? QualifiedName(*modal_symbol_info_)
                           : variable_modal_.SelectedName();
                current = calibration_form_.current;
                form_status = calibration_form_.status;
                if (!name.empty()) sample = observation_model_.Find(name);
                std::lock_guard watched_lock(watched_mutex_);
                watched = std::find(watched_.begin(), watched_.end(), name) !=
                          watched_.end();
            }
            auto read_reason = ModalReadUnsupportedReason();
            auto write_reason = ModalWriteUnsupportedReason();
            const bool daq_active = DaqRunning();
            const bool array_page_supported =
                ModalArrayPageUnsupportedReason().empty();
            if (daq_active) {
                if (read_reason.empty()) read_reason = "DAQ运行中";
                if (write_reason.empty()) write_reason = "DAQ运行中";
            }
            const std::string kind =
                modal_symbol_info_.has_value()
                    ? (modal_symbol_info_->kind == SymbolKind::Measurement
                           ? "MEASUREMENT"
                       : modal_symbol_info_->kind ==
                                   SymbolKind::Characteristic &&
                               modal_symbol_info_->characteristic_type ==
                                   CharacteristicType::Value
                           ? "CHARACTERISTIC/VALUE"
                       : modal_symbol_info_->kind ==
                                   SymbolKind::Characteristic &&
                               modal_symbol_info_->characteristic_type ==
                                   CharacteristicType::Curve
                           ? "CHARACTERISTIC/CURVE"
                           : "其他CHARACTERISTIC/结构体")
                    : "";
            std::string element_info;
            if (modal_symbol_info_.has_value() &&
                !modal_symbol_info_->dimensions.empty()) {
                const auto& symbol = *modal_symbol_info_;
                element_info = "元素总数：" +
                               std::to_string(symbol.dimensions.front().extent);
                if (symbol.fixed_curve_axis.has_value()) {
                    const auto index = calmcar::xcp::tools::ParseElementIndex(
                        modal_element_index_text_);
                    if (index &&
                        *index <
                            symbol.fixed_curve_axis->explicit_values.size()) {
                        element_info +=
                            " | X[" + std::to_string(*index) + "]=" +
                            std::to_string(symbol.fixed_curve_axis
                                               ->explicit_values[*index]);
                    } else if (index &&
                               *index < symbol.fixed_curve_axis->point_count) {
                        element_info +=
                            " | X[" + std::to_string(*index) + "]=" +
                            std::to_string(symbol.fixed_curve_axis->offset +
                                           symbol.fixed_curve_axis->distance *
                                               static_cast<double>(*index));
                    } else {
                        element_info +=
                            " | X[0]=" +
                            std::to_string(symbol.fixed_curve_axis->offset) +
                            "，步长=" +
                            std::to_string(symbol.fixed_curve_axis->distance);
                    }
                }
            }
            const std::string unit = modal_symbol_info_.has_value()
                                         ? modal_symbol_info_->conversion.unit
                                         : "";
            const bool curve_point_write =
                modal_symbol_info_.has_value() &&
                modal_symbol_info_->kind == SymbolKind::Characteristic &&
                modal_symbol_info_->characteristic_type ==
                    CharacteristicType::Curve;
            const std::string write_label =
                curve_point_write ? "曲线点写入：" : "写入能力：";
            const std::string latest =
                sample.has_value() && sample->has_value
                    ? sample->value + (unit.empty() ? "" : " " + unit) +
                          (sample->stale ? "（旧值）" : "") +
                          (sample->stopped ? "（已停止）" : "")
                    : "尚无成功样本";
            const std::string sample_time =
                sample.has_value() && !sample->timestamp.empty()
                    ? " | 时间：" + sample->timestamp
                    : "";
            const auto warning =
                layout.too_small
                    ? text("终端尺寸不足；读取/编辑/危险确认已禁用。") |
                          color(Color::Red)
                    : text(
                          "详情弹窗锁定背景列表；Esc "
                          "关闭并保留筛选与滚动位置。");
            const auto title = text("变量详情 · " + name + "    [Esc 关闭]") |
                               bold | color(Color::Cyan);
            if (array_page_supported) {
                return vbox({title, separator(),
                             text("一维MEASUREMENT数组" +
                                  (unit.empty() ? "" : " | 单位：" + unit) +
                                  " | 支持0基单项读取和8项分页；只读"),
                             text("状态：" + form_status),
                             array_page_control->Render(),
                             array_page_display->Render(),
                             element_index_control->Render(),
                             hbox({read_control->Render(), text("  "),
                                   close_button->Render()}),
                             warning}) |
                       border |
                       size(WIDTH, LESS_THAN, std::max(1, layout.modal_width)) |
                       size(HEIGHT, LESS_THAN,
                            std::max(1, layout.modal_height));
            }
            return vbox({title, separator(),
                         text("类型：" + kind +
                              (unit.empty() ? "" : " | 单位：" + unit)),
                         text("采样状态：" +
                              std::string(DaqRunning()
                                              ? "DAQ运行中，采样集合锁定"
                                          : watched ? "已加入采样"
                                                    : "未加入采样")),
                         text("最近值：" + latest + sample_time +
                              (sample.has_value() && !sample->status.empty()
                                   ? " | " + sample->status
                                   : "")),
                         text(array_page_supported
                                  ? "读取：支持索引/分页 | 写入：数组只读"
                                  : "手动读取：" +
                                        (read_reason.empty()
                                             ? "允许"
                                             : "拒绝 — " + read_reason)),
                         text(write_label + (write_reason.empty()
                                                 ? "可申请（仍需确认）"
                                                 : "拒绝 — " + write_reason)),
                         text("当前值：" + current +
                              (current == "尚未读取" || unit.empty()
                                   ? ""
                                   : " " + unit) +
                              " | " + form_status),
                         element_info.empty()
                             ? text("")
                             : text("元素布局：" + element_info),
                         array_page_control->Render(),
                         array_page_display->Render(),

                         element_index_control->Render(),
                         target_control->Render(),
                         hbox({read_control->Render(), text("  "),
                               watch_control->Render(), text("  "),
                               write_control->Render(), text("  "),
                               close_button->Render()}),
                         text("索引为0基；写入仅限安全策略允许的 "
                              "CHARACTERISTIC/VALUE 或固定轴 CURVE 单点；"
                              "表单编辑不会自动写入。") |
                             color(Color::Yellow),
                         warning}) |
                   border |
                   size(WIDTH, LESS_THAN, std::max(1, layout.modal_width)) |
                   size(HEIGHT, LESS_THAN, std::max(1, layout.modal_height));
        });

        auto confirmation_renderer = Renderer([this] {
            const auto layout = current_layout_;
            return vbox({text("危险操作确认 · 默认取消") | bold |
                             color(Color::Yellow),
                         separator(), paragraph(confirmation_prompt_),
                         text("Y/y：明确确认；N/"
                              "n、Esc：取消；变量写入返回详情，命令确认返回界面"
                              "。"),
                         text("Enter、其它按键及窗口过小都不会发送操作；F5取消"
                              "并停止。")}) |
                   border |
                   size(WIDTH, LESS_THAN, std::max(1, layout.modal_width)) |
                   size(HEIGHT, LESS_THAN, std::max(1, layout.modal_height));
        });
        auto modal_pages = Container::Tab(
            {details_renderer, confirmation_renderer}, &modal_page_selected_);

        auto root =
            Container::Vertical({view_menu, view_content, command_input});
        root->SetActiveChild(command_input);
        auto renderer = Renderer(root, [&] {
            const auto terminal = Terminal::Size();
            current_layout_ = calmcar::xcp::tools::CalculateTuiLayout(
                terminal.dimx, terminal.dimy);
            const bool was_too_small =
                terminal_too_small_.exchange(current_layout_.too_small);
            if (current_layout_.too_small && !was_too_small && modal_visible_ &&
                variable_modal_.Page() ==
                    calmcar::xcp::tools::VariableModalPage::Confirmation) {
                if (confirmation_gate_.CancelBeforeDispatch()) {
                    Say("终端缩小至 80×24 以下，已在 Dispatch "
                        "前取消操作；没有发送。");
                } else {
                    Say("终端已缩小，但操作已越过 Dispatch "
                        "边界或已取消；在途请求无法撤回。");
                }
            }
            {
                std::lock_guard lock(output_mutex_);
                viewport_.SetPageSize(current_layout_.panel_rows);
            }
            std::size_t watched_count = 0;
            {
                std::lock_guard lock(watched_mutex_);
                watched_count = watched_.size();
            }
            const auto run_state = daq_running_.load()   ? "DAQ 运行中"
                                   : watching_.load()    ? "POLL 运行中"
                                   : worker_busy_.load() ? "操作中"
                                                         : "已停止";
            const auto mode_state =
                options_.mode == "daq"
                    ? "DAQ/" + options_.daq_profile
                    : "POLL/目标 " + std::to_string(options_.interval.count()) +
                          " ms";
            const bool compact = current_layout_.compact;
            std::string help;
            if (modal_visible_ &&
                variable_modal_.Page() ==
                    calmcar::xcp::tools::VariableModalPage::Confirmation) {
                help = compact ? "Y确认 | N/Esc取消回详情 | F5取消并停止"
                               : "Y 确认 | N/Esc 取消并回详情 | F5 取消并停止";
            } else if (modal_visible_) {
                help = compact ? "Tab切换 | Esc关闭 | F5停止"
                               : "Tab 弹窗内切换 | Enter 激活 | Esc 关闭详情 | "
                                 "F5 停止";
            } else if (command_input->Focused()) {
                help = compact
                           ? "Enter执行 | ↑↓历史 | F2视图 | F5停止 | quit退出"
                           : "Enter 执行 | ↑↓ 命令历史 | F2 切换视图 | F5 停止 "
                             "| quit 退出";
            } else if (preview_menu->Focused()) {
                help =
                    compact
                        ? "↑↓选择 | PgUp/Dn翻页 | Enter详情 | F2视图 | Esc命令"
                        : "↑↓ 选择变量 | PgUp/PgDn 翻页 | Enter 打开详情 | F2 "
                          "视图 | Esc 命令";
            } else if (search_input->Focused()) {
                help = compact ? "Enter搜索元数据 | 不触发ECU操作 | Esc命令"
                               : "输入名称模式 | Enter 搜索A2L元数据 | "
                                 "不触发ECU读写 | Esc 命令";
            } else if (scope_menu->Focused()) {
                help = compact ? "←→范围 | Enter应用 | F2视图 | Esc命令"
                               : "←→ 选择全部/已采样 | Enter 应用 | F2 视图 | "
                                 "Esc 命令";
            } else {
                help =
                    compact
                        ? "F2视图 | Tab焦点 | PgUp/Dn翻页 | F5停止 | quit退出"
                        : "F2 切换三视图 | Tab 焦点 | PgUp/PgDn 浏览 | F5 停止 "
                          "| quit 退出";
            }
            const auto dimensions =
                current_layout_.too_small
                    ? text("窗口不足 80×24：禁用详情、编辑和危险确认。") |
                          color(Color::Red)
                    : text("就绪");
            const auto path_separator = options_.a2l_path.find_last_of("/\\\\");
            const auto a2l_name =
                path_separator == std::string::npos
                    ? options_.a2l_path
                    : options_.a2l_path.substr(path_separator + 1U);
            const auto status_bar =
                vbox({hbox({text("XCP Master") | bold,
                            text("  |  已连接 " + options_.host + ":" +
                                 std::to_string(options_.port) + "  |  " +
                                 mode_state + "  |  " + run_state)}),
                      hbox({text("A2L " + a2l_name),
                            options_.module.empty()
                                ? text("")
                                : text("  |  MODULE " + options_.module),
                            text("  |  候选 " +
                                 std::to_string(preview_symbols_.size()) +
                                 "  |  采样集合 " +
                                 std::to_string(watched_count))})});
            const auto preview_panel =
                vbox({hbox({text("搜索："), search_input->Render(),
                            text("  范围："), scope_menu->Render()}),
                      text(preview_status_),
                      preview_menu->Render() | vscroll_indicator | frame |
                          size(HEIGHT, LESS_THAN,
                               static_cast<int>(std::max<std::size_t>(
                                   1U, current_layout_.preview_rows))) |
                          flex});
            const auto main_panel =
                vbox({view_menu->Render(), separator(),
                      (view_selected_ == 0 ? preview_panel
                                           : view_content->Render()) |
                          flex});
            return vbox({status_bar, main_panel | flex, separator(),
                         hbox({text("命令 > "), command_input->Render()}),
                         hbox({dimensions, text("  |  "), text(help)})}) |
                   border;
        });
        auto ui = Modal(renderer, modal_pages, &modal_visible_);
        ui = CatchEvent(ui, [this, command_input](Event event) {
            if (modal_visible_) {
                if (event == Event::F2) return true;
                if (event == Event::F5) {
                    if (variable_modal_.Page() ==
                        calmcar::xcp::tools::VariableModalPage::Confirmation) {
                        ResolveConfirmation(false);
                    }
                    SubmitCommand("stop");
                    return true;
                }
                if (event == Event::Escape) {
                    if (variable_modal_.Page() ==
                        calmcar::xcp::tools::VariableModalPage::Confirmation) {
                        ResolveConfirmation(false);
                        return true;
                    }
                    CloseVariableModal();
                    return true;
                }
                if (variable_modal_.Page() ==
                    calmcar::xcp::tools::VariableModalPage::Confirmation) {
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
                return false;
            }
            if (event == Event::F5) {
                SubmitCommand("stop");
                return true;
            }
            if (event == Event::PageUp || event == Event::PageDown) {
                if (view_selected_ == 0) return true;
                std::lock_guard lock(output_mutex_);
                const auto view = static_cast<std::size_t>(view_selected_);
                const auto total =
                    view_selected_ == 1
                        ? (listing_active_ ? listing_rows_.size()
                                           : result_lines_.Size())
                        : log_lines_.Size();
                if (event == Event::PageUp)
                    viewport_.PageUp(view, total);
                else
                    viewport_.PageDown(view, total);
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
                command_input->TakeFocus();
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
        SubmitCommand("find *", std::nullopt, {},
                      preview_input_revision_.Capture());
        screen_->Loop(ui);
        worker_stop_.store(true);
        command_cv_.notify_all();
        confirmation_gate_.Cancel();
        if (command_worker_.joinable()) command_worker_.join();
        StopWatch();
        screen_ = nullptr;
    }

private:
    /** @brief 按当前尺寸渲染结果或日志分页，调用期间保护共享缓冲。 */
    ftxui::Element RenderPanel(std::size_t panel) {
        using namespace ftxui;
        std::lock_guard lock(output_mutex_);
        const auto viewport_index = panel;
        const auto& source =
            panel == 1U
                ? (listing_active_ ? listing_rows_ : result_lines_.Lines())
                : log_lines_.Lines();
        viewport_.SetPageSize(current_layout_.panel_rows);
        const auto start = viewport_.Start(viewport_index, source.size());
        const auto end =
            std::min(source.size(), start + current_layout_.panel_rows);
        Elements rows;
        for (auto index = start; index < end; ++index)
            rows.push_back(text(source[index]));
        if (rows.empty()) rows.push_back(text("暂无内容"));
        rows.push_back(
            text("显示 " + std::to_string(source.empty() ? 0U : start + 1U) +
                 "-" + std::to_string(end) + "/" +
                 std::to_string(source.size()) + " | PgUp/PgDn 翻页"));
        return vbox(std::move(rows)) | vscroll_indicator | frame;
    }

    /** @brief 构造变量表格中的名称、最新值、采样状态及写入资格摘要。 */
    std::string FormatPreviewLabel(
        const SymbolInfo& symbol,
        const std::optional<calmcar::xcp::tools::ObservationSnapshot>& sample,
        bool watched) const {
        const auto name = QualifiedName(symbol);
        std::string label = name + " | ";
        if (sample.has_value() && sample->has_value) {
            label += sample->value;
            if (!symbol.conversion.unit.empty())
                label += " " + symbol.conversion.unit;
            if (sample->stale) label += "（旧值）";
            if (sample->stopped) label += "（已停止）";
            if (!current_layout_.compact && !sample->timestamp.empty())
                label += " | " + sample->timestamp;
        } else {
            label += sample.has_value() && !sample->status.empty()
                         ? sample->status
                         : "—";
        }
        label += watched ? " | 已加入采样" : " | 未采样";
        const auto read_reason =
            calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
        if (!read_reason.empty()) label += " | 读取拒绝: " + read_reason;
        if (symbol.kind == SymbolKind::Measurement) {
            label += " | MEASUREMENT";
        } else {
            const auto write_reason =
                calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
            label += write_reason.empty() ? " | 可写标量候选"
                                          : " | 写入拒绝: " + write_reason;
        }
        return label;
    }

    /** @brief 在界面线程重建过滤后的变量表和索引。 */
    void RebuildPreviewLabels() {
        const auto old_selected_name =
            preview_selected_ >= 0 &&
                    static_cast<std::size_t>(preview_selected_) <
                        preview_visible_names_.size()
                ? preview_visible_names_[preview_selected_]
                : std::string{};
        const auto old_selected_index = preview_selected_;
        preview_labels_.clear();
        preview_visible_names_.clear();
        preview_visible_index_.clear();
        std::lock_guard lock(output_mutex_);
        std::lock_guard watched_lock(watched_mutex_);
        for (const auto& symbol : preview_symbols_) {
            const auto name = QualifiedName(symbol);
            const bool watched = std::find(watched_.begin(), watched_.end(),
                                           name) != watched_.end();
            if (preview_scope_selected_ == 1 && !watched) continue;
            const auto sample = observation_model_.Find(name);
            preview_visible_index_[name] = preview_labels_.size();
            preview_visible_names_.push_back(name);
            preview_labels_.push_back(
                FormatPreviewLabel(symbol, sample, watched));
        }
        const auto retained = preview_visible_index_.find(old_selected_name);
        if (retained != preview_visible_index_.end()) {
            preview_selected_ = static_cast<int>(retained->second);
        } else {
            preview_selected_ =
                preview_labels_.empty()
                    ? 0
                    : std::clamp(old_selected_index, 0,
                                 static_cast<int>(preview_labels_.size() - 1U));
        }
        const std::string active_filter =
            preview_active_filter_.empty() ? "*" : preview_active_filter_;
        preview_status_ =
            "结果筛选 " + active_filter + "：匹配 " +
            std::to_string(preview_symbols_.size()) + " 项；当前显示 " +
            std::to_string(preview_labels_.size()) + " 项 | ↑↓选择，Enter详情";
        if (preview_filter_ != preview_active_filter_)
            preview_status_ += " | 搜索框是未提交草稿";
    }

    /** @brief 保存一次成功样本；保留最近值并刷新预览行。 */
    void RecordObservationValue(const std::string& name,
                                const std::string& value,
                                const std::string& status,
                                const std::string& timestamp) {
        {
            std::lock_guard lock(output_mutex_);
            observation_model_.UpdateValue(name, value, status, timestamp);
            const auto selection = calibration_form_.Snapshot();
            if (selection.name == name)
                static_cast<void>(
                    calibration_form_.Apply(selection, status, value));
        }
        RequestPreviewRefresh(name);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 保存失败状态但保留最近成功值，并标为旧样本。 */
    void RecordObservationFailure(const std::string& name,
                                  const std::string& status,
                                  const std::string& timestamp) {
        {
            std::lock_guard lock(output_mutex_);
            observation_model_.MarkFailure(name, status, timestamp);
            const auto selection = calibration_form_.Snapshot();
            if (selection.name == name)
                static_cast<void>(calibration_form_.Apply(selection, status));
        }
        RequestPreviewRefresh(name);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 新增采样项后保留旧值并显示等待状态。 */
    void RecordObservationPending(const std::string& name) {
        {
            std::lock_guard lock(output_mutex_);
            observation_model_.MarkPending(name);
            const auto selection = calibration_form_.Snapshot();
            if (selection.name == name)
                static_cast<void>(
                    calibration_form_.Apply(selection, "等待样本"));
        }
        RequestPreviewRefresh(name);
    }

    /** @brief 只更新已显示符号的一行，避免高速采样时重建整张表。 */
    void UpdatePreviewLabel(const std::string& name) {
        const auto visible = preview_visible_index_.find(name);
        if (visible == preview_visible_index_.end()) return;
        const auto symbol_index = preview_symbol_index_.find(name);
        if (symbol_index == preview_symbol_index_.end()) return;
        bool rebuild = false;
        {
            std::lock_guard lock(output_mutex_);
            std::lock_guard watched_lock(watched_mutex_);
            const bool watched = std::find(watched_.begin(), watched_.end(),
                                           name) != watched_.end();
            if (preview_scope_selected_ == 1 && !watched) {
                rebuild = true;
            } else {
                preview_labels_[visible->second] =
                    FormatPreviewLabel(preview_symbols_[symbol_index->second],
                                       observation_model_.Find(name), watched);
            }
        }
        if (rebuild) RebuildPreviewLabels();
    }

    /** @brief 处理“全部/仅采样”范围切换。 */
    void OnPreviewScopeChanged() { RebuildPreviewLabels(); }

    /** @brief Enter 打开预览行详情；列表位置与搜索词不变。 */
    void OpenPreviewDetails() {
        if (terminal_too_small_.load()) {
            Say("窗口小于 80×24，变量详情和编辑操作已禁用。");
            return;
        }
        if (preview_selected_ < 0 ||
            static_cast<std::size_t>(preview_selected_) >=
                preview_visible_names_.size())
            return;
        const auto name = preview_visible_names_[preview_selected_];
        const auto symbol = preview_symbol_index_.find(name);
        if (symbol == preview_symbol_index_.end()) return;
        modal_symbol_info_ = preview_symbols_[symbol->second];
        modal_element_index_text_ =
            RequiresElementIndex(*modal_symbol_info_) ? "0" : "";
        modal_array_page_start_ = 0;
        modal_array_page_lines_ = "尚未读取本页。";
        {
            std::lock_guard lock(output_mutex_);
            static_cast<void>(calibration_form_.Select(name, true));
            const auto sample = observation_model_.Find(name);
            if (sample.has_value() && sample->has_value)
                static_cast<void>(
                    calibration_form_.Apply(calibration_form_.Snapshot(),
                                            sample->status, sample->value));
        }
        const auto layout = current_layout_;
        if (!variable_modal_.Open(name, layout)) {
            modal_symbol_info_.reset();
            Say("无法打开变量详情，请检查终端尺寸和A2L符号。");
            return;
        }
        modal_page_selected_ = 0;
        modal_visible_ = true;
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /**
     * @brief 关闭详情并丢弃草稿；底层列表筛选/选择/滚动保持原样。
     * @param revoke_confirmation 是否撤销尚未派发的确认票据。
     */
    void CloseVariableModal(bool revoke_confirmation = true) {
        if (variable_modal_.Page() ==
            calmcar::xcp::tools::VariableModalPage::Confirmation) {
            if (revoke_confirmation) ResolveConfirmation(false);
        }
        if (revoke_confirmation) confirmation_gate_.Cancel();
        variable_modal_.Close();
        modal_visible_ = false;
        modal_page_selected_ = 0;
        modal_symbol_info_.reset();
        modal_array_page_start_ = 0;
        modal_array_page_lines_.clear();
        {
            std::lock_guard lock(output_mutex_);
            static_cast<void>(calibration_form_.Select({}, true));
        }
        ApplyPendingPreviewSearchResult();
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 在模态详情中切换该变量是否加入当前采样集合。 */
    void TogglePreviewWatch() {
        if (!modal_symbol_info_.has_value()) return;
        const auto name = QualifiedName(*modal_symbol_info_);
        SubmitCommand(IsWatched(name) ? "remove " + name : "add " + name);
    }

    /** @brief 由后台更新触发；样本/观测集合变化统一回到界面线程。 */
    void RequestPreviewRefresh(std::string name = {}, bool rebuild = false) {
        {
            std::lock_guard lock(preview_refresh_mutex_);
            if (rebuild)
                preview_refresh_all_ = true;
            else if (!name.empty())
                preview_refresh_pending_.insert(std::move(name));
        }
        SchedulePreviewRefresh();
    }

    /** @brief 将待处理样本更新合并成一个 FTXUI UI 线程事件。 */
    void SchedulePreviewRefresh() {
        if (!screen_) return;
        bool expected = false;
        if (!preview_refresh_posted_.compare_exchange_strong(expected, true))
            return;
        screen_->Post(ftxui::Closure([this] {
            std::vector<std::string> names;
            bool rebuild = false;
            {
                std::lock_guard lock(preview_refresh_mutex_);
                rebuild = std::exchange(preview_refresh_all_, false);
                names.assign(preview_refresh_pending_.begin(),
                             preview_refresh_pending_.end());
                preview_refresh_pending_.clear();
                preview_refresh_posted_.store(false);
            }
            if (rebuild)
                RebuildPreviewLabels();
            else
                for (const auto& name : names) UpdatePreviewLabel(name);
            bool pending = false;
            {
                std::lock_guard lock(preview_refresh_mutex_);
                pending =
                    preview_refresh_all_ || !preview_refresh_pending_.empty();
            }
            if (pending) SchedulePreviewRefresh();
        }));
    }

    /** @brief 一次命令及表单不可变快照，避免后台再读取新选择。 */
    struct CommandRequest {
        std::string text; /**< 命令文本。 */
        std::optional<calmcar::xcp::tools::CalibrationSelection::Snapshot>
            calibration;    /**< 表单所选变量与选择代际。 */
        std::string target; /**< 表单提交时的目标物理值。 */
        std::optional<std::uint64_t>
            preview_revision; /**< 搜索输入提交代际。 */
        std::optional<std::uint64_t>
            element_index; /**< 详情弹窗提交的0基元素索引。 */
        std::optional<std::uint64_t>
            array_page_start; /**< 数组分页读取的0基起始索引。 */
    };

    /** @brief 在详情弹窗打开期间延迟列表重建，保护背景列表上下文。 */
    struct PendingPreviewSearchResult {
        std::uint64_t generation{0};                   /**< A2L查询代际。 */
        std::string pattern;                           /**< 实际查询文本。 */
        std::optional<std::uint64_t> preview_revision; /**< 输入草稿代际。 */
        std::vector<SymbolInfo> symbols;               /**< 搜索结果快照。 */
    };

    /** @brief 将命令提交到后台队列，避免 XCP 操作阻塞界面线程。 */
    bool SubmitCommand(
        const std::string& command,
        std::optional<calmcar::xcp::tools::CalibrationSelection::Snapshot>
            calibration = std::nullopt,
        std::string target = {},
        std::optional<std::uint64_t> preview_revision = std::nullopt,
        std::optional<std::uint64_t> element_index = std::nullopt,
        std::optional<std::uint64_t> array_page_start = std::nullopt) {
        if (command.empty()) return true;
        const bool control =
            command == "stop" || command == "quit" || command == "exit";
        bool rejected = false;
        {
            std::lock_guard lock(command_queue_mutex_);
            if (!control && (worker_busy_.load() || !command_queue_.empty())) {
                rejected = true;
            } else if (control) {
                control_cancel_.store(true);
                confirmation_gate_.Cancel();
                command_queue_.push_front({command, std::move(calibration),
                                           std::move(target), preview_revision,
                                           element_index, array_page_start});
            } else {
                command_queue_.push_back({command, std::move(calibration),
                                          std::move(target), preview_revision,
                                          element_index, array_page_start});
            }
        }
        if (rejected) {
            Say("操作正在执行；普通命令不排队，请稍后重试。");
            return false;
        }
        if (command == "stop" || command == "quit" || command == "exit") {
            watching_.store(false);
            wait_cv_.notify_all();
        }
        command_cv_.notify_one();
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
        return true;
    }

    /** @brief 后台串行执行命令和需要确认的操作。 */
    void CommandLoop() {
        while (true) {
            CommandRequest request;
            {
                std::unique_lock lock(command_queue_mutex_);
                command_cv_.wait(lock, [this] {
                    return worker_stop_.load() || !command_queue_.empty();
                });
                if (command_queue_.empty() && worker_stop_.load()) return;
                request = std::move(command_queue_.front());
                command_queue_.pop_front();
                if (request.text != "stop" && request.text != "quit" &&
                    request.text != "exit")
                    control_cancel_.store(false);
                worker_busy_.store(true);
            }
            bool keep_running = true;
            try {
                keep_running = Execute(request);
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

    /**
     * @brief 在模态窗口中请求确认并等待用户选择；无明确接受则取消。
     * @param prompt 危险操作完整说明。
     * @param keep_details 确认完成后是否返回当前变量详情。
     * @return 本次有效确认票据；拒绝/退出/尺寸不足时为空。
     */
    std::optional<calmcar::xcp::tools::ConfirmationGate::Ticket> Confirm(
        const std::string& prompt, bool keep_details = false) {
        if (worker_stop_.load() || control_cancel_.load()) return std::nullopt;
        if (terminal_too_small_.load()) {
            Say("终端尺寸不足（至少 80×24）；危险操作未发送。");
            return std::nullopt;
        }
        Say(prompt);
        const auto ticket = confirmation_gate_.Begin();
        if (worker_stop_.load() || control_cancel_.load())
            confirmation_gate_.Cancel();
        if (!screen_) {
            confirmation_gate_.Cancel();
            return std::nullopt;
        }
        screen_->Post(ftxui::Closure([this, ticket, prompt, keep_details] {
            if (!confirmation_gate_.Pending() ||
                confirmation_gate_.CurrentTicket() != ticket)
                return;
            const auto terminal = ftxui::Terminal::Size();
            current_layout_ = calmcar::xcp::tools::CalculateTuiLayout(
                terminal.dimx, terminal.dimy);
            terminal_too_small_.store(current_layout_.too_small);
            if (current_layout_.too_small) {
                confirmation_gate_.Resolve(ticket, false);
                Say("终端尺寸不足；危险操作默认取消，未发送。");
                return;
            }
            const bool opened =
                keep_details
                    ? variable_modal_.BeginConfirmation(true, current_layout_)
                    : variable_modal_.OpenCommandConfirmation(current_layout_);
            if (!opened) {
                confirmation_gate_.Resolve(ticket, false);
                return;
            }
            confirmation_prompt_ = prompt;
            modal_page_selected_ = 1;
            modal_visible_ = true;
        }));
        const bool accepted = confirmation_gate_.Wait(ticket, worker_stop_);
        if (screen_) {
            screen_->Post(ftxui::Closure([this, keep_details] {
                if (keep_details) {
                    variable_modal_.CompleteConfirmation();
                    modal_visible_ = variable_modal_.IsOpen();
                    modal_page_selected_ = 0;
                } else {
                    CloseVariableModal(false);
                }
            }));
        }
        if (!accepted) return std::nullopt;
        return ticket;
    }

    /** @brief 将当前模态页的明确选择交回命令线程。 */
    void ResolveConfirmation(bool accepted) {
        confirmation_gate_.Resolve(confirmation_gate_.CurrentTicket(),
                                   accepted);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 判断后台命令所属的选择是否仍是当前表单选择。 */
    bool CalibrationSelectionMatches(
        const calmcar::xcp::tools::CalibrationSelection::Snapshot& selection) {
        std::lock_guard lock(output_mutex_);
        return calibration_form_.Matches(selection);
    }

    /** @brief 只允许所选变量与代际一致的业务结果更新标定表单。 */
    void UpdateCalibrationState(
        const calmcar::xcp::tools::CalibrationSelection::Snapshot& selection,
        const std::string& status,
        std::optional<std::string> current = std::nullopt) {
        std::lock_guard lock(output_mutex_);
        static_cast<void>(
            calibration_form_.Apply(selection, status, std::move(current)));
    }

    /** @brief 判断详情对象是否需要0基元素索引输入。 */
    bool RequiresElementIndex(const SymbolInfo& symbol) const {
        return !symbol.dimensions.empty() ||
               (symbol.kind == SymbolKind::Characteristic &&
                symbol.characteristic_type == CharacteristicType::Curve);
    }

    /** @brief 判断详情对象是否可显示连续的一维测量数组分页。 */
    bool IsMeasurementArray(const SymbolInfo& symbol) const {
        return symbol.kind == SymbolKind::Measurement &&
               symbol.dimensions.size() == 1U &&
               symbol.dimensions.front().extent > 0U;
    }

    /** @brief 当前详情是否支持安全的数组分页读取。 */
    std::string ModalArrayPageUnsupportedReason() const {
        if (!modal_symbol_info_.has_value()) return "未选择变量";
        const auto& symbol = *modal_symbol_info_;
        if (!IsMeasurementArray(symbol)) return "分页只支持一维MEASUREMENT数组";
        return calmcar::xcp::tools::SymbolDetailReadUnsupportedReason(symbol);
    }

    /** @brief 输入索引变化时同步所在数组页并丢弃旧页缓存。 */
    void OnElementIndexChanged() {
        if (!modal_symbol_info_.has_value() ||
            !IsMeasurementArray(*modal_symbol_info_))
            return;
        const auto index =
            calmcar::xcp::tools::ParseElementIndex(modal_element_index_text_);
        if (!index) return;
        const auto page_start = (*index / kArrayPageSize) * kArrayPageSize;
        if (page_start == modal_array_page_start_) return;
        modal_array_page_start_ = page_start;
        modal_array_page_lines_ = "尚未读取本页。";
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 通过上一页/下一页按钮导航数组页并更新起始索引。 */
    void NavigateArrayPage(int direction) {
        if (!modal_symbol_info_.has_value() ||
            !IsMeasurementArray(*modal_symbol_info_))
            return;
        const auto extent = modal_symbol_info_->dimensions.front().extent;
        const auto last_start =
            ((extent - 1U) / kArrayPageSize) * kArrayPageSize;
        const auto next_start =
            direction < 0 ? (modal_array_page_start_ >= kArrayPageSize
                                 ? modal_array_page_start_ - kArrayPageSize
                                 : 0U)
                          : calmcar::xcp::tools::NextElementPageStart(
                                extent, modal_array_page_start_, kArrayPageSize)
                                .value_or(last_start);
        if (next_start == modal_array_page_start_) return;
        modal_array_page_start_ = next_start;
        modal_element_index_text_ = std::to_string(next_start);
        modal_array_page_lines_ = "尚未读取本页。";
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 当前详情是否满足安全的手动读取能力。 */
    std::string ModalReadUnsupportedReason() const {
        if (!modal_symbol_info_.has_value()) return "未选择变量";
        const auto& symbol = *modal_symbol_info_;
        const auto reason =
            calmcar::xcp::tools::SymbolDetailReadUnsupportedReason(symbol);
        if (!reason.empty()) return reason;
        if (RequiresElementIndex(symbol)) {
            const auto index = calmcar::xcp::tools::ParseElementIndex(
                modal_element_index_text_);
            if (!index) return "请输入有效的0基元素索引";
            if (symbol.dimensions.empty() ||
                *index >= symbol.dimensions.front().extent)
                return "元素索引超出范围";
        }
        return {};
    }

    /** @brief 当前详情是否满足标量VALUE/固定轴CURVE点写入策略。 */
    std::string ModalWriteUnsupportedReason() const {
        if (!modal_symbol_info_.has_value()) return "未选择变量";
        const auto& symbol = *modal_symbol_info_;
        std::string reason;
        if (symbol.kind == SymbolKind::Characteristic &&
            symbol.characteristic_type == CharacteristicType::Curve) {
            const auto index = calmcar::xcp::tools::ParseElementIndex(
                modal_element_index_text_);
            if (!index) return "请输入有效的0基曲线点索引";
            reason = calmcar::xcp::tools::CurvePointWriteUnsupportedReason(
                symbol, *index);
        } else {
            reason = calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
        }
        if (!reason.empty()) return reason;
        if (!symbol.memory_segment.has_value() ||
            !symbol.memory_segment->has_xcp_segment)
            return "无法唯一关联有效的XCP MEMORY_SEGMENT/PAG";
        for (const auto& page : symbol.memory_segment->pages) {
            if (calmcar::xcp::tools::CalibrationPageWriteUnsupportedReason(
                    symbol, page.number, calmcar::xcp::PageAccessType::DontCare)
                    .empty())
                return {};
        }
        return "A2L未声明允许无条件XCP写入的PAGE";
    }

    /** @brief 将模态详情绑定的符号快照提交到安全读取路径。 */
    void SubmitCalibrationRead() {
        calmcar::xcp::tools::CalibrationSelection::Snapshot selection;
        {
            std::lock_guard lock(output_mutex_);
            selection = calibration_form_.Snapshot();
        }
        if (selection.name.empty() || !modal_symbol_info_.has_value()) return;
        std::optional<std::uint64_t> element_index;
        if (RequiresElementIndex(*modal_symbol_info_)) {
            element_index = calmcar::xcp::tools::ParseElementIndex(
                modal_element_index_text_);
            if (!element_index) {
                Say("请输入有效的0基元素索引。");
                return;
            }
        }
        SubmitCommand("calread", std::move(selection), {}, std::nullopt,
                      element_index);
    }

    /** @brief 将当前数组页读取请求提交到后台XCP命令队列。 */
    void SubmitCalibrationArrayPage() {
        if (!modal_symbol_info_.has_value()) return;
        const auto reason = ModalArrayPageUnsupportedReason();
        if (!reason.empty()) {
            Say("数组分页读取不支持: " + reason);
            return;
        }
        if (DaqRunning()) {
            Say("DAQ运行中，不能执行数组分页读取。");
            return;
        }
        calmcar::xcp::tools::CalibrationSelection::Snapshot selection;
        {
            std::lock_guard lock(output_mutex_);
            selection = calibration_form_.Snapshot();
        }
        if (selection.name != QualifiedName(*modal_symbol_info_)) return;
        if (SubmitCommand("calpage", selection, {}, std::nullopt, std::nullopt,
                          modal_array_page_start_)) {
            modal_array_page_lines_ = "读取中…";
            if (screen_) screen_->PostEvent(ftxui::Event::Custom);
        }
    }

    /** @brief 将详情弹窗中的目标值交给既有预读/确认/回写路径。 */
    void SubmitCalibrationWrite() {
        if (!modal_symbol_info_.has_value()) return;
        const std::string reason = ModalWriteUnsupportedReason();
        if (!reason.empty()) {
            Say("该变量不允许写入: " + reason);
            return;
        }
        std::optional<std::uint64_t> element_index;
        if (modal_symbol_info_->kind == SymbolKind::Characteristic &&
            modal_symbol_info_->characteristic_type ==
                CharacteristicType::Curve)
            element_index = calmcar::xcp::tools::ParseElementIndex(
                modal_element_index_text_);
        calmcar::xcp::tools::CalibrationSelection::Snapshot selection;
        std::string target;
        {
            std::lock_guard lock(output_mutex_);
            selection = calibration_form_.Snapshot();
            target = calibration_form_.target;
        }
        if (selection.name.empty() || target.empty() ||
            selection.name != QualifiedName(*modal_symbol_info_)) {
            Say("请选择有效标量VALUE或曲线点，并填写目标物理值。");
            return;
        }
        SubmitCommand("calwrite", std::move(selection), std::move(target),
                      std::nullopt, element_index);
    }

    /** @brief 在UI线程应用搜索结果；模态期间先暂存以锁定背景列表。 */
    void ApplyPreviewSearchResult(PendingPreviewSearchResult result) {
        if (result.generation != preview_search_generation_.load()) return;
        if (result.preview_revision.has_value() &&
            !preview_input_revision_.Matches(*result.preview_revision)) {
            preview_status_ =
                "搜索输入已修改；保留当前列表，按 Enter 执行新搜索。";
            if (screen_) screen_->PostEvent(ftxui::Event::Custom);
            return;
        }
        if (variable_modal_.BlocksBackgroundListUpdates()) {
            pending_preview_search_result_ = std::move(result);
            return;
        }

        modal_symbol_info_.reset();
        variable_modal_.Close();
        modal_page_selected_ = 0;
        {
            std::lock_guard lock(output_mutex_);
            static_cast<void>(calibration_form_.Select({}, true));
        }
        preview_symbols_ = std::move(result.symbols);
        preview_symbol_index_.clear();
        for (std::size_t index = 0; index < preview_symbols_.size(); ++index)
            preview_symbol_index_[QualifiedName(preview_symbols_[index])] =
                index;
        preview_active_filter_ = result.pattern == "*" ? "" : result.pattern;
        if (result.preview_revision.has_value() &&
            preview_input_revision_.Matches(*result.preview_revision))
            preview_filter_ = preview_active_filter_;
        RebuildPreviewLabels();
    }

    /** @brief 模态关闭后应用仍有效的延迟搜索结果。 */
    void ApplyPendingPreviewSearchResult() {
        if (!pending_preview_search_result_.has_value()) return;
        auto result = std::move(*pending_preview_search_result_);
        pending_preview_search_result_.reset();
        ApplyPreviewSearchResult(std::move(result));
    }

    /** @brief 搜索可展示详情的 MEASUREMENT/CHARACTERISTIC 符号。 */
    void FindPreviewCandidates(
        const std::string& pattern,
        std::optional<std::uint64_t> preview_revision = std::nullopt) {
        const auto generation = ++preview_search_generation_;
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
        std::vector<SymbolInfo> symbols;
        for (const auto& symbol : result.Value()) {
            if ((!options_.module.empty() &&
                 symbol.module_name != options_.module) ||
                (symbol.kind != SymbolKind::Measurement &&
                 symbol.kind != SymbolKind::Characteristic))
                continue;
            symbols.push_back(symbol);
        }
        const auto candidate_count = symbols.size();
        auto apply = [this, generation, pattern, preview_revision,
                      symbols = std::move(symbols)]() mutable {
            ApplyPreviewSearchResult(PendingPreviewSearchResult{
                generation, pattern, preview_revision, std::move(symbols)});
        };
        if (screen_)
            screen_->Post(ftxui::Closure(std::move(apply)));
        else
            apply();
        Say("找到 " + std::to_string(candidate_count) +
            " 个 MEASUREMENT/CHARACTERISTIC 详情候选；详情页标明可读写范围。");
    }

    bool Execute(const CommandRequest& request) {
        const auto& line = request.text;
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command.empty()) return true;
        {
            std::lock_guard lock(output_mutex_);
            if (listing_active_) viewport_.FollowBottom(1);
            listing_active_ = false;
        }
        if (command == "quit" || command == "exit") return false;
        if (command == "find") {
            std::string pattern;
            input >> pattern;
            FindPreviewCandidates(pattern.empty() ? "*" : pattern,
                                  request.preview_revision);
        } else if (command == "calwrite") {
            if (!request.calibration)
                Say("标定表单参数无效。");
            else
                Write(request.calibration->name, request.target,
                      request.calibration, request.element_index);
        } else if (command == "calread") {
            if (!request.calibration)
                Say("标定表单参数无效。");
            else
                ReadCalibration(*request.calibration, request.element_index);
        } else if (command == "calpage") {
            if (!request.calibration || !request.array_page_start)
                Say("数组分页参数无效。");
            else
                ReadCalibrationArrayPage(*request.calibration,
                                         *request.array_page_start);
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
            "[pattern] | quit；变量预览 Enter 打开详情，Esc 关闭弹窗。");
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
        std::vector<std::string> full_listing;
        if (all) {
            full_listing.reserve(end - page_ + 2U);
            full_listing.push_back("完整目录 | " +
                                   std::to_string(listing_.size()) +
                                   " 个符号；仅 A2L 元数据，不读取 ECU");
        }
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
            if (all)
                full_listing.push_back(row.str());
            else
                Say(row.str());
        }
        if (all) {
            full_listing.push_back("列表结束。");
            {
                std::lock_guard lock(output_mutex_);
                listing_rows_ = std::move(full_listing);
                listing_active_ = true;
                viewport_.ResetToTop(1);
            }
            if (screen_) screen_->PostEvent(ftxui::Event::Custom);
        } else if (page_ < listing_.size()) {
            Say("还有结果；输入 more 翻页。");
        } else {
            Say("列表结束。");
        }
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
        if (added) {
            RecordObservationPending(canonical);
            RequestPreviewRefresh(canonical, true);
        }
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
            // 取消采样不删除最后一次成功读数；变量预览仍保留历史值。
            RequestPreviewRefresh(canonical, true);
        }
        Say(removed ? "已移除: " + canonical : "该变量不在观测列表。");
    }

    /** @brief 执行标定表单读取并更新其状态显示。 */
    void ReadCalibration(
        const calmcar::xcp::tools::CalibrationSelection::Snapshot& selection,
        std::optional<std::uint64_t> element_index = std::nullopt) {
        if (!CalibrationSelectionMatches(selection)) {
            Say("标定选择已改变，取消旧读取请求。");
            return;
        }
        if (DaqRunning()) {
            UpdateCalibrationState(selection,
                                   "DAQ 运行期间禁止读取，请先 stop");
            Say("DAQ 运行期间不能手动 read；请先 stop。");
            return;
        }
        auto found = database_.Find(selection.name);
        if (!found.HasValue()) {
            UpdateCalibrationState(selection, "A2L 查找失败", "读取失败");
            RecordObservationFailure(selection.name, "A2L 查找失败",
                                     LocalTimestamp());
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        const auto& symbol = found.Value();
        const auto read_reason =
            element_index.has_value()
                ? calmcar::xcp::tools::SymbolDetailReadUnsupportedReason(symbol)
                : calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
        if (!read_reason.empty()) {
            UpdateCalibrationState(selection, "拒绝读取：" + read_reason);
            Say("不支持读取: " + read_reason);
            return;
        }
        if (RequiresElementIndex(symbol) && !element_index.has_value()) {
            UpdateCalibrationState(selection, "需要指定0基元素索引");
            Say("数组/曲线读取需要0基元素索引。");
            return;
        }
        if (element_index.has_value() &&
            (symbol.dimensions.empty() ||
             *element_index >= symbol.dimensions.front().extent)) {
            UpdateCalibrationState(selection, "元素索引超出范围");
            Say("元素索引超出范围。");
            return;
        }
        auto value = ReadPhysical(symbol, element_index);
        const std::string index_label =
            element_index ? "[" + std::to_string(*element_index) + "] " : "";
        if (value) {
            const auto formatted = index_label + PhysicalToString(*value) +
                                   (found.Value().conversion.unit.empty()
                                        ? ""
                                        : " " + found.Value().conversion.unit);
            UpdateCalibrationState(selection, "读取成功", formatted);
            RecordObservationValue(QualifiedName(found.Value()),
                                   index_label + PhysicalToString(*value),
                                   element_index ? "手动元素读取" : "手动读取",
                                   LocalTimestamp());
        } else {
            UpdateCalibrationState(selection, "读取失败；保留最近值");
            RecordObservationFailure(QualifiedName(found.Value()),
                                     "手动读取失败", LocalTimestamp());
        }
        Say(value ? QualifiedName(found.Value()) + " " + index_label + "= " +
                        PhysicalToString(*value) + " " +
                        found.Value().conversion.unit
                  : "读取失败或不支持: " + QualifiedName(found.Value()));
    }

    /** @brief 仅把仍匹配详情符号/选择代际的数组页结果回填UI。 */
    void PostArrayPageResult(
        const calmcar::xcp::tools::CalibrationSelection::Snapshot& selection,
        const std::string& name, std::uint64_t page_start,
        std::string page_text) {
        const auto apply_page = [this, selection, name, page_start,
                                 page_text = std::move(page_text)] {
            if (!modal_visible_ || !modal_symbol_info_.has_value() ||
                QualifiedName(*modal_symbol_info_) != name ||
                modal_array_page_start_ != page_start ||
                !CalibrationSelectionMatches(selection))
                return;
            modal_array_page_lines_ = page_text;
            if (screen_) screen_->PostEvent(ftxui::Event::Custom);
        };
        if (screen_)
            screen_->Post(ftxui::Closure(apply_page));
        else
            apply_page();
    }

    /** @brief 读取并换算一页连续MEASUREMENT数组元素，回填详情弹窗。 */
    void ReadCalibrationArrayPage(
        const calmcar::xcp::tools::CalibrationSelection::Snapshot& selection,
        std::uint64_t page_start) {
        if (!CalibrationSelectionMatches(selection)) {
            Say("标定选择已改变，取消旧数组页读取请求。");
            return;
        }
        if (DaqRunning()) {
            Say("DAQ运行期间禁止读取数组页，请先 stop。");
            PostArrayPageResult(selection, selection.name, page_start,
                                "DAQ运行中；请先停止采样。");
            return;
        }
        auto found = database_.Find(selection.name);
        if (!found.HasValue()) {
            const auto error = "A2L查找数组失败: " + found.ErrorInfo().message;
            Say(error);
            PostArrayPageResult(selection, selection.name, page_start, error);
            return;
        }
        const auto& symbol = found.Value();
        if (symbol.kind != SymbolKind::Measurement ||
            symbol.dimensions.size() != 1U) {
            const std::string error = "数组页参数或MEASUREMENT布局无效。";
            Say(error);
            PostArrayPageResult(selection, QualifiedName(symbol), page_start,
                                error);
            return;
        }
        const auto page = calmcar::xcp::tools::MakeElementPageRange(
            symbol.dimensions.front().extent, page_start, kArrayPageSize);
        if (!page.has_value()) {
            const std::string error = "数组页起点未对齐或超出维度。";
            Say(error);
            PostArrayPageResult(selection, QualifiedName(symbol), page_start,
                                error);
            return;
        }
        const auto session = master_.GetSessionParameters();
        const auto ag =
            static_cast<std::size_t>(session.connect.address_granularity);
        const auto count = page->count;
        const auto reason =
            calmcar::xcp::tools::ElementRangeLayoutUnsupportedReason(
                symbol, page_start, count, ag, session.connect.byte_order);
        if (!reason.empty()) {
            const auto error = "数组页布局不支持: " + reason;
            Say(error);
            PostArrayPageResult(selection, QualifiedName(symbol), page_start,
                                error);
            return;
        }
        const auto byte_count = count * symbol.element_size_bytes;
        if (byte_count == 0U ||
            byte_count > std::numeric_limits<ByteCount>::max()) {
            const std::string error = "数组页字节长度超出XCP读取范围。";
            Say(error);
            PostArrayPageResult(selection, QualifiedName(symbol), page_start,
                                error);
            return;
        }
        const auto stride = symbol.dimensions.front().byte_stride;
        const auto address = symbol.xcp_address + page_start * stride;
        Bytes raw;
        try {
            std::lock_guard command_lock(command_mutex_);
            raw = master_.ReadMemoryBytes(
                static_cast<Address>(address),
                static_cast<AddressExtension>(symbol.address_extension),
                static_cast<ByteCount>(byte_count));
        } catch (const std::exception& error) {
            const auto message =
                "数组页XCP读取失败: " + std::string(error.what());
            Say(message);
            PostArrayPageResult(selection, QualifiedName(symbol), page_start,
                                message);
            return;
        }
        if (raw.size() != byte_count) {
            const std::string error = "数组页XCP返回字节数与A2L布局不一致。";
            Say(error);
            PostArrayPageResult(selection, QualifiedName(symbol), page_start,
                                error);
            return;
        }
        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t offset = 0; offset < count; ++offset) {
            const auto byte_offset =
                static_cast<std::size_t>(offset * symbol.element_size_bytes);
            const auto element_begin =
                raw.begin() + static_cast<std::ptrdiff_t>(byte_offset);
            const auto element_end =
                element_begin +
                static_cast<std::ptrdiff_t>(symbol.element_size_bytes);
            Bytes element(element_begin, element_end);
            const auto index = page_start + offset;
            auto physical = database_.ToPhysicalElement(QualifiedName(symbol),
                                                        index, element);
            if (!physical.HasValue()) {
                const auto error =
                    "数组元素物理换算失败: " + physical.ErrorInfo().message;
                Say(error);
                PostArrayPageResult(selection, QualifiedName(symbol),
                                    page_start, error);
                return;
            }
            values.push_back(PhysicalToString(physical.Value()));
        }

        std::ostringstream lines;
        for (std::size_t row = 0; row < values.size(); row += 4U) {
            if (row != 0U) lines << '\n';
            for (std::size_t column = row;
                 column < std::min(values.size(), row + 4U); ++column) {
                if (column != row) lines << " | ";
                lines << '[' << (page_start + column)
                      << "]: " << values[column];
            }
        }
        const auto page_text = lines.str();
        const auto name = QualifiedName(symbol);
        const auto page_end = page_start + count;
        Say(name + " 数组页[" + std::to_string(page_start) + ", " +
            std::to_string(page_end) + ")读取成功。");
        PostArrayPageResult(selection, name, page_start, page_text);
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
        if (value) {
            RecordObservationValue(canonical, PhysicalToString(*value),
                                   "命令读取", LocalTimestamp());
        } else {
            RecordObservationFailure(canonical, "命令读取失败",
                                     LocalTimestamp());
        }
        Say(value ? canonical + " = " + PhysicalToString(*value) + " " +
                        found.Value().conversion.unit
                  : "读取失败或不支持: " + canonical);
    }

    std::optional<PhysicalValue> ReadPhysical(
        const SymbolInfo& symbol,
        std::optional<std::uint64_t> element_index = std::nullopt) {
        std::lock_guard command_lock(command_mutex_);
        return ReadPhysicalLocked(symbol, element_index);
    }

    /** @brief 持有XCP命令互斥锁时读取并换算一个标量元素。 */
    std::optional<PhysicalValue> ReadPhysicalLocked(
        const SymbolInfo& symbol, std::optional<std::uint64_t> element_index) {
        const auto session = master_.GetSessionParameters();
        const auto ag =
            static_cast<std::size_t>(session.connect.address_granularity);
        const auto reason =
            element_index
                ? calmcar::xcp::tools::ElementRangeLayoutUnsupportedReason(
                      symbol, *element_index, 1, ag, session.connect.byte_order)
                : calmcar::xcp::tools::ScalarLayoutUnsupportedReason(
                      symbol, ag, session.connect.byte_order);
        if (!reason.empty()) {
            Say("不支持读取 " + QualifiedName(symbol) + ": " + reason);
            return std::nullopt;
        }
        std::uint64_t address = symbol.xcp_address;
        if (element_index.has_value()) {
            const std::uint64_t stride =
                symbol.dimensions.empty()
                    ? symbol.element_size_bytes
                    : symbol.dimensions.front().byte_stride;
            address += *element_index * stride;
        }
        try {
            Bytes raw = master_.ReadMemoryBytes(
                static_cast<Address>(address),
                static_cast<AddressExtension>(symbol.address_extension),
                static_cast<ByteCount>(symbol.element_size_bytes));
            auto physical =
                element_index.has_value()
                    ? database_.ToPhysicalElement(QualifiedName(symbol),
                                                  *element_index, raw)
                    : database_.ToPhysical(QualifiedName(symbol), raw);
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

    /** @brief 在持有XCP命令互斥锁时复核活动页及实时PAGE_PROPERTIES。 */
    std::string CheckCalibrationPageWriteLocked(
        const SymbolInfo& symbol, std::uint8_t* active_page_number) {
        if (!symbol.memory_segment.has_value() ||
            !symbol.memory_segment->has_xcp_segment)
            return "无法唯一关联有效的XCP MEMORY_SEGMENT/PAG";
        const auto& segment = *symbol.memory_segment;
        const auto active_page = master_.GetCalPage(
            calmcar::xcp::CalPageAccessMode::Xcp, segment.segment_number);
        if (!active_page.has_value()) return "无法查询XCP活动页，拒绝写入";
        const auto page_info =
            master_.GetPageInfo(segment.segment_number, active_page->page);
        if (!page_info.has_value())
            return "无法查询XCP PAGE_PROPERTIES，拒绝写入";
        const std::string reason =
            calmcar::xcp::tools::CalibrationPageWriteUnsupportedReason(
                symbol, active_page->page,
                page_info->properties.xcp_write_access);
        if (!reason.empty()) return reason;
        if (active_page_number != nullptr)
            *active_page_number = active_page->page;
        return {};
    }

    /** @brief 线程安全查询当前活动页，仅接受静态/实时DONT_CARE。 */
    std::string CheckCalibrationPageWrite(const SymbolInfo& symbol,
                                          std::uint8_t* active_page_number) {
        std::lock_guard command_lock(command_mutex_);
        return CheckCalibrationPageWriteLocked(symbol, active_page_number);
    }

    void Write(
        const std::string& name, const std::string& value_text,
        std::optional<calmcar::xcp::tools::CalibrationSelection::Snapshot>
            selection = std::nullopt,
        std::optional<std::uint64_t> element_index = std::nullopt) {
        const bool from_calibration_view = selection.has_value();
        if (selection && !CalibrationSelectionMatches(*selection)) {
            Say("标定选择已改变，取消旧写入请求。");
            return;
        }
        if (DaqRunning()) {
            if (selection)
                UpdateCalibrationState(*selection,
                                       "DAQ 运行期间禁止写入，请先 stop");
            Say("DAQ 运行期间不能 write；请先 stop。");
            return;
        }
        if (name.empty() || value_text.empty()) {
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "目标变量或物理值为空");
            Say("用法: write <module::name> <physical-value>");
            return;
        }
        auto found = database_.Find(name);
        if (!found.HasValue()) {
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "A2L 查找失败");
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        const auto symbol = found.Value();
        const auto reason =
            element_index.has_value()
                ? calmcar::xcp::tools::CurvePointWriteUnsupportedReason(
                      symbol, *element_index)
                : calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
        if (!reason.empty()) {
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "拒绝写入：" + reason);
            Say("拒绝写入: " + reason);
            return;
        }
        PhysicalValue target;
        if (!ParsePhysical(value_text, target)) {
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "目标值不是有限数值");
            Say("目标值必须是有限数值。");
            return;
        }
        auto encoded =
            element_index.has_value()
                ? database_.FromPhysicalElement(QualifiedName(symbol),
                                                *element_index, target)
                : database_.FromPhysical(QualifiedName(symbol), target);
        if (!encoded.HasValue()) {
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "目标值无法换算或编码");
            Say("目标值不可编码: " + encoded.ErrorInfo().message);
            return;
        }
        const auto session = master_.GetSessionParameters();
        const auto ag =
            static_cast<std::size_t>(session.connect.address_granularity);
        const auto encoded_reason =
            element_index.has_value()
                ? calmcar::xcp::tools::ElementRangeLayoutUnsupportedReason(
                      symbol, *element_index, 1, ag, session.connect.byte_order)
                : calmcar::xcp::tools::EncodedScalarUnsupportedReason(
                      symbol, encoded.Value().size(), ag);
        if (!encoded_reason.empty() ||
            encoded.Value().size() != symbol.element_size_bytes) {
            const std::string safe_reason = encoded_reason.empty()
                                                ? "编码宽度与元素宽度不一致"
                                                : encoded_reason;
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "拒绝写入：" + safe_reason);
            Say(safe_reason + "，拒绝写入。");
            return;
        }
        std::uint8_t preflight_page = 0;
        const std::string page_reason =
            CheckCalibrationPageWrite(symbol, &preflight_page);
        if (!page_reason.empty()) {
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "拒绝写入：" + page_reason);
            Say("拒绝写入: " + page_reason);
            return;
        }
        auto current = ReadPhysical(symbol, element_index);
        if (!current) {
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "预读失败，未写入");
            RecordObservationFailure(QualifiedName(symbol), "写入前预读失败",
                                     LocalTimestamp());
            return;
        }
        std::uint8_t post_read_page = 0;
        const std::string post_read_page_reason =
            CheckCalibrationPageWrite(symbol, &post_read_page);
        if (!post_read_page_reason.empty() ||
            post_read_page != preflight_page) {
            const std::string page_failure = post_read_page_reason.empty()
                                                 ? "预读期间XCP活动页发生变化"
                                                 : post_read_page_reason;
            if (from_calibration_view)
                UpdateCalibrationState(*selection, "拒绝写入：" + page_failure);
            RecordObservationFailure(QualifiedName(symbol), page_failure,
                                     LocalTimestamp());
            Say("拒绝写入: " + page_failure);
            return;
        }
        const std::string element_label =
            element_index ? "[" + std::to_string(*element_index) + "] " : "";
        RecordObservationValue(
            QualifiedName(symbol), element_label + PhysicalToString(*current),
            element_index ? "元素写入前预读" : "写入前预读", LocalTimestamp());
        Say("当前值: " + element_label + PhysicalToString(*current) + " " +
            symbol.conversion.unit);
        Say("目标值: " + element_label + PhysicalToString(target) + " " +
            symbol.conversion.unit);
        if (selection)
            UpdateCalibrationState(*selection, "等待确认；写入默认取消",
                                   element_label + PhysicalToString(*current) +
                                       " " + symbol.conversion.unit);
        const auto ticket = Confirm(
            "写入 " + QualifiedName(symbol) + " " + element_label + "| 当前 " +
                PhysicalToString(*current) + " | 目标 " +
                PhysicalToString(target) + " " + symbol.conversion.unit +
                "；A2L read_write 不是 ECU 许可。",
            selection.has_value());
        if (!ticket) {
            if (selection)
                UpdateCalibrationState(*selection, "已取消，未发送写入命令");
            Say("已取消写入。");
            return;
        }
        std::uint64_t write_address = symbol.xcp_address;
        if (element_index.has_value()) {
            const std::uint64_t stride =
                symbol.dimensions.empty()
                    ? symbol.element_size_bytes
                    : symbol.dimensions.front().byte_stride;
            write_address += *element_index * stride;
        }
        try {
            std::optional<PhysicalValue> actual;
            std::string dispatch_failure;
            std::string readback_failure;
            {
                std::lock_guard command_lock(command_mutex_);
                if (DaqRunning() ||
                    (selection && !CalibrationSelectionMatches(*selection))) {
                    static_cast<void>(
                        confirmation_gate_.CancelBeforeDispatch());
                    dispatch_failure = "许可已撤销、DAQ 已运行或变量选择已变化";
                } else {
                    std::uint8_t dispatch_page = 0;
                    dispatch_failure =
                        CheckCalibrationPageWriteLocked(symbol, &dispatch_page);
                    if (dispatch_failure.empty() &&
                        dispatch_page != preflight_page)
                        dispatch_failure = "确认期间XCP活动页发生变化";
                    if (!dispatch_failure.empty()) {
                        static_cast<void>(
                            confirmation_gate_.CancelBeforeDispatch());
                    } else if (!calmcar::xcp::tools::SendIfConfirmed(
                                   confirmation_gate_, *ticket, control_cancel_,
                                   [&] {
                                       master_.WriteMemoryBytes(
                                           static_cast<Address>(write_address),
                                           static_cast<AddressExtension>(
                                               symbol.address_extension),
                                           encoded.Value());
                                   })) {
                        dispatch_failure = "确认许可已撤销，未发送写入命令";
                    }
                }

                if (dispatch_failure.empty()) {
                    std::uint8_t before_readback_page = 0;
                    readback_failure = CheckCalibrationPageWriteLocked(
                        symbol, &before_readback_page);
                    if (readback_failure.empty() &&
                        before_readback_page != preflight_page)
                        readback_failure = "写入后XCP活动页发生变化";
                    if (readback_failure.empty()) {
                        actual = ReadPhysicalLocked(symbol, element_index);
                        std::uint8_t after_readback_page = 0;
                        readback_failure = CheckCalibrationPageWriteLocked(
                            symbol, &after_readback_page);
                        if (readback_failure.empty() &&
                            after_readback_page != preflight_page) {
                            actual.reset();
                            readback_failure =
                                "回读期间XCP活动页发生变化，实际值未知";
                        }
                    }
                }
            }
            if (!dispatch_failure.empty()) {
                if (from_calibration_view)
                    UpdateCalibrationState(*selection,
                                           "未发送写入：" + dispatch_failure);
                Say("未发送写入: " + dispatch_failure);
                return;
            }
            if (actual) {
                RecordObservationValue(
                    QualifiedName(symbol),
                    element_label + PhysicalToString(*actual),
                    element_index ? "元素写入后回读" : "写入后回读",
                    LocalTimestamp());
            } else {
                RecordObservationFailure(QualifiedName(symbol),
                                         readback_failure.empty()
                                             ? "写入后回读失败；实际值未知"
                                             : readback_failure,
                                         LocalTimestamp());
            }
            if (selection)
                UpdateCalibrationState(*selection,
                                       actual ? "写入命令成功，回读已完成"
                                              : "写入命令成功，但回读失败",
                                       actual ? element_label +
                                                    PhysicalToString(*actual) +
                                                    " " + symbol.conversion.unit
                                              : "回读失败；实际值未知");
            if (actual)
                Say("写入后回读: " + element_label + PhysicalToString(*actual) +
                    " " + symbol.conversion.unit);
            else
                Say("写入命令成功，但回读失败；实际值未知: " +
                    (readback_failure.empty() ? "读取失败" : readback_failure));
        } catch (const std::exception& e) {
            if (from_calibration_view)
                UpdateCalibrationState(*selection,
                                       "写入失败：" + std::string(e.what()));
            RecordObservationFailure(QualifiedName(symbol),
                                     "写入异常；可能部分修改",
                                     LocalTimestamp());
            Say("XCP 写入被拒绝或失败: " + std::string(e.what()));
        }
    }

    bool DaqRunning() const noexcept { return daq_running_.load(); }

    /** @brief 在sample与采样集合的统一锁顺序下更新仍在采样的值。 */
    void RecordWatchedObservationValue(const std::string& name,
                                       const std::string& value,
                                       const std::string& status,
                                       const std::string& timestamp) {
        {
            std::lock_guard output_lock(output_mutex_);
            std::lock_guard watched_lock(watched_mutex_);
            if (std::find(watched_.begin(), watched_.end(), name) ==
                watched_.end())
                return;
            observation_model_.UpdateValue(name, value, status, timestamp);
            const auto selection = calibration_form_.Snapshot();
            if (selection.name == name)
                static_cast<void>(
                    calibration_form_.Apply(selection, status, value));
        }
        RequestPreviewRefresh(name);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 只为仍在采样的变量登记失败，不覆盖最近成功值。 */
    void RecordWatchedObservationFailure(const std::string& name,
                                         const std::string& status,
                                         const std::string& timestamp) {
        {
            std::lock_guard output_lock(output_mutex_);
            std::lock_guard watched_lock(watched_mutex_);
            if (std::find(watched_.begin(), watched_.end(), name) ==
                watched_.end())
                return;
            observation_model_.MarkFailure(name, status, timestamp);
            const auto selection = calibration_form_.Snapshot();
            if (selection.name == name)
                static_cast<void>(calibration_form_.Apply(selection, status));
        }
        RequestPreviewRefresh(name);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

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
        std::vector<std::string> current_watch;
        {
            std::lock_guard lock(watched_mutex_);
            current_watch = watched_;
        }
        for (const auto& name : current_watch) RecordObservationPending(name);
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
                        RecordWatchedObservationFailure(name, "A2L符号查找失败",
                                                        LocalTimestamp());
                        continue;
                    }
                    auto value = ReadPhysical(found.Value());
                    const auto timestamp = LocalTimestamp();
                    if (value) {
                        RecordWatchedObservationValue(
                            name, PhysicalToString(*value), "POLL", timestamp);
                    } else {
                        RecordWatchedObservationFailure(name, "POLL读取失败",
                                                        timestamp);
                    }
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
        const auto ticket = Confirm(
            "即将 FREE_DAQ 清空 ECU 所有动态 DAQ 配置（不可恢复）；profile=" +
            options_.daq_profile + "，观测变量 " +
            std::to_string(names.size()) +
            " 个。能力查询及布局检查已通过。继续? [Y/N]");
        if (!ticket) {
            Say("已取消；没有发送 FREE_DAQ。");
            return;
        }

        bool samples_pending = false;
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
            if (!calmcar::xcp::tools::SendIfConfirmed(
                    confirmation_gate_, *ticket, control_cancel_,
                    [this] { measurement_session_.Prepare(); })) {
                Say("FREE_DAQ 前许可已撤销；未发送破坏性配置命令。");
                return;
            }
            if (control_cancel_.load() || worker_stop_.load()) {
                Say("配置已执行但收到停止请求；不启动 "
                    "DAQ（已发送的命令不可撤销）。");
                return;
            }
            for (const auto& name : names) RecordObservationPending(name);
            samples_pending = true;
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
            if (samples_pending) {
                for (const auto& name : names)
                    RecordObservationFailure(name, "DAQ启动失败",
                                             LocalTimestamp());
            }
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
                const auto timestamp = LocalTimestamp();
                std::ostringstream status;
                status << "DAQ list=" << frame.daq_list
                       << " ODT=" << static_cast<unsigned>(frame.odt);
                if (frame.timestamp_valid)
                    status << " t=" << frame.timestamp.count() << "ns";
                else if (frame.timestamp_raw != 0U)
                    status << " timestamp_raw=" << frame.timestamp_raw;
                if (!sample.valid) {
                    RecordWatchedObservationFailure(
                        sample.name, "DAQ样本无效 | " + status.str(),
                        timestamp);
                    continue;
                }
                std::ostringstream value_text;
                std::visit(
                    [&](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, std::string>)
                            value_text << value;
                        else
                            value_text << std::setprecision(15) << value;
                    },
                    sample.value);
                RecordWatchedObservationValue(sample.name, value_text.str(),
                                              status.str(), timestamp);
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

    /** @brief 只将采样集合标记为停止，手动读取值保持原有状态。 */
    void MarkObservationsStopped() {
        std::vector<std::string> stopped_names;
        {
            std::lock_guard lock(watched_mutex_);
            stopped_names = watched_;
        }
        {
            std::lock_guard lock(output_mutex_);
            for (const auto& name : stopped_names)
                observation_model_.MarkStopped(name);
        }
        RequestPreviewRefresh({}, true);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    void StopWatch() {
        if (options_.mode == "daq") {
            const bool was_running =
                daq_running_.exchange(false) || measurement_session_.Running();
            if (measurement_session_.Running()) {
                try {
                    measurement_session_.Stop();
                } catch (const std::exception& e) {
                    Say("DAQ 停止错误: " + std::string(e.what()));
                }
            }
            StopFrameOutput();
            if (was_running) MarkObservationsStopped();
            return;
        }
        if (!watching_.exchange(false)) {
            if (watch_thread_.joinable()) watch_thread_.join();
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
            if (!listing_active_ && result_lines_.Size() == 512U)
                viewport_.EvictFront(1);
            if (log_lines_.Size() == 512U) viewport_.EvictFront(2);
            result_lines_.Append(text);
            log_lines_.Append(text);
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
    mutable calmcar::xcp::tools::PanelViewport viewport_{
        3}; /**< 结果/日志两面板独立滚动窗口；受 output_mutex_ 保护。 */
    std::vector<std::string>
        listing_rows_;           /**< list all 完整、可回看的目录快照。 */
    bool listing_active_{false}; /**< 结果视图当前是否显示完整目录。 */
    calmcar::xcp::tools::CalibrationFormModel
        calibration_form_; /**< 绑定模态详情的选择、草稿、回读及状态。 */
    calmcar::xcp::tools::VariableModalState
        variable_modal_; /**< 当前变量详情或危险确认页面。 */
    std::optional<SymbolInfo>
        modal_symbol_info_;                /**< 模态打开时固定的A2L符号快照。 */
    std::string modal_element_index_text_; /**< 0基数组元素/曲线点索引草稿。 */
    std::uint64_t modal_array_page_start_{0}; /**< 数组分页的0基起始索引。 */
    std::string modal_array_page_lines_{
        "尚未读取本页。"};              /**< 当前页读取结果。 */
    bool modal_visible_{false};         /**< FTXUI模态覆盖层是否显示。 */
    int modal_page_selected_{0};        /**< 详情页0、确认页1。 */
    TuiLayoutMetrics current_layout_{}; /**< UI线程计算的最近布局。 */
    std::atomic<bool> terminal_too_small_{
        true}; /**< 尺寸不足时禁止危险操作。 */
    mutable calmcar::xcp::tools::LatestObservationModel observation_model_{
        512}; /**< 按规范符号名保存的最新观测/读取快照。 */
    mutable calmcar::xcp::tools::BoundedLineBuffer result_lines_{
        512}; /**< 命令结果视图的有界显示行。 */
    mutable calmcar::xcp::tools::BoundedLineBuffer log_lines_{
        512};                                 /**< 日志视图的有界显示行。 */
    std::vector<SymbolInfo> preview_symbols_; /**< 当前A2L搜索候选快照。 */
    std::unordered_map<std::string, std::size_t>
        preview_symbol_index_; /**< 规范名到A2L候选快照索引。 */
    std::vector<std::string>
        preview_visible_names_; /**< 当前过滤后的菜单名称。 */
    std::unordered_map<std::string, std::size_t>
        preview_visible_index_;               /**< 规范名到预览菜单行号。 */
    std::vector<std::string> preview_labels_; /**< 变量预览菜单文本。 */
    std::vector<std::string> view_labels_{"变量预览", "命令结果",
                                          "日志"}; /**< 顶部三视图标签。 */
    std::vector<std::string> preview_scope_labels_{
        "全部变量", "仅采样"};          /**< 变量预览范围筛选。 */
    int view_selected_{0};              /**< 当前选中视图：预览、结果、日志。 */
    int preview_selected_{0};           /**< 当前变量预览菜单行。 */
    int preview_scope_selected_{0};     /**< 0=全部变量，1=仅采样。 */
    std::string preview_filter_;        /**< 当前变量名搜索输入框草稿。 */
    std::string preview_active_filter_; /**< 当前结果使用的实际筛选。 */
    calmcar::xcp::tools::SearchInputRevision
        preview_input_revision_; /**< 防止旧搜索结果覆盖新草稿。 */
    std::optional<PendingPreviewSearchResult>
        pending_preview_search_result_; /**< Modal关闭后应用候选列表快照。 */
    std::string preview_status_{"正在加载A2L变量…"}; /**< 搜索结果/表格提示。 */
    std::mutex preview_refresh_mutex_; /**< 合并后台采样的UI刷新请求。 */
    std::unordered_set<std::string>
        preview_refresh_pending_;     /**< 待更新行名。 */
    bool preview_refresh_all_{false}; /**< 待重建完整变量列表。 */
    std::atomic<bool> preview_refresh_posted_{
        false}; /**< 刷新事件是否已排队。 */
    std::atomic<std::uint64_t> preview_search_generation_{
        0};                    /**< 忽略过期搜索结果。 */
    std::string command_text_; /**< 固定命令输入框内容。 */
    calmcar::xcp::tools::CommandHistory command_history_{
        200}; /**< 最多保留 200 条命令历史。 */
    ftxui::ScreenInteractive* screen_{
        nullptr};                    /**< 当前交互界面，供后台事件唤醒使用。 */
    std::mutex command_queue_mutex_; /**< 串行命令队列互斥量。 */
    std::condition_variable command_cv_;       /**< 命令线程等待通知。 */
    std::deque<CommandRequest> command_queue_; /**< 待执行命令队列。 */
    std::thread command_worker_;               /**< 串行执行命令的后台线程。 */
    std::atomic<bool> worker_stop_{false};     /**< 命令线程停止标志。 */
    std::atomic<bool> control_cancel_{
        false};                       /**< stop/quit 请求已撤销危险操作。 */
    std::string confirmation_prompt_; /**< 当前确认操作的完整提示文本。 */
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
