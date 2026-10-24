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
#include "symbol_display.hpp"
#include "transport_selection.hpp"
#include "tui_model.hpp"
#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/a2l/ia2l_database.hpp"
#include "libxcp/measurement/measurement_planner.hpp"
#include "libxcp/measurement/measurement_session.hpp"
#include "libxcp/log.hpp"
#include "libxcp/tcp_transport.hpp"
#include "libxcp/udp_transport.hpp"
#include "a2l/a2l_measurement_database.hpp"

#if LIBXCP_ENABLE_LOG
#include "program_logging.hpp"
#endif
#include "a2l/measurement_runtime_profile.hpp"
#include "libxcp/xcp_master.hpp"
#include "symbol_policy.hpp"

namespace {

/** @brief 数组详情每页读取的连续元素数量。 */
constexpr std::uint64_t kArrayPageSize = 8U;

/** @brief 单帧界面构建耗时超过该值（毫秒）时才逐帧写日志。 */
constexpr double kFramePerfWarnMs = 50.0;

/** @brief 界面渲染汇总日志的输出周期（秒）。 */
constexpr double kFramePerfWindowSeconds = 1.0;

/**
 * @brief 轻量单调时钟计时器，用于性能日志。
 *
 * 只依赖 steady_clock，不参与业务逻辑；拷贝语义由编译器生成，
 * 拷贝后两个对象都从各自构造时刻或拷贝时刻继续计时。
 */
class PerfStopwatch {
public:
    /** @brief 从当前时刻开始计时。 */
    PerfStopwatch() : started_(std::chrono::steady_clock::now()) {}

    /**
     * @brief 返回自计时起点以来的毫秒数。
     * @return 以毫秒为单位、单调递增的经过时间。
     */
    [[nodiscard]] double ElapsedMs() const {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - started_)
            .count();
    }

    /**
     * @brief 返回自计时起点以来的秒数。
     * @return 以秒为单位、单调递增的经过时间。
     */
    [[nodiscard]] double ElapsedSeconds() const {
        return ElapsedMs() / 1000.0;
    }

private:
    std::chrono::steady_clock::time_point started_; /**< 计时起点。 */
};

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
using calmcar::xcp::BuildMeasurementRuntimeProfile;
using calmcar::xcp::ByteCount;
using calmcar::xcp::Bytes;
using calmcar::xcp::IdentificationFieldType;
using calmcar::xcp::MeasurementDaqCapabilities;
using calmcar::xcp::MeasurementFrame;
using calmcar::xcp::MeasurementPlanner;
using calmcar::xcp::MeasurementRuntimeProfile;
using calmcar::xcp::MeasurementSession;
using calmcar::xcp::XcpMaster;
using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::CharacteristicType;
using calmcar::xcp::a2l::PhysicalValue;
using calmcar::xcp::a2l::SymbolInfo;
using calmcar::xcp::a2l::SymbolKind;
using calmcar::xcp::tools::BuildPreviewRows;
using calmcar::xcp::tools::PreviewRow;
using calmcar::xcp::tools::PreviewRowSource;
using calmcar::xcp::tools::TuiLayoutMetrics;

struct Options {
    std::string a2l_path;
    std::string host;
    std::uint16_t port{0};
    std::string module;
    std::chrono::milliseconds interval{500};
    std::string mode{"poll"};
    bool interval_explicit{false};

    /**
     * @brief 传输层选择的原始请求（B-T5 §8.1）
     * @details 这里保存的是**用户原样输入**，尚未与 A2L 事实合并。合并由
     *          `calmcar::xcp::tools::ResolveTransportSelection()` 完成，
     *          从而"用户给了什么"与"最终怎么连"可以分开断言。
     */
    calmcar::xcp::tools::TransportSelectionRequest transport_request;
};

void PrintUsage(std::ostream& out) {
    out << "Usage: xcp_master --a2l <path> --host <IPv4> --port <1..65535> "
           "[--module <name>] [--mode poll|daq] [--interval-ms <ms>]\n"
           "                  [--transport udp|tcp] [--tcp-profile "
           "asam-v1.5|asam-v1.4] [--packet-alignment 8|16|32]\n"
           "                  [--protocol-version 1.2|1.3|1.4|1.5] "
           "[--transport-version 1.4|1.5] [--preset xcplite]\n"
           "       xcp_master --help\n"
           "poll: --interval-ms is the target polling cycle (default 500 ms).\n"
           "daq: event-driven; --interval-ms is invalid. The DTO layout is "
           "selected automatically from the slave's reported identification "
           "field type (DAQ_KEY_BYTE); no profile option is needed.\n"
           "tcp: --tcp-profile is mandatory and never inferred from the A2L or "
           "from version numbers. asam-v1.5 additionally needs "
           "--packet-alignment (or an explicit A2L PACKET_ALIGNMENT_x).\n"
           "--host/--port may be omitted for tcp when the A2L declares "
           "XCP_ON_TCP_IP.\n";
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
    // 是否显式写下了 --transport：只有显式写作才能压过 --preset 隐含的 tcp，
    // 且判断放在循环之后，避免"选项顺序不同、结果不同"的顺序依赖。
    bool transport_explicit = false;
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
        } else if (key == "--transport") {
            const auto parsed = calmcar::xcp::tools::ParseTransportKind(value);
            if (!parsed.has_value()) {
                error = "--transport must be udp or tcp";
                return std::nullopt;
            }
            options.transport_request.transport = *parsed;
            transport_explicit = true;
        } else if (key == "--tcp-profile") {
            const auto parsed =
                calmcar::xcp::tools::ParseTcpWireProfileText(value);
            if (!parsed.has_value()) {
                error = "--tcp-profile must be asam-v1.5 or asam-v1.4";
                return std::nullopt;
            }
            options.transport_request.tcp_profile = *parsed;
        } else if (key == "--packet-alignment") {
            const auto parsed =
                calmcar::xcp::tools::ParseAlignmentBitsText(value);
            if (!parsed.has_value()) {
                error = "--packet-alignment must be 8, 16 or 32";
                return std::nullopt;
            }
            options.transport_request.packet_alignment_bits = *parsed;
        } else if (key == "--protocol-version") {
            const auto parsed = calmcar::xcp::tools::ParseVersionText(value);
            if (!parsed.has_value()) {
                error = "--protocol-version must be a version like 1.2";
                return std::nullopt;
            }
            options.transport_request.protocol_version = *parsed;
        } else if (key == "--transport-version") {
            const auto parsed = calmcar::xcp::tools::ParseVersionText(value);
            if (!parsed.has_value()) {
                error = "--transport-version must be a version like 1.4";
                return std::nullopt;
            }
            options.transport_request.transport_version = *parsed;
        } else if (key == "--preset") {
            if (value != "xcplite") {
                error = "--preset only supports xcplite";
                return std::nullopt;
            }
            options.transport_request.preset_xcplite = true;
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

    // ---- 传输层选择的收尾（B-T5 §8.1） ------------------------------------
    const bool host_given = !options.host.empty();
    const bool port_given = options.port != 0;
    // 1) preset 隐含 tcp：`--preset xcplite` 描述的是 XCPlite 的 TCP 部署事实。
    //    若调用方同行写了 `--transport udp`，交给解析层报冲突而不是在这里
    //    悄悄改掉用户的显式选择。
    if (options.transport_request.preset_xcplite && !transport_explicit) {
        options.transport_request.transport =
            calmcar::xcp::tools::TransportKind::Tcp;
    }
    // 2) host/port 交给解析层成对校验：UDP 仍要求两者齐全（既有契约），
    //    TCP 允许两者都缺并在 A2L 中寻找 XCP_ON_TCP_IP 端点。
    if (host_given) {
        options.transport_request.host = options.host;
    }
    if (port_given) {
        options.transport_request.port = options.port;
    }
    if (options.a2l_path.empty()) {
        error = "--a2l is required";
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
           std::vector<calmcar::xcp::A2lEventMetadata> events,
           calmcar::xcp::tools::CalibrationPageFacts calibration_page_facts = {})
        : options_(options),
          database_(database),
          measurement_database_(measurement_database),
          measurement_session_(measurement_session),
          master_(master),
          calibration_page_facts_(calibration_page_facts),
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
        preview_options.on_enter = [this] { ActivatePreviewRow(); };
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

        InputOption calibration_index_options;
        calibration_index_options.placeholder = "0基数组元素/曲线点索引";
        auto calibration_element_index_input =
            Input(&modal_element_index_text_, calibration_index_options);
        InputOption target_options;
        target_options.placeholder = "目标物理值（仅VALUE或单曲线点）";
        auto target_input = Input(&calibration_form_.target, target_options);
        auto calibration_read_button =
            Button("读回当前值", [this] { SubmitCalibrationRead(); });
        auto write_button =
            Button("申请写入…", [this] { SubmitCalibrationWrite(); });
        auto close_button =
            Button("返回预览", [this] { CloseVariableModal(); });
        auto close_calibration_button =
            Button("取消标定", [this] { CloseVariableModal(); });
        auto calibration_element_index_label =
            Renderer([] { return text("元素索引(0基)："); });
        auto calibration_element_index_control =
            Maybe(Container::Horizontal({calibration_element_index_label,
                                         calibration_element_index_input}),
                  [this] {
                      return !terminal_too_small_.load() &&
                             variable_modal_.Page() ==
                                 calmcar::xcp::tools::VariableModalPage::
                                     Calibration &&
                             modal_symbol_info_.has_value() &&
                             RequiresElementIndex(*modal_symbol_info_);
                  });
        auto target_label = Renderer([] { return text("目标值："); });
        auto target_control =
            Maybe(Container::Horizontal({target_label, target_input}), [this] {
                return calmcar::xcp::tools::CalibrationCommandPermitted(
                    terminal_too_small_.load(), variable_modal_.Page(),
                    modal_symbol_info_.has_value(),
                    ModalWriteUnsupportedReason());
            });
        auto calibration_read_control = Maybe(calibration_read_button, [this] {
            return calmcar::xcp::tools::CalibrationCommandPermitted(
                terminal_too_small_.load(), variable_modal_.Page(),
                modal_symbol_info_.has_value(), ModalReadUnsupportedReason());
        });
        auto write_control = Maybe(write_button, [this] {
            return calmcar::xcp::tools::CalibrationCommandPermitted(
                terminal_too_small_.load(), variable_modal_.Page(),
                modal_symbol_info_.has_value(), ModalWriteUnsupportedReason());
        });
        auto details_container = Container::Vertical({close_button});
        auto calibration_container =
            Container::Vertical({calibration_element_index_control,
                                 target_control, calibration_read_control,
                                 write_control, close_calibration_button});
        auto details_renderer = Renderer(details_container, [this,
                                                             close_button] {
            const auto layout = current_layout_;
            if (!modal_symbol_info_.has_value())
                return text("没有可显示的 A2L 元数据") | border;

            const auto& symbol = *modal_symbol_info_;
            const auto description =
                calmcar::xcp::tools::DescribeSymbolType(symbol);
            const auto read_reason =
                calmcar::xcp::tools::SymbolDetailReadUnsupportedReason(symbol);
            const auto write_reason =
                calmcar::xcp::tools::SymbolStaticWriteUnsupportedReason(symbol);

            std::string dimensions = "标量";
            if (!symbol.dimensions.empty()) {
                dimensions.clear();
                for (const auto& dimension : symbol.dimensions) {
                    if (!dimensions.empty()) dimensions += " × ";
                    dimensions += std::to_string(dimension.extent);
                }
            }
            std::string axis = "固定轴：无";
            if (symbol.fixed_curve_axis.has_value()) {
                const auto& fixed_axis = *symbol.fixed_curve_axis;
                axis =
                    "固定轴：点数=" + std::to_string(fixed_axis.point_count) +
                    "，偏移=" + std::to_string(fixed_axis.offset) +
                    "，步长=" + std::to_string(fixed_axis.distance);
            }
            const std::string read_status = read_reason.empty()
                                                ? "通过静态布局策略"
                                                : "拒绝：" + read_reason;
            const std::string write_status =
                write_reason.empty()
                    ? "静态候选；实际写入仍须在独立标定流程检查活动页并确认"
                    : "拒绝：" + write_reason;
            const auto warning =
                layout.too_small
                    ? text("终端尺寸不足；确认操作已禁用。") | color(Color::Red)
                    : text(
                          "本页只显示 A2L 元数据与静态策略，不访问 ECU。"
                          "观测请返回预览列表；标定请返回预览列表按 F4。");

            return vbox(
                       {text("A2L 元数据 · " + QualifiedName(symbol) +
                             "    [Esc 返回]") |
                            bold | color(Color::Cyan),
                        separator(),
                        text("对象类别：" + KindName(symbol) + " | 基础类型：" +
                             description.base_type),
                        text("数据形态：" + description.data_shape +
                             " | 维度：" + dimensions + " | 单元素宽度：" +
                             description.element_width),
                        text("单位：" + symbol.conversion.unit),
                        text("XCP 地址：" + std::to_string(symbol.xcp_address) +
                             " | 地址扩展：" +
                             std::to_string(symbol.address_extension)),
                        text(axis), text("读取静态策略：" + read_status),
                        text("标定静态策略：" + write_status), warning,
                        close_button->Render()}) |
                   border |
                   size(WIDTH, LESS_THAN, std::max(1, layout.modal_width)) |
                   size(HEIGHT, LESS_THAN, std::max(1, layout.modal_height));
        });

        auto calibration_renderer = Renderer(
            calibration_container, [this, calibration_element_index_control,
                                    target_control, calibration_read_control,
                                    write_control, close_calibration_button] {
                const auto layout = current_layout_;
                if (!modal_symbol_info_.has_value())
                    return text("没有可标定的变量") | border;
                const auto& symbol = *modal_symbol_info_;
                const auto name = QualifiedName(symbol);
                const auto description =
                    calmcar::xcp::tools::DescribeSymbolType(symbol);
                const auto write_reason = ModalWriteUnsupportedReason();
                const auto read_reason = ModalReadUnsupportedReason();
                const auto write_status = write_reason.empty()
                                              ? "满足候选条件；仍需明确确认"
                                              : "拒绝：" + write_reason;
                // 标定表单的当前值/状态由工作线程在 output_mutex_ 保护下更新，
                // 渲染必须取快照，避免读到正在被改写的 std::string。
                std::string form_current;
                std::string form_status;
                std::string form_target;
                {
                    std::lock_guard lock(output_mutex_);
                    form_current = calibration_form_.current;
                    form_status = calibration_form_.status;
                    form_target = calibration_form_.target;
                }
                // 行数随状态变化：越界提示只在需要时插入，避免空白占位行。
                std::vector<Element> lines;
                lines.push_back(text("独立标定流程 · " + name +
                                     "    [Esc 返回]") |
                                bold | color(Color::Yellow));
                lines.push_back(separator());
                lines.push_back(text("类别：" + KindName(symbol) +
                                     " | 基础类型：" + description.base_type));
                lines.push_back(text("形态：" + description.data_shape +
                                     " | 单元素宽度：" +
                                     description.element_width));
                lines.push_back(text("当前值：" + form_current));
                lines.push_back(
                    text("允许范围：" +
                         calmcar::xcp::tools::CalibrationLimitText(symbol)));
                // 目标值一旦越出 A2L 限值就即时提示（B-15）：此前越界只会
                // 在按下「申请写入」后写进主界面面板，而模态窗口正遮住它。
                const auto limit_hint =
                    calmcar::xcp::tools::CalibrationTargetLimitHint(
                        symbol, form_target);
                if (!limit_hint.empty())
                    lines.push_back(text(limit_hint) | color(Color::Red));
                lines.push_back(text("状态：" + form_status) |
                                color(Color::Cyan));
                lines.push_back(
                    text("读取策略：" + (read_reason.empty()
                                             ? "可用"
                                             : "拒绝：" + read_reason)));
                lines.push_back(text("写入资格：" + write_status));
                // P2：把页判定依据显式写出来，避免"A2L 缺段页声明"被误读成"不可写"。
                const auto page_note = ModalCalibrationPageNote();
                if (!page_note.empty())
                    lines.push_back(text("页判定依据：" + page_note) |
                                    color(Color::GrayLight));
                lines.push_back(calibration_element_index_control->Render());
                lines.push_back(target_control->Render());
                lines.push_back(
                    hbox({calibration_read_control->Render(), text("  "),
                          write_control->Render(), text("  "),
                          close_calibration_button->Render()}));
                lines.push_back(
                    text("标定值只在确认页按 Y 后发送；N/Esc 取消。") |
                    color(Color::Yellow));
                return vbox(std::move(lines)) | border |
                       size(WIDTH, LESS_THAN, std::max(1, layout.modal_width)) |
                       size(HEIGHT, LESS_THAN,
                            std::max(1, layout.modal_height));
            });

        auto confirmation_renderer = Renderer([this] {
            const auto layout = current_layout_;
            return vbox({text("危险操作确认 · 默认取消") | bold |
                             color(Color::Yellow),
                         separator(), paragraph(confirmation_prompt_),
                         text("Y/y：明确确认；N/"
                              "n、Esc：取消；变量写入返回标定表单，命令确认返回"
                              "界面"
                              "。"),
                         text("Enter、其它按键及窗口过小都不会发送操作；F5取消"
                              "并停止。")}) |
                   border |
                   size(WIDTH, LESS_THAN, std::max(1, layout.modal_width)) |
                   size(HEIGHT, LESS_THAN, std::max(1, layout.modal_height));
        });
        auto modal_pages = Container::Tab(
            {details_renderer, calibration_renderer, confirmation_renderer},
            &modal_page_selected_);

        // 根组件按模式隔离事件，预览区的 Tab 不可抵达命令输入。
        auto preview_area = Container::Vertical({view_menu, view_content});
        auto root = Container::Tab({preview_area, command_input},
                                   input_mode_.Selector());
        preview_menu->TakeFocus();
        auto renderer = Renderer(root, [&] {
            // 性能观测：统计单帧「元素树构建 + 菜单条目生成」耗时。
            const PerfStopwatch frame_stopwatch;
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
                    ? "DAQ/自动布局"
                    : "POLL/目标 " + std::to_string(options_.interval.count()) +
                          " ms";
            const bool compact = current_layout_.compact;
            std::string help;
            if (modal_visible_ &&
                variable_modal_.Page() ==
                    calmcar::xcp::tools::VariableModalPage::Confirmation) {
                help = compact
                           ? "Y确认 | N/Esc取消返回 | F5取消并停止"
                           : "Y 确认 | N/Esc 取消后返回发起页 | F5 取消并停止";
            } else if (modal_visible_) {
                help = compact ? "Tab切换 | Esc关闭 | F5停止"
                               : "Tab 弹窗内切换 | Enter 激活 | Esc 关闭详情 | "
                                 "F5 停止";
            } else if (input_mode_.InCommandMode()) {
                help =
                    compact
                        ? "Enter执行 | ↑↓历史 | Esc预览 | F2视图 | F5停止"
                        : "Enter 执行 | ↑↓ 命令历史 | Esc 返回预览（保留草稿）"
                          " | F2 切换视图 | F5 停止 | quit 退出";
            } else if (view_selected_ != 0) {
                help = compact ? "F2视图 | F7命令 | PgUp/Dn翻页 | F5停止"
                               : "F2 切换三视图 | F7 输入命令 | PgUp/PgDn "
                                 "浏览 | F5 停止";
            } else if (preview_menu->Focused()) {
                help = compact
                           ? "Tab焦点 | Enter展开 | F3详情 | F4标定 | F6观测 | "
                             "F7命令"
                           : "Tab 预览内焦点 | Enter 展开/读取 | F3 只读详情 "
                             "| F4 标定 | F6 切换观测成员 | F7 输入命令";
            } else if (search_input->Focused()) {
                help = compact ? "Tab焦点 | Enter搜索 | F7命令 | F5停止"
                               : "Tab 预览内焦点 | Enter 搜索A2L元数据 | "
                                 "F7 输入命令 | F5 停止";
            } else if (scope_menu->Focused()) {
                help = compact ? "Tab焦点 | ←→范围 | F7命令 | F5停止"
                               : "Tab 预览内焦点 | ←→ 选择全部/已采样 | "
                                 "F7 输入命令 | F5 停止";
            } else {
                help = compact ? "F2视图 | Tab焦点 | F7命令 | F5停止"
                               : "F2 切换三视图 | Tab 在预览内切换焦点 | "
                                 "F7 输入命令 | F5 停止";
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
            auto frame_element =
                vbox({status_bar, main_panel | flex, separator(),
                      hbox({text(input_mode_.InCommandMode()
                                     ? "命令 > "
                                     : "命令（按 F7 输入）> "),
                            command_input->Render()}),
                      hbox({dimensions, text("  |  "), text(help)})}) |
                border;
            // 性能观测：构建耗时超过阈值才逐帧记录，避免日志自身拖慢界面。
            NoteFrameRendered(frame_stopwatch.ElapsedMs(),
                              preview_symbols_.size(),
                              preview_labels_.size());
            return frame_element;
        });
        auto ui = Modal(renderer, modal_pages, &modal_visible_);
        ui = CatchEvent(ui, [this, command_input, search_input, scope_menu,
                             preview_menu, view_menu](Event event) {
            if (modal_visible_) {
                if (event == Event::F2 || event == Event::F3 ||
                    event == Event::F4 || event == Event::F6 ||
                    event == Event::F7)
                    return true;
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
            // 安全停止及模式切换优先于输入组件，且不会提交当前草稿。
            if (event == Event::F5) {
                SubmitCommand("stop");
                return true;
            }
            if (event == Event::F7) {
                input_mode_.EnterCommandMode();
                command_input->TakeFocus();
                return true;
            }
            if (event == Event::Escape) {
                if (input_mode_.InCommandMode()) {
                    input_mode_.LeaveCommandMode(view_selected_);
                    preview_menu->TakeFocus();
                }
                return true;
            }
            if (input_mode_.AllowsPreviewListAction(view_selected_,
                                                    preview_menu->Focused())) {
                if (event == Event::F6) {
                    TogglePreviewWatch();
                    return true;
                }
                if (event == Event::F4) {
                    OpenCalibration();
                    return true;
                }
                if (event == Event::F3) {
                    OpenPreviewDetails();
                    return true;
                }
            }
            // 菜单自身会消费 Tab，因此由顶层显式实现预览三控件环路。
            if (!input_mode_.InCommandMode() && view_selected_ == 0 &&
                (event == Event::Tab || event == Event::TabReverse)) {
                using calmcar::xcp::tools::PreviewFocusTarget;
                auto current = PreviewFocusTarget::List;
                if (search_input->Focused())
                    current = PreviewFocusTarget::Search;
                else if (scope_menu->Focused())
                    current = PreviewFocusTarget::Scope;
                else if (!preview_menu->Focused()) {
                    preview_menu->TakeFocus();
                    return true;
                }
                const auto next = calmcar::xcp::tools::NextPreviewFocusTarget(
                    current, event == Event::TabReverse);
                switch (next) {
                    case PreviewFocusTarget::Search:
                        search_input->TakeFocus();
                        break;
                    case PreviewFocusTarget::Scope:
                        scope_menu->TakeFocus();
                        break;
                    case PreviewFocusTarget::List:
                        preview_menu->TakeFocus();
                        break;
                }
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
            if (input_mode_.InCommandMode() && command_input->Focused() &&
                event == Event::ArrowUp) {
                if (auto previous = command_history_.Previous(command_text_))
                    command_text_ = std::move(*previous);
                return true;
            }
            if (input_mode_.InCommandMode() && command_input->Focused() &&
                event == Event::ArrowDown) {
                if (auto next = command_history_.Next())
                    command_text_ = std::move(*next);
                return true;
            }
            if (event == Event::F2) {
                view_selected_ = calmcar::xcp::tools::NextViewIndex(
                    view_selected_, view_labels_.size());
                if (!input_mode_.InCommandMode()) {
                    if (view_selected_ == 0)
                        preview_menu->TakeFocus();
                    else
                        view_menu->TakeFocus();
                }
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
        const std::string type_label =
            calmcar::xcp::tools::FormatSymbolTypeLabel(symbol, KindName(symbol),
                                                       current_layout_.compact);
        std::string label = current_layout_.compact
                                ? type_label + " | " + name + " | "
                                : name + " | " + type_label + " | ";
        if (sample.has_value() && sample->has_value) {
            if (sample->is_array) {
                label += "[" + std::to_string(sample->array_values.size()) +
                         " elements]";
            } else {
                label += sample->value;
            }
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
        const auto observation_reason = ObservationUnsupportedReason(symbol);
        if (!observation_reason.empty()) {
            // 标定量本就不属于周期采集范畴，用中性表述；其余是真正的
            // 观测缺陷，保留「观测拒绝」以维持警示强度。
            label += symbol.kind == SymbolKind::Characteristic
                         ? " | 不适用周期采集: " + observation_reason
                         : " | 观测拒绝: " + observation_reason;
        }
        if (symbol.kind != SymbolKind::Measurement) {
            const auto write_reason =
                calmcar::xcp::tools::SymbolWriteUnsupportedReason(symbol);
            if (write_reason.empty()) {
                // 明确区分「可标定」与「可周期采集」两件事：标定量既不能
                // 被 DAQ 采集，也仍然可以通过标定表单按需读写。
                label += symbol.kind == SymbolKind::Characteristic
                             ? " | 可标定（可标定表单读写）"
                             : " | 可标量候选";
            } else {
                label += " | 写入拒绝: " + write_reason;
            }
        }
        return label;
    }

    /** @brief 格式化展开的标量观测子行。 */
    std::string FormatExpandedScalarValue(
        const SymbolInfo& symbol,
        const calmcar::xcp::tools::ObservationSnapshot& sample) const {
        std::string label = "  └ 值：" + sample.value;
        if (!symbol.conversion.unit.empty())
            label += " " + symbol.conversion.unit;
        if (!sample.timestamp.empty()) label += " | " + sample.timestamp;
        if (sample.stale || sample.stopped) label += " | 旧样本";
        return label;
    }

    /** @brief 格式化展开的数组元素子行。 */
    std::string FormatExpandedArrayValue(const SymbolInfo& symbol,
                                         std::size_t index,
                                         const std::string& value,
                                         bool stale) const {
        std::string label = "  └ [" + std::to_string(index) + "] = " + value;
        if (!symbol.conversion.unit.empty())
            label += " " + symbol.conversion.unit;
        if (stale) label += " | 旧样本";
        return label;
    }

    /** @brief 在界面线程重建父行/动作行/数据行并保留行身份。 */
    void RebuildPreviewLabels() {
        // 性能观测：整表重建是 O(候选数) 的热路径，耗时直接决定界面响应。
        const PerfStopwatch stopwatch;
        const auto old_selected_row =
            preview_selected_ >= 0 &&
                    static_cast<std::size_t>(preview_selected_) <
                        preview_rows_.size()
                ? preview_rows_[preview_selected_]
                : PreviewRow{};
        const auto old_selected_index = preview_selected_;
        preview_labels_.clear();
        preview_visible_names_.clear();
        preview_visible_index_.clear();
        preview_rows_.clear();
        std::vector<PreviewRowSource> row_sources;
        std::unordered_map<
            std::string,
            std::optional<calmcar::xcp::tools::ObservationSnapshot>>
            samples_by_name;
        std::unordered_map<std::string, bool> watched_by_name;
        std::unordered_map<std::string, std::string> read_reasons_by_name;
        std::lock_guard lock(output_mutex_);
        std::lock_guard watched_lock(watched_mutex_);
        for (const auto& symbol : preview_symbols_) {
            const auto name = QualifiedName(symbol);
            const bool watched = std::find(watched_.begin(), watched_.end(),
                                           name) != watched_.end();
            if (preview_scope_selected_ == 1 && !watched) continue;
            const auto sample = observation_model_.Find(name);
            const bool expanded = expanded_preview_names_.contains(name);
            const bool has_sample = sample.has_value() && sample->has_value;
            const auto read_reason = PreviewReadUnsupportedReason(symbol);
            const bool manual_read_available =
                (!watching_.load() || !watched) && !DaqRunning() &&
                read_reason.empty();
            row_sources.push_back(
                {name, expanded, has_sample, has_sample && sample->is_array,
                 has_sample && sample->is_array ? sample->array_values.size()
                                                : 0U,
                 !symbol.dimensions.empty(), manual_read_available});
            samples_by_name.emplace(name, sample);
            watched_by_name.emplace(name, watched);
            read_reasons_by_name.emplace(name, read_reason);
        }

        preview_rows_ = BuildPreviewRows(row_sources);
        for (const auto& row : preview_rows_) {
            preview_visible_names_.push_back(row.symbol_name);
            const auto symbol_index =
                preview_symbol_index_.find(row.symbol_name);
            if (symbol_index == preview_symbol_index_.end()) {
                preview_labels_.push_back(row.symbol_name + " | A2L候选已失效");
                continue;
            }
            const auto& symbol = preview_symbols_[symbol_index->second];
            const auto sample_it = samples_by_name.find(row.symbol_name);
            const auto watched_it = watched_by_name.find(row.symbol_name);
            const auto reason_it = read_reasons_by_name.find(row.symbol_name);
            const std::optional<calmcar::xcp::tools::ObservationSnapshot>
                sample = sample_it == samples_by_name.end() ? std::nullopt
                                                            : sample_it->second;
            const bool watched =
                watched_it != watched_by_name.end() && watched_it->second;
            if (!row.is_child) {
                preview_visible_index_.try_emplace(row.symbol_name,
                                                   preview_labels_.size());
                preview_labels_.push_back(
                    FormatPreviewLabel(symbol, sample, watched));
            } else if (row.is_read_action) {
                preview_labels_.push_back(
                    "  └ [单次读取] 按 Enter 读取完整变量（不加入观测）");
            } else if (row.is_placeholder) {
                std::string status;
                if (sample.has_value() && !sample->status.empty()) {
                    status = sample->status;
                } else if (watched && watching_.load()) {
                    status = "等待首次完整样本";
                } else if (reason_it != read_reasons_by_name.end() &&
                           !reason_it->second.empty()) {
                    status = "读取不可用: " + reason_it->second;
                } else if (watched) {
                    status = "观测已停止；尚无样本";
                } else {
                    status = "尚无样本";
                }
                preview_labels_.push_back("  └ " + status);
            } else if (row.element_index.has_value() && sample.has_value() &&
                       sample->has_value && sample->is_array &&
                       *row.element_index < sample->array_values.size()) {
                preview_labels_.push_back(FormatExpandedArrayValue(
                    symbol, static_cast<std::size_t>(*row.element_index),
                    sample->array_values[*row.element_index],
                    sample->stale || sample->stopped));
            } else if (sample.has_value() && sample->has_value &&
                       !sample->is_array) {
                preview_labels_.push_back(
                    FormatExpandedScalarValue(symbol, *sample));
            } else {
                preview_labels_.push_back("  └ 尚无可显示的完整样本");
            }
        }

        // 空搜索结果也留一个不可操作的菜单占位行，保持列表可聚焦。
        if (preview_labels_.empty())
            preview_labels_.push_back("（当前范围没有可显示的变量）");

        const auto same_row = [&](const PreviewRow& candidate) {
            return candidate.symbol_name == old_selected_row.symbol_name &&
                   candidate.is_child == old_selected_row.is_child &&
                   candidate.is_read_action ==
                       old_selected_row.is_read_action &&
                   candidate.is_placeholder ==
                       old_selected_row.is_placeholder &&
                   candidate.element_index == old_selected_row.element_index;
        };
        auto retained =
            std::find_if(preview_rows_.begin(), preview_rows_.end(), same_row);
        if (retained == preview_rows_.end()) {
            retained = std::find_if(preview_rows_.begin(), preview_rows_.end(),
                                    [&](const PreviewRow& candidate) {
                                        return !candidate.is_child &&
                                               candidate.symbol_name ==
                                                   old_selected_row.symbol_name;
                                    });
        }
        if (retained != preview_rows_.end()) {
            preview_selected_ = static_cast<int>(
                std::distance(preview_rows_.begin(), retained));
        } else {
            preview_selected_ =
                preview_labels_.empty()
                    ? 0
                    : std::clamp(old_selected_index, 0,
                                 static_cast<int>(preview_labels_.size() - 1U));
        }
        const std::string active_filter =
            preview_active_filter_.empty() ? "*" : preview_active_filter_;
        preview_status_ = "结果筛选 " + active_filter + "：匹配 " +
                          std::to_string(preview_symbols_.size()) +
                          " 个变量；当前显示 " +
                          std::to_string(preview_rows_.size()) +
                          " 行 | Enter 展开/读取，F3 详情，F4 标定，F6 "
                          "观测，F7 命令";
        if (preview_filter_ != preview_active_filter_)
            preview_status_ += " | 搜索框是未提交草稿";
        // 性能日志：候选数、展开后的行数与真实耗时一并记录，便于定位 A2L 规模影响。
        LIBXCP_LOG_INFO(
            "UI 整表重建耗时 {:.1f} ms（候选 {} 个 / 展开后 {} 行 / 范围 {}）",
            stopwatch.ElapsedMs(), preview_symbols_.size(), preview_rows_.size(),
            preview_scope_selected_ == 1 ? "仅采样" : "全部");
    }

    /** @brief 保存一次成功样本；保留最近值并刷新预览行。 */
    void RecordObservationValue(const std::string& name,
                                const std::string& value,
                                const std::string& status,
                                const std::string& timestamp) {
        {
            std::lock_guard lock(output_mutex_);
            observation_model_.UpdateValue(name, value, status, timestamp);
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
        }
        RequestPreviewRefresh(name);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 新增采样项后保留旧值并显示等待状态。 */
    void RecordObservationPending(const std::string& name) {
        {
            std::lock_guard lock(output_mutex_);
            observation_model_.MarkPending(name);
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
            if ((preview_scope_selected_ == 1 && !watched) ||
                expanded_preview_names_.contains(name)) {
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

    /** @brief 在预览列表中原位展开或折叠当前父变量行。 */
    void TogglePreviewExpansion() {
        if (terminal_too_small_.load() || preview_selected_ < 0 ||
            static_cast<std::size_t>(preview_selected_) >= preview_rows_.size())
            return;
        const auto& row = preview_rows_[preview_selected_];
        if (row.symbol_name.empty() || row.is_child) return;
        const auto [expanded, inserted] =
            expanded_preview_names_.insert(row.symbol_name);
        if (!inserted) expanded_preview_names_.erase(expanded);
        RebuildPreviewLabels();
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 根据当前行类型展开变量或发起独立的一次性读取。 */
    void ActivatePreviewRow() {
        if (preview_selected_ < 0 ||
            static_cast<std::size_t>(preview_selected_) >= preview_rows_.size())
            return;
        const auto row = preview_rows_[preview_selected_];
        if (row.symbol_name.empty()) return;
        switch (calmcar::xcp::tools::ActionForPreviewRow(row)) {
            case calmcar::xcp::tools::PreviewRowAction::ToggleExpansion:
                TogglePreviewExpansion();
                break;
            case calmcar::xcp::tools::PreviewRowAction::ReadOnce:
                SubmitPreviewReadOnce(row.symbol_name);
                break;
            case calmcar::xcp::tools::PreviewRowAction::Ignore:
                break;
        }
    }

    /** @brief 将展开行的一次性读取提交到后台命令线程。 */
    void SubmitPreviewReadOnce(const std::string& name) {
        if (DaqRunning()) {
            RecordObservationFailure(name, "DAQ运行期间不能执行单次读取。",
                                     LocalTimestamp());
            return;
        }
        const auto symbol_index = preview_symbol_index_.find(name);
        if (symbol_index == preview_symbol_index_.end()) {
            RecordObservationFailure(name, "A2L变量已不在当前预览结果中。",
                                     LocalTimestamp());
            return;
        }
        const auto reason = PreviewReadUnsupportedReason(
            preview_symbols_[symbol_index->second]);
        if (!reason.empty()) {
            RecordObservationFailure(name, "单次读取拒绝：" + reason,
                                     LocalTimestamp());
            return;
        }
        if (!SubmitCommand("previewread", std::nullopt, {}, std::nullopt,
                           std::nullopt, std::optional<std::string>{name}))
            RecordObservationFailure(name, "单次读取未能进入命令队列。",
                                     LocalTimestamp());
    }

    /** @brief 从预览列表进入独立标定表单；不改变观测集合。 */
    void OpenCalibration() {
        if (terminal_too_small_.load() || preview_selected_ < 0 ||
            static_cast<std::size_t>(preview_selected_) >= preview_rows_.size())
            return;
        const auto name = preview_rows_[preview_selected_].symbol_name;
        const auto symbol = preview_symbol_index_.find(name);
        if (symbol == preview_symbol_index_.end()) return;
        modal_symbol_info_ = preview_symbols_[symbol->second];
        modal_element_index_text_ =
            RequiresElementIndex(*modal_symbol_info_) ? "0" : "";
        {
            std::lock_guard lock(output_mutex_);
            static_cast<void>(calibration_form_.Select(name, true));
            const auto sample = observation_model_.Find(name);
            if (sample.has_value() && sample->has_value && !sample->is_array)
                static_cast<void>(
                    calibration_form_.Apply(calibration_form_.Snapshot(),
                                            sample->status, sample->value));
        }
        if (!variable_modal_.OpenCalibration(name, current_layout_)) {
            modal_symbol_info_.reset();
            Say("无法打开标定表单，请检查终端尺寸和 A2L 符号。");
            return;
        }
        modal_page_selected_ = 1;
        calibration_modal_active_ = true;
        modal_visible_ = true;
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief F3 打开只读详情；列表位置与搜索词不变。 */
    void OpenPreviewDetails() {
        if (terminal_too_small_.load()) {
            Say("窗口小于 80×24，变量详情和编辑操作已禁用。");
            return;
        }
        if (preview_selected_ < 0 ||
            static_cast<std::size_t>(preview_selected_) >= preview_rows_.size())
            return;
        const auto name = preview_rows_[preview_selected_].symbol_name;
        if (name.empty()) return;
        const auto symbol = preview_symbol_index_.find(name);
        if (symbol == preview_symbol_index_.end()) return;
        modal_symbol_info_ = preview_symbols_[symbol->second];
        modal_element_index_text_.clear();
        calibration_modal_active_ = false;
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
        const bool closing_calibration = calibration_modal_active_;
        variable_modal_.Close();
        modal_visible_ = false;
        modal_page_selected_ = 0;
        modal_symbol_info_.reset();
        modal_element_index_text_.clear();
        calibration_modal_active_ = false;
        if (closing_calibration) {
            std::lock_guard lock(output_mutex_);
            static_cast<void>(calibration_form_.Select({}, true));
        }
        ApplyPendingPreviewSearchResult();
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 在预览列表中切换当前符号的观测集合成员资格。 */
    void TogglePreviewWatch() {
        if (preview_selected_ < 0 ||
            static_cast<std::size_t>(preview_selected_) >= preview_rows_.size())
            return;
        const auto& name = preview_rows_[preview_selected_].symbol_name;
        if (name.empty()) return;
        if (IsWatched(name))
            Remove(name);
        else
            Add(name);
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
            element_index; /**< 标定表单提交的0基元素索引。 */
        std::optional<std::string>
            preview_read_symbol; /**< 展开预览行发起的一次性读取符号。 */
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
        std::optional<std::string> preview_read_symbol = std::nullopt) {
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
                                           element_index,
                                           std::move(preview_read_symbol)});
            } else {
                command_queue_.push_back({command, std::move(calibration),
                                          std::move(target), preview_revision,
                                          element_index,
                                          std::move(preview_read_symbol)});
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
     * @param calibration_context 确认完成后是否返回独立标定页。
     * @return 本次有效确认票据；拒绝/退出/尺寸不足时为空。
     */
    std::optional<calmcar::xcp::tools::ConfirmationGate::Ticket> Confirm(
        const std::string& prompt, bool calibration_context = false) {
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
        screen_->Post(ftxui::Closure([this, ticket, prompt,
                                      calibration_context] {
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
                calibration_context
                    ? variable_modal_.BeginConfirmation(true, current_layout_)
                    : variable_modal_.OpenCommandConfirmation(current_layout_);
            if (!opened) {
                confirmation_gate_.Resolve(ticket, false);
                return;
            }
            confirmation_prompt_ = prompt;
            modal_page_selected_ = 2;
            modal_visible_ = true;
        }));
        const bool accepted = confirmation_gate_.Wait(ticket, worker_stop_);
        if (screen_) {
            screen_->Post(ftxui::Closure([this, calibration_context] {
                if (calibration_context) {
                    variable_modal_.CompleteConfirmation();
                    modal_visible_ = variable_modal_.IsOpen();
                    modal_page_selected_ =
                        variable_modal_.Page() ==
                                calmcar::xcp::tools::VariableModalPage::
                                    Calibration
                            ? 1
                            : 0;
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

    /**
     * @brief 直接改写标定表单的状态行（界面线程的本地拒绝反馈）。
     * @details 用于「还没进入异步命令」就被拒绝的分支：这些分支没有业务结果，
     *          无法走 UpdateCalibrationState，而 Say 写的是被模态窗口遮挡的
     *          主面板，因此必须显式更新表单状态并触发重绘。
     * @param status 要展示给用户的状态文本。
     */
    void SetCalibrationFormStatus(const std::string& status) {
        {
            std::lock_guard lock(output_mutex_);
            calibration_form_.status = status;
        }
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 判断详情对象是否需要0基元素索引输入。 */
    bool RequiresElementIndex(const SymbolInfo& symbol) const {
        return !symbol.dimensions.empty() ||
               (symbol.kind == SymbolKind::Characteristic &&
                symbol.characteristic_type == CharacteristicType::Curve);
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
        // P2：A2L 未声明段页事实时不再拒绝（段页是可选能力），写入资格交由运行时
        // 页校验决定；判定依据通过 ModalCalibrationPageNote() 单独展示。
        if (!calmcar::xcp::tools::HasCalibrationPageFacts(symbol)) return {};
        for (const auto& page : symbol.memory_segment->pages) {
            if (calmcar::xcp::tools::CalibrationPageWriteUnsupportedReason(
                    symbol, page.number, calmcar::xcp::PageAccessType::DontCare)
                    .empty())
                return {};
        }
        return "A2L未声明允许无条件XCP写入的PAGE";
    }

    /**
     * @brief 写入资格的页判定依据（无附注时返回空串）。
     * @details 用于界面区分"确实不可写"与"A2L 缺段页声明、按单页放行"。
     */
    std::string ModalCalibrationPageNote() const {
        if (!modal_symbol_info_.has_value()) return {};
        return calmcar::xcp::tools::CalibrationPageNote(*modal_symbol_info_,
                                                       calibration_page_facts_);
    }

    /** @brief 标定页读取依赖独立的标定选择快照。 */
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

    /** @brief 将详情弹窗中的目标值交给既有预读/确认/回写路径。 */
    void SubmitCalibrationWrite() {
        if (!modal_symbol_info_.has_value()) {
            SetCalibrationFormStatus("尚未选择变量；请先在列表中选中对象");
            return;
        }
        const std::string reason = ModalWriteUnsupportedReason();
        if (!reason.empty()) {
            SetCalibrationFormStatus("拒绝写入：" + reason);
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
            SetCalibrationFormStatus("请选择有效标量VALUE或曲线点，并填写目标物理值");
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
        preview_symbols_ = std::move(result.symbols);
        preview_symbol_index_.clear();
        for (std::size_t index = 0; index < preview_symbols_.size(); ++index)
            preview_symbol_index_[QualifiedName(preview_symbols_[index])] =
                index;
        preview_active_filter_ = result.pattern == "*" ? "" : result.pattern;
        if (result.preview_revision.has_value() &&
            preview_input_revision_.Matches(*result.preview_revision))
            preview_filter_ = preview_active_filter_;
        // 性能观测：应用结果含建索引与整表重建，是搜索后那一跳的主要成本。
        const PerfStopwatch stopwatch;
        RebuildPreviewLabels();
        LIBXCP_LOG_INFO("UI 应用搜索结果耗时 {:.1f} ms（{} 个候选，筛选 {}）",
                        stopwatch.ElapsedMs(), preview_symbols_.size(),
                        preview_active_filter_.empty() ? "*"
                                                       : preview_active_filter_);
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
        // 性能观测：A2L 检索是库内全量线性扫描，耗时随符号总数线性增长。
        const PerfStopwatch search_stopwatch;
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
        // 性能日志：检索耗时与命中/过滤后的数量分开记，命中数是渲染成本的真正输入。
        LIBXCP_LOG_INFO("UI 候选检索耗时 {:.1f} ms（模式 {} / 命中 {} 个 / 库内共 {} 个）",
                        search_stopwatch.ElapsedMs(), query,
                        result.Value().size(), count.Value());
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
        if (command == "previewread") {
            if (!request.preview_read_symbol)
                Say("预览单次读取参数无效。");
            else
                ReadPreviewOnce(*request.preview_read_symbol);
        } else if (command == "find") {
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
        Say("命令：list [pattern|all] | more | info <name> | remove <name> "
            "| read <name> | watch | stop | write <name> <value> | find "
            "[pattern] | quit；变量预览 Enter 展开/折叠，F3 只读详情，"
            "F4 标定、F6 观测成员，F7 进入命令、Esc 返回预览。");
        Say("mode=" + options_.mode +
            (options_.mode == "poll"
                 ? "；watch 按目标周期 " +
                       std::to_string(options_.interval.count()) +
                       " ms 主动读取。"
                 : "；watch 使用事件驱动 DAQ，DTO 布局按从站取证自动选择。"));
        Say("DAQ 只支持唯一 FIXED_EVENT_LIST 的 MEASUREMENT；运行中禁止 "
            "read/write 和修改观测集合。stop + Enter 停止。");
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

    /** @brief 按采样模式检查A2L静态观测资格；DAQ还需固定事件信息。 */
    std::string ObservationUnsupportedReason(const SymbolInfo& symbol) const {
        if (options_.mode == "daq") {
            std::vector<std::uint16_t> available_events;
            available_events.reserve(events_.size());
            for (const auto& event : events_)
                available_events.push_back(event.channel);
            return calmcar::xcp::tools::SymbolDaqObservationUnsupportedReason(
                symbol, available_events, true);
        }
        if (symbol.dimensions.empty())
            return calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
        if (symbol.kind == SymbolKind::Measurement)
            return calmcar::xcp::tools::SymbolDetailReadUnsupportedReason(
                symbol);
        return calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
    }

    /** @brief 检查停止采样时展开行的一次性读取资格。 */
    std::string PreviewReadUnsupportedReason(const SymbolInfo& symbol) const {
        if (symbol.dimensions.empty())
            return calmcar::xcp::tools::SymbolReadUnsupportedReason(symbol);
        if (symbol.kind != SymbolKind::Measurement)
            return "单次数组读取仅支持MEASUREMENT";
        return calmcar::xcp::tools::SymbolDetailReadUnsupportedReason(symbol);
    }

    void Add(const std::string& name) {
        if (DaqRunning()) {
            Say("DAQ 运行期间不能修改观测集；请先 stop。");
            return;
        }
        if (name.empty()) {
            Say("请先在变量列表中选择变量，再按 F6 加入观测。");
            return;
        }
        auto found = database_.Find(name);
        if (!found.HasValue()) {
            Say("Find 失败: " + found.ErrorInfo().message);
            return;
        }
        if (!ObservationUnsupportedReason(found.Value()).empty()) {
            Say("不能观测: " + ObservationUnsupportedReason(found.Value()));
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
        // 标定表单读取与 DAQ 并行合法：DAQ 运行期间不再拒绝读取请求。
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

    /** @brief 从展开预览行执行一次性POLL读取，不触碰标定表单。 */
    void ReadPreviewOnce(const std::string& name) {
        const auto timestamp = LocalTimestamp();
        if (DaqRunning()) {
            RecordObservationFailure(name, "DAQ运行中；不能执行单次读取。",
                                     timestamp);
            return;
        }
        const auto found = database_.Find(name);
        if (!found.HasValue()) {
            RecordObservationFailure(
                name, "A2L查找失败: " + found.ErrorInfo().message, timestamp);
            return;
        }
        const auto canonical = QualifiedName(found.Value());
        const auto reason = PreviewReadUnsupportedReason(found.Value());
        if (!reason.empty()) {
            RecordObservationFailure(canonical, "读取拒绝: " + reason,
                                     timestamp);
            return;
        }
        if (!found.Value().dimensions.empty()) {
            std::string error;
            const auto values = ReadObservedArray(found.Value(), error, false);
            if (values.has_value()) {
                RecordArrayObservationValue(canonical, std::move(*values),
                                            "单次读取", timestamp, false);
            } else if (!error.empty()) {
                RecordObservationFailure(canonical, error, timestamp);
            }
            return;
        }
        const auto value = ReadPhysical(found.Value());
        if (value.has_value())
            RecordObservationValue(canonical, PhysicalToString(*value),
                                   "单次读取", timestamp);
        else
            RecordObservationFailure(canonical, "单次读取失败", timestamp);
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

    /**
     * @brief 以小块读取一维 MEASUREMENT 数组，全部元素成功后才返回快照。
     * @param symbol 已解析的数组符号
     * @param error 读取/换算失败原因；因停止而取消时保持为空
     * @return 完整物理值数组；中断或任一块失败时返回空
     */
    std::optional<std::vector<std::string>> ReadObservedArray(
        const SymbolInfo& symbol, std::string& error,
        bool require_watched = true) {
        const auto reason =
            calmcar::xcp::tools::SymbolDetailReadUnsupportedReason(symbol);
        if (!reason.empty()) {
            error = "数组布局不支持: " + reason;
            return std::nullopt;
        }
        if (symbol.kind != SymbolKind::Measurement ||
            symbol.dimensions.size() != 1U) {
            error = "数组仅读取一维 MEASUREMENT。";
            return std::nullopt;
        }

        const auto extent = symbol.dimensions.front().extent;
        const auto canonical = QualifiedName(symbol);
        const auto operation = require_watched ? "POLL" : "单次读取";
        const auto cancelled = [this, &canonical, require_watched] {
            if (worker_stop_.load()) return true;
            return require_watched &&
                   (control_cancel_.load() || !watching_.load() ||
                    !IsWatched(canonical));
        };
        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(extent));
        for (std::uint64_t start = 0; start < extent; start += kArrayPageSize) {
            if (cancelled()) return std::nullopt;
            const auto page = calmcar::xcp::tools::MakeElementPageRange(
                extent, start, kArrayPageSize);
            if (!page.has_value()) {
                error = std::string(operation) + "数组页范围验证失败。";
                return std::nullopt;
            }
            const auto session = master_.GetSessionParameters();
            const auto ag =
                static_cast<std::size_t>(session.connect.address_granularity);
            const auto range_reason =
                calmcar::xcp::tools::ElementRangeLayoutUnsupportedReason(
                    symbol, start, page->count, ag, session.connect.byte_order);
            if (!range_reason.empty()) {
                error = std::string(operation) +
                        "数组页布局不支持: " + range_reason;
                return std::nullopt;
            }
            if (symbol.element_size_bytes == 0U ||
                page->count > std::numeric_limits<std::uint64_t>::max() /
                                  symbol.element_size_bytes) {
                error = std::string(operation) + "数组页字节长度溢出。";
                return std::nullopt;
            }
            const auto byte_count = page->count * symbol.element_size_bytes;
            if (byte_count == 0U ||
                byte_count > std::numeric_limits<ByteCount>::max()) {
                error =
                    std::string(operation) + "数组页字节长度超出 XCP 范围。";
                return std::nullopt;
            }
            const auto address = symbol.xcp_address +
                                 start * symbol.dimensions.front().byte_stride;
            Bytes raw;
            try {
                std::lock_guard command_lock(command_mutex_);
                if (cancelled()) return std::nullopt;
                raw = master_.ReadMemoryBytes(
                    static_cast<Address>(address),
                    static_cast<AddressExtension>(symbol.address_extension),
                    static_cast<ByteCount>(byte_count));
            } catch (const std::exception& exception) {
                if (cancelled()) return std::nullopt;
                error = std::string(operation) +
                        "数组 XCP 读取失败: " + std::string(exception.what());
                return std::nullopt;
            }
            if (cancelled()) return std::nullopt;
            if (raw.size() != byte_count) {
                error = std::string(operation) +
                        "数组 XCP 返回字节数与 A2L 布局不一致。";
                return std::nullopt;
            }
            for (std::uint64_t offset = 0; offset < page->count; ++offset) {
                const auto byte_offset = static_cast<std::size_t>(
                    offset * symbol.element_size_bytes);
                const auto element_begin =
                    raw.begin() + static_cast<std::ptrdiff_t>(byte_offset);
                const auto element_end =
                    element_begin +
                    static_cast<std::ptrdiff_t>(symbol.element_size_bytes);
                const Bytes element(element_begin, element_end);
                const auto index = start + offset;
                const auto physical = database_.ToPhysicalElement(
                    QualifiedName(symbol), index, element);
                if (!physical.HasValue()) {
                    error = std::string(operation) + "数组元素物理换算失败: " +
                            physical.ErrorInfo().message;
                    return std::nullopt;
                }
                values.push_back(PhysicalToString(physical.Value()));
            }
        }
        if (values.size() != extent) {
            error =
                std::string(operation) + "数组快照元素数量与 A2L 维度不一致。";
            return std::nullopt;
        }
        return values;
    }

    /** @brief 在持有XCP命令互斥锁时复核活动页及实时PAGE_PROPERTIES。 */
    std::string CheckCalibrationPageWriteLocked(
        const SymbolInfo& symbol, std::uint8_t* active_page_number) {
        // P2：段页是可选能力。A2L 没有 MEMORY_SEGMENT 时按 0 号段处理，并且只查询
        // A2L 真正声明过的页命令，避免每次写入都换回一个 ERR_CMD_UNKNOWN。
        const bool has_page_facts =
            calmcar::xcp::tools::HasCalibrationPageFacts(symbol);
        if (!has_page_facts &&
            !calibration_page_facts_.declares_get_cal_page &&
            !calibration_page_facts_.declares_get_page_info) {
            // 既无段页事实也无页命令声明：按单页直写，不做页校验。
            return {};
        }
        const std::uint8_t segment_number =
            has_page_facts ? symbol.memory_segment->segment_number : 0U;
        const auto active_page = master_.GetCalPage(
            calmcar::xcp::CalPageAccessMode::Xcp, segment_number);
        if (!active_page.has_value()) {
            // 有段页事实却查不到活动页属于真问题；无事实时只是"该 ECU 不支持页",
            // 写入权限仍由 ECU 侧 ERR_WRITE_PROTECTED 兜底。
            return has_page_facts ? "无法查询XCP活动页，拒绝写入" : std::string{};
        }
        if (calibration_page_facts_.declares_get_page_info) {
            const auto page_info =
                master_.GetPageInfo(segment_number, active_page->page);
            if (!page_info.has_value())
                return has_page_facts ? "无法查询XCP PAGE_PROPERTIES，拒绝写入"
                                     : std::string{};
            if (has_page_facts) {
                const std::string reason =
                    calmcar::xcp::tools::CalibrationPageWriteUnsupportedReason(
                        symbol, active_page->page,
                        page_info->properties.xcp_write_access);
                if (!reason.empty()) return reason;
            } else if (page_info->properties.xcp_write_access !=
                       calmcar::xcp::PageAccessType::DontCare) {
                return "Slave 当前 PAGE_PROPERTIES 不允许无条件 XCP 写访问";
            }
        } else if (active_page->page != 0U) {
            // 无法查询 PAGE_PROPERTIES 时，只把"活动页 0（工作页约定）"当作放行依据。
            return "当前活动页为 " + std::to_string(active_page->page) +
                   "，A2L 未声明 GET_PAGE_INFO，无法确认该页允许写入；请先切回工作页";
        }
        if (active_page_number != nullptr) *active_page_number = active_page->page;
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
        // 标定写入与 DAQ 并行合法：DAQ 运行期间不再拒绝写入请求。
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
            // 把换算层的真实原因（如越出 A2L 限值 B-15）一并写进状态行，
            // 否则界面只剩一句「无法换算或编码」，用户无从判断该改什么。
            if (from_calibration_view)
                UpdateCalibrationState(*selection,
                                       "目标值不可编码：" +
                                           encoded.ErrorInfo().message);
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
                // 标定写入不再受 DAQ 运行状态限制：DAQ 与标定共用同一标定页
                // 与命令写入流程，二者可安全并行。此处只保留代际校验，防止
                // 用户在确认期间切换变量后仍把旧目标写进 ECU。
                if (selection && !CalibrationSelectionMatches(*selection)) {
                    static_cast<void>(
                        confirmation_gate_.CancelBeforeDispatch());
                    dispatch_failure = "确认许可已失效或变量选择已变化";
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
        }
        RequestPreviewRefresh(name);
        if (screen_) screen_->PostEvent(ftxui::Event::Custom);
    }

    /** @brief 原子发布完整数组快照，并可为POLL复核观测集合。 */
    void RecordArrayObservationValue(const std::string& name,
                                     std::vector<std::string> values,
                                     const std::string& status,
                                     const std::string& timestamp,
                                     bool require_watched) {
        {
            std::lock_guard output_lock(output_mutex_);
            if (require_watched) {
                std::lock_guard watched_lock(watched_mutex_);
                if (std::find(watched_.begin(), watched_.end(), name) ==
                    watched_.end())
                    return;
            }
            observation_model_.UpdateArrayValue(name, std::move(values), status,
                                                timestamp);
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
            Say("观测列表为空；请在变量列表选择变量后按 F6。");
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
                    const auto timestamp = LocalTimestamp();
                    if (!found.Value().dimensions.empty()) {
                        std::string error;
                        std::optional<std::vector<std::string>> values;
                        try {
                            values = ReadObservedArray(found.Value(), error);
                        } catch (const std::exception& exception) {
                            error = "POLL 数组读取异常: " +
                                    std::string(exception.what());
                        }
                        if (values.has_value()) {
                            RecordArrayObservationValue(
                                name, std::move(*values), "POLL", timestamp,
                                true);
                        } else if (watching_.load() && IsWatched(name)) {
                            RecordWatchedObservationFailure(
                                name,
                                error.empty() ? "POLL数组读取已取消" : error,
                                timestamp);
                        }
                    } else {
                        auto value = ReadPhysical(found.Value());
                        if (value) {
                            RecordWatchedObservationValue(
                                name, PhysicalToString(*value), "POLL",
                                timestamp);
                        } else {
                            RecordWatchedObservationFailure(
                                name, "POLL读取失败", timestamp);
                        }
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
            Say("观测列表为空；请从变量预览中选择变量并按 F6。");
            return;
        }

        std::vector<A2lEventMetadata> selected_events;
        for (const auto& name : names) {
            auto found = database_.Find(name);
            if (!found.HasValue()) {
                Say(name + " | A2L 查找失败: " + found.ErrorInfo().message);
                return;
            }
            const auto& symbol = found.Value();
            // 与F6和预览列表共用静态准入，运行期配置仍在下方复核。
            const auto reason = ObservationUnsupportedReason(symbol);
            if (!reason.empty()) {
                Say(name + " | DAQ 不支持: " + reason);
                return;
            }
            const auto event = std::find_if(
                events_.begin(), events_.end(), [&](const auto& item) {
                    return item.channel == symbol.daq_event_fixed.front();
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
        }

        const auto session = master_.GetSessionParameters();
        std::size_t timestamp_bytes = 0;
        std::size_t identification_bytes = 1;
        MeasurementDaqCapabilities daq_entry_capabilities{};
        std::optional<MeasurementRuntimeProfile> runtime_profile;
        {
            // DTO 布局由从站上报的识别字段模式自动决定，无需预先声明厂商。
            auto profile =
                BuildMeasurementRuntimeProfile(master_, selected_events);
            if (!profile.HasValue()) {
                const std::string& reason = profile.ErrorInfo().message;
                // 结构化提示：附带从站上报的 DTO 标识与时间戳事实，便于定位。
                std::string facts;
                const auto processor = master_.QueryDaqProcessorInfo();
                if (processor.has_value()) {
                    facts = "；从站 DAQ_KEY_BYTE=0x" +
                            [&processor] {
                                constexpr char kHex[] = "0123456789ABCDEF";
                                std::uint8_t raw = processor->key_byte.raw;
                                std::string text;
                                text += kHex[(raw >> 4U) & 0x0FU];
                                text += kHex[raw & 0x0FU];
                                return text;
                            }() +
                            "（识别字段模式 " +
                            std::to_string(
                                processor->key_byte.identification_field_type) +
                            "，地址扩展模式 " +
                            std::to_string(
                                processor->key_byte.address_extension_mode) +
                            "）";
                }
                Say("DAQ 能力校验失败: " + reason + facts +
                    "。请核对 A2L 与从站能力后再试。");
                return;
            }
            runtime_profile = profile.Value();
            timestamp_bytes =
                profile.Value().timestamp_unit_ns != 0U ? 4U : 0U;
            identification_bytes = profile.Value().identification_header_bytes;
            daq_entry_capabilities = profile.Value().daq_entry_capabilities;
        }
        MeasurementPlanner planner(measurement_database_,
                                   session.connect.max_dto,
                                   session.connect.address_granularity,
                                   timestamp_bytes, identification_bytes,
                                   daq_entry_capabilities);
        auto planned = planner.Build(names);
        if (!planned.HasValue()) {
            Say("DAQ 本地规划失败: " + planned.ErrorInfo().message);
            return;
        }
        // 规范约束：识别字段为绝对 ODT 号（Absolute PID）时，PID 空间有限，
        // 只支持单一 DAQ list。判据取自已取证的识别字段模式，而非厂商名。
        if (runtime_profile->identification_field_type ==
                IdentificationFieldType::Absolute &&
            planned.Value().daq_lists.size() != 1U) {
            Say("Absolute PID 布局仅支持一个 DAQ list。");
            return;
        }

        Say("警告：开始 DAQ 将发送 FREE_DAQ，清除 Slave 当前全部动态 DAQ "
            "配置（不可恢复）。");
        const auto ticket = Confirm(
            "即将 FREE_DAQ 清空 ECU 所有动态 DAQ 配置（不可恢复）；识别字段模式 " +
            std::to_string(static_cast<int>(
                runtime_profile->identification_field_type)) +
            "（头长 " + std::to_string(identification_bytes) + " 字节），观测变量 " +
            std::to_string(names.size()) +
            " 个。能力查询及布局检查已通过。继续? [Y/N]");
        if (!ticket) {
            Say("已取消；没有发送 FREE_DAQ。");
            return;
        }

        bool samples_pending = false;
        try {
            if (!profile_configured_) {
                ApplyMeasurementRuntimeProfile(measurement_session_,
                                               *runtime_profile);
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
                if (sample.is_array) {
                    // DTO 中的数组按一个样本一次发布；失败时不覆盖此前整批值。
                    std::vector<std::string> values;
                    values.reserve(sample.elements.size());
                    for (const auto& element : sample.elements)
                        values.push_back(PhysicalToString(element));
                    if (values.empty()) {
                        RecordWatchedObservationFailure(
                            sample.name, "DAQ数组样本为空 | " + status.str(),
                            timestamp);
                    } else {
                        RecordArrayObservationValue(sample.name,
                                                    std::move(values),
                                                    status.str(), timestamp,
                                                    true);
                    }
                } else {
                    RecordWatchedObservationValue(
                        sample.name, PhysicalToString(sample.value),
                        status.str(), timestamp);
                }
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

    /**
     * @brief 记录一帧界面构建耗时：超阈值逐帧告警，并按固定窗口汇总一次。
     *
     * 只在界面线程调用（渲染器内），因此聚合状态无需加锁。
     * 逐帧写日志本身会显著拖慢界面，故低于阈值的帧只累加计数。
     * @param elapsed_ms 本帧元素构建耗时（毫秒）。
     * @param candidate_count 当前候选符号数量。
     * @param row_count 当前展开后的预览行数。
     */
    void NoteFrameRendered(double elapsed_ms, std::size_t candidate_count,
                           std::size_t row_count) {
        ++frame_perf_frames_;
        frame_perf_total_ms_ += elapsed_ms;
        if (elapsed_ms > frame_perf_max_ms_) frame_perf_max_ms_ = elapsed_ms;
        if (elapsed_ms >= kFramePerfWarnMs) {
            LIBXCP_LOG_WARNING(
                "UI 单帧构建耗时 {:.1f} ms（候选 {} 个 / 展开后 {} 行）",
                elapsed_ms, candidate_count, row_count);
        }
        const auto now = std::chrono::steady_clock::now();
        const double window_seconds =
            std::chrono::duration<double>(now - frame_perf_window_start_).count();
        if (window_seconds < kFramePerfWindowSeconds) return;
        const double average_ms =
            frame_perf_frames_ == 0U
                ? 0.0
                : frame_perf_total_ms_ / static_cast<double>(frame_perf_frames_);
        LIBXCP_LOG_INFO(
            "UI 渲染汇总：近 {:.1f} s 共 {} 帧，均值 {:.1f} ms，峰值 {:.1f} ms"
            "（候选 {} 个 / 展开后 {} 行）",
            window_seconds, frame_perf_frames_, average_ms, frame_perf_max_ms_,
            candidate_count, row_count);
        frame_perf_window_start_ = now;
        frame_perf_frames_ = 0U;
        frame_perf_total_ms_ = 0.0;
        frame_perf_max_ms_ = 0.0;
    }

    const Options& options_;
    const calmcar::xcp::a2l::IA2lDatabase& database_;
    calmcar::xcp::IMeasurementDatabase& measurement_database_;
    MeasurementSession& measurement_session_;
    XcpMaster& master_;
    /** @brief A2L 声明的标定页命令能力（决定运行时是否查询 GET_CAL_PAGE/GET_PAGE_INFO）。 */
    calmcar::xcp::tools::CalibrationPageFacts calibration_page_facts_;
    std::vector<A2lEventMetadata> events_;
    mutable std::mutex output_mutex_;
    std::mutex command_mutex_;
    std::mutex watched_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::atomic<bool> watching_{false};
    std::atomic<bool> daq_running_{false};
    /** @brief 单帧渲染耗时汇总窗口的起点（仅界面线程访问）。 */
    std::chrono::steady_clock::time_point frame_perf_window_start_ =
        std::chrono::steady_clock::now();
    /** @brief 当前汇总窗口内已统计的帧数（仅界面线程访问）。 */
    std::uint64_t frame_perf_frames_{0U};
    /** @brief 当前汇总窗口内的帧耗时总和，毫秒（仅界面线程访问）。 */
    double frame_perf_total_ms_{0.0};
    /** @brief 当前汇总窗口内的单帧耗时峰值，毫秒（仅界面线程访问）。 */
    double frame_perf_max_ms_{0.0};
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
    bool modal_visible_{false};            /**< FTXUI模态覆盖层是否显示。 */
    bool calibration_modal_active_{
        false};                         /**< 标定表单是否拥有当前模态状态。 */
    int modal_page_selected_{0};        /**< 详情0、标定1、确认2。 */
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
        preview_visible_index_;            /**< 规范名到父预览菜单行号。 */
    std::vector<PreviewRow> preview_rows_; /**< 菜单行到符号/元素的映射。 */
    std::unordered_set<std::string>
        expanded_preview_names_; /**< 当前原位展开的符号规范名。 */
    /** @brief 变量预览菜单文本；空结果也用占位行保持焦点。 */
    std::vector<std::string> preview_labels_{"（正在加载A2L变量…）"};
    std::vector<std::string> view_labels_{"变量预览", "命令结果",
                                          "日志"}; /**< 顶部三视图标签。 */
    std::vector<std::string> preview_scope_labels_{
        "全部变量", "仅采样"}; /**< 变量预览范围筛选。 */
    calmcar::xcp::tools::TuiInputModeState
        input_mode_;                /**< 显式预览/命令输入模式及事件隔离。 */
    int view_selected_{0};          /**< 当前选中视图：预览、结果、日志。 */
    int preview_selected_{0};       /**< 当前变量预览菜单行。 */
    int preview_scope_selected_{0}; /**< 0=全部变量，1=仅采样。 */
    std::string preview_filter_;    /**< 当前变量名搜索输入框草稿。 */
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

#if LIBXCP_ENABLE_LOG
    calmcar::xcp::internal::ProgramLogSession logging;
    std::string log_error;
    if (!logging.Initialize(argv[0], "tools", log_error)) {
        std::cerr << "日志初始化失败: " << log_error << '\n';
        return 1;
    }
#endif

    // 性能观测：A2L 解析是启动阶段的一次性大开销，耗时随文件规模线性增长。
    const PerfStopwatch load_stopwatch;
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
    // 性能日志：把解析耗时、符号总数与文件路径记在一起，便于对比不同 A2L。
    LIBXCP_LOG_INFO("A2L 加载耗时 {:.1f} ms（{} 个符号 / {}）",
                    load_stopwatch.ElapsedMs(), count.Value(),
                    options->a2l_path);
    for (const auto& warning : bridge->ListLoadWarnings()) {
        std::cout << "A2L 告警: " << warning.message << '\n';
    }

    calmcar::xcp::A2lMeasurementDatabase a2l_measurement_database(*database);
    calmcar::xcp::MeasurementEventBindingCandidates event_bindings;
    // 性能观测：启动时的一次全库检索（用于 DAQ 事件绑定），同样是 O(符号数)。
    const PerfStopwatch boot_search_stopwatch;
    auto all_symbols =
        database->Search("*", std::max<std::size_t>(1, count.Value()));
    if (!all_symbols.HasValue()) {
        std::cerr << "A2L DAQ event binding 查询失败: "
                  << all_symbols.ErrorInfo().message << '\n';
        return 1;
    }
    LIBXCP_LOG_INFO("A2L 启动全库检索耗时 {:.1f} ms（命中 {} 个）",
                    boot_search_stopwatch.ElapsedMs(),
                    all_symbols.Value().size());
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

    // ---- 传输层选择（B-T5 §8.1）------------------------------------------
    // 把 A2L 事实按本次 `--transport` **过滤后**再交给纯函数解析：
    // 规则 5 要求端点/对齐/Profile 同属一个 transport kind，因此这里绝不
    // 把另一类传输的端点或对齐"借"给本次连接。
    calmcar::xcp::tools::A2lTransportFacts a2l_facts;
    if (const auto* xcp_info = bridge->XcpInfo();
        xcp_info != nullptr && !xcp_info->ambiguous) {
        const auto wanted =
            options->transport_request.transport ==
                    calmcar::xcp::tools::TransportKind::Tcp
                ? calmcar::xcp::a2l::TransportEndpoint::Kind::TcpIp
                : calmcar::xcp::a2l::TransportEndpoint::Kind::UdpIp;
        const auto other =
            wanted == calmcar::xcp::a2l::TransportEndpoint::Kind::TcpIp
                ? calmcar::xcp::a2l::TransportEndpoint::Kind::UdpIp
                : calmcar::xcp::a2l::TransportEndpoint::Kind::TcpIp;
        for (const auto& endpoint : xcp_info->transports) {
            if (endpoint.kind == other) {
                a2l_facts.other_kind_declared = true;
                continue;
            }
            if (endpoint.kind != wanted) continue;
            ++a2l_facts.instance_count;
            // 逐实例明细交给纯函数层判定多实例冲突（计划 §7.3）：未显式声明
            // 对齐时按 0 记录，避免把桥接层默认值当成"已声明 8 bit"。
            a2l_facts.instances.push_back(
                calmcar::xcp::tools::A2lEndpointInstance{
                    endpoint.remote_host, endpoint.remote_port,
                    endpoint.packet_alignment_declared
                        ? endpoint.packet_alignment
                        : static_cast<std::uint8_t>(0U),
                    endpoint.version});
            if (a2l_facts.declared) continue;  // 标量字段：先到先得
            a2l_facts.declared = true;
            a2l_facts.port = endpoint.remote_port;
            a2l_facts.host = endpoint.remote_host;
            // 位宽 0 与"未声明"都视为无证据：不得把默认值当成声明
            if (endpoint.packet_alignment_declared &&
                endpoint.packet_alignment != 0U) {
                a2l_facts.packet_alignment_bits = endpoint.packet_alignment;
                a2l_facts.alignment_declared = true;
            }
            if (endpoint.version != 0U) {
                a2l_facts.transport_version = calmcar::xcp::XcpProtocolVersion{
                    static_cast<std::uint8_t>(endpoint.version >> 8U),
                    static_cast<std::uint8_t>(endpoint.version & 0xFFU)};
            }
            if (wanted == calmcar::xcp::a2l::TransportEndpoint::Kind::TcpIp) {
                // 仅诊断用途：把 A2L 声明的 TL 版本渲染成可读提示。
                // 它**不参与** Profile 选择（§1.2：A2L 不得推断 wire profile）。
                a2l_facts.profile_hint = "XCP_ON_TCP_IP(TL ";
                if (endpoint.version != 0U) {
                    a2l_facts.profile_hint +=
                        calmcar::xcp::XcpProtocolVersion{
                            static_cast<std::uint8_t>(endpoint.version >> 8U),
                            static_cast<std::uint8_t>(endpoint.version & 0xFFU)}
                            .ToString();
                } else {
                    a2l_facts.profile_hint += "未声明";
                }
                a2l_facts.profile_hint += ")";
            }
        }
        if (xcp_info->protocol_layer.version != 0U) {
            a2l_facts.protocol_version = calmcar::xcp::XcpProtocolVersion{
                static_cast<std::uint8_t>(xcp_info->protocol_layer.version >>
                                          8U),
                static_cast<std::uint8_t>(xcp_info->protocol_layer.version &
                                          0xFFU)};
        }
    }

    std::string selection_error;
    const auto selection = calmcar::xcp::tools::ResolveTransportSelection(
        options->transport_request, a2l_facts, &selection_error);
    if (!selection.has_value()) {
        std::cerr << "传输层配置错误: " << selection_error << '\n';
        return 1;
    }
    // 端点可能来自 A2L：把生效值回写到 Options，供 TUI 标题与日志展示，
    // 避免界面显示一个"用户没给因此为空"的端点。
    options->host = selection->host;
    options->port = selection->port;
    std::cout << calmcar::xcp::tools::FormatSelectionSummary(*selection) << '\n';
    for (const auto& note : selection->notes) {
        std::cout << "  · " << note << '\n';
    }

    try {
        // 按解析结果构造传输层；UDP/TCP 的差异只体现在这一处，
        // 其后的命令流、DAQ、标定路径完全共用。
        std::unique_ptr<calmcar::xcp::IXcpTransport> transport;
        if (selection->transport == calmcar::xcp::tools::TransportKind::Tcp) {
            calmcar::xcp::TcpTransportConfig tcp_config;
            tcp_config.role = calmcar::xcp::TcpRole::Master;
            tcp_config.remote_host = selection->host;
            tcp_config.remote_port = selection->port;
            tcp_config.packet_alignment = selection->alignment;
            tcp_config.wire_profile = selection->wire_profile;
            transport =
                std::make_unique<calmcar::xcp::TcpTransport>(tcp_config);
        } else {
            calmcar::xcp::UdpTransportConfig udp_config;
            udp_config.remote_host = selection->host;
            udp_config.remote_port = selection->port;
            udp_config.packet_alignment = selection->alignment;
            transport =
                std::make_unique<calmcar::xcp::UdpTransport>(udp_config);
        }
        // 注意：外层已有名为 options 的 CLI 参数指针，此处变量名须避让
        calmcar::xcp::XcpMasterOptions master_options;
        master_options.event_listener = &measurement_session;
        // 规则 6：PL/TL 作为独立先验下发，绝不因 Profile 自动声称版本。
        master_options.version_policy.protocol_version =
            selection->protocol_version;
        master_options.version_policy.transport_version =
            selection->transport_version;
        XcpMaster master(std::move(transport), master_options);
        master.Connect();
        measurement_session.Bind(master);
        {
            // 运行期版本证据：reported 与 effective 必须并排展示（§8.1）
            const auto pl = master.GetProtocolVersionSnapshot();
            const auto tl = master.GetTransportVersionSnapshot();
            calmcar::xcp::tools::RuntimeVersionStatus version_status;
            version_status.pl_reported = pl.evidence.reported;
            version_status.pl_effective = pl.effective;
            version_status.tl_reported = tl.evidence.reported;
            version_status.tl_effective = tl.effective;
            version_status.pl_conservative = pl.conservative;
            version_status.tl_conservative = tl.conservative;
            version_status.get_version_outcome =
                pl.evidence.source == calmcar::xcp::VersionSource::GetVersion
                    ? "可用"
                    : "不可用（按显式/A2L 证据解析）";
            std::cout << calmcar::xcp::tools::FormatRuntimeVersionSummary(
                             version_status)
                      << '\n';
            // 版本证据冲突不阻断，但必须可见；降级能力同理。
            for (const auto& conflict : master.VersionConflicts()) {
                std::cout << "  · 版本证据冲突: " << conflict << '\n';
            }
        }
        std::cout << "XCP on "
                  << (selection->transport ==
                              calmcar::xcp::tools::TransportKind::Tcp
                          ? "TCP"
                          : "UDP")
                  << " 已连接: " << options->host << ':' << options->port
                  << '\n';
        // ---- 标定页能力（P2）-------------------------------------------------
        // 页命令是否可用由 A2L 的 OPTIONAL_CMD 决定：Slave 明确声明不支持的命令
        // 不该在每次写入时再去试探（那会换回一个 ERR_CMD_UNKNOWN 并拖慢确认流程）。
        calmcar::xcp::tools::CalibrationPageFacts calibration_page_facts;
        if (const auto* xcp_info = bridge->XcpInfo(); xcp_info != nullptr) {
            calibration_page_facts =
                calmcar::xcp::tools::MakeCalibrationPageFacts(
                    xcp_info->protocol_layer.optional_commands);
        }
        CliApp app(*options, *database, event_bound_database,
                   measurement_session, master, std::move(event_metadata),
                   calibration_page_facts);
        app.Run();
        if (measurement_session.Running()) measurement_session.Stop();
        master.Disconnect();
    } catch (const std::exception& e) {
        LIBXCP_LOG_ERROR("XCP master connection or run failed");
        std::cerr << "XCP 连接/运行失败: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
