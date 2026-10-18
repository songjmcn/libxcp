/**
 * @file main.cpp
 * @brief 独立 XCPlite UDP Slave 与离线 A2L 生成命令行入口。
 */
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "slave_registry.hpp"
#include "xcplib.h"

#ifndef XCP_104_AML_FILE
#error "CMake must define XCP_104_AML_FILE for A2L generation"
#endif
#ifndef XCP_THIRDPARTY_DIR
#error "CMake must define XCP_THIRDPARTY_DIR to protect third-party sources"
#endif

namespace xcp_slave_demo {
namespace {

/// @brief 示例对外使用的固定项目名及 A2L 文件基名。
constexpr char kProjectName[] = "xcp_slave_demo";
/// @brief 用于判断 A2L 与运行进程兼容性的 EPK 版本字符串。
constexpr char kEpkVersion[] = "1.0.0";
/// @brief 默认 UDP 监听端口。
constexpr std::uint16_t kDefaultPort = 5555U;
/// @brief 默认 XCP 测量消息队列容量，单位为字节。
constexpr std::uint32_t kMeasurementQueueSize = 32768U;
/// @brief 快速循环周期，单位为纳秒。
constexpr std::chrono::nanoseconds kFastPeriod{kFastEventCycleNs};
/// @brief 慢速循环周期，单位为纳秒。
constexpr std::chrono::nanoseconds kSlowPeriod{kSlowEventCycleNs};
/// @brief 接收 Ctrl+C / SIGTERM 请求后，控制服务循环退出。
volatile std::sig_atomic_t g_keep_running = 1;

/**
 * @brief 命令行解析后的运行配置。
 */
struct CommandLineOptions {
    bool generate_a2l{false};          ///< 是否只生成 A2L 并在完成后退出。
    bool overwrite{false};             ///< A2L 生成时是否替换两个约定输出文件。
    bool show_help{false};             ///< 是否输出帮助并成功退出。
    bool output_dir_specified{false};  ///< 用户是否显式指定了输出目录。
    std::string bind_address{"127.0.0.1"};  ///< IPv4 监听及 A2L XCP 端点地址。
    std::uint16_t port{kDefaultPort};       ///< UDP 端口。
    std::filesystem::path output_dir{"a2l"};  ///< A2L 与 AML 的输出目录。
};

/**
 * @brief 解析点分十进制 IPv4 字符串。
 * @param text 待解析的地址字符串。
 * @param address 用于接收四个网络地址字节的数组。
 * @return 字符串恰有四个 0 到 255 的十进制分段时返回 true。
 */
bool ParseIpv4(std::string_view text, std::array<std::uint8_t, 4>& address) {
    std::size_t begin = 0;
    for (std::size_t index = 0; index < address.size(); ++index) {
        const std::size_t end = text.find('.', begin);
        const bool final_part = index + 1U == address.size();
        if ((final_part && end != std::string_view::npos) ||
            (!final_part && end == std::string_view::npos)) {
            return false;
        }
        const std::size_t part_end = final_part ? text.size() : end;
        if (part_end == begin) {
            return false;
        }
        unsigned int value = 0;
        const char* first = text.data() + begin;
        const char* last = text.data() + part_end;
        const auto result = std::from_chars(first, last, value, 10);
        if (result.ec != std::errc{} || result.ptr != last || value > 255U) {
            return false;
        }
        address[index] = static_cast<std::uint8_t>(value);
        begin = part_end + 1U;
    }
    return begin == text.size() + 1U;
}

/**
 * @brief 解析 CLI 参数并拒绝不明确或互相冲突的配置。
 * @param argc 参数数量。
 * @param argv 参数数组。
 * @param options 用于接收解析结果的配置。
 * @param error_message 解析失败时的中文说明。
 * @return 参数有效时返回 true。
 */
bool ParseArguments(int argc, char* argv[], CommandLineOptions& options,
                    std::string& error_message) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
            continue;
        }
        if (argument == "--generate-a2l") {
            options.generate_a2l = true;
            continue;
        }
        if (argument == "--overwrite") {
            options.overwrite = true;
            continue;
        }
        if (argument == "--bind" || argument == "--port" ||
            argument == "--output-dir") {
            if (index + 1 >= argc) {
                error_message = "参数缺少取值：" + std::string(argument);
                return false;
            }
            const std::string_view value(argv[++index]);
            if (argument == "--bind") {
                std::array<std::uint8_t, 4> parsed_address{};
                if (!ParseIpv4(value, parsed_address)) {
                    error_message = "--bind 必须是有效的点分十进制 IPv4 地址";
                    return false;
                }
                options.bind_address.assign(value);
            } else if (argument == "--port") {
                unsigned int parsed_port = 0;
                const auto result = std::from_chars(
                    value.data(), value.data() + value.size(), parsed_port, 10);
                if (value.empty() || result.ec != std::errc{} ||
                    result.ptr != value.data() + value.size() ||
                    parsed_port == 0U || parsed_port > 65535U) {
                    error_message = "--port 必须是 1 到 65535 之间的十进制端口";
                    return false;
                }
                options.port = static_cast<std::uint16_t>(parsed_port);
            } else {
                if (value.empty()) {
                    error_message = "--output-dir 不能为空";
                    return false;
                }
                options.output_dir.assign(value);
                options.output_dir_specified = true;
            }
            continue;
        }
        error_message = "无法识别的命令行参数：" + std::string(argument);
        return false;
    }

    if (options.show_help) {
        return true;
    }
    if (options.overwrite && !options.generate_a2l) {
        error_message = "--overwrite 只能与 --generate-a2l 一起使用";
        return false;
    }
    if (options.output_dir_specified && !options.generate_a2l) {
        error_message = "--output-dir 只能与 --generate-a2l 一起使用";
        return false;
    }
    return true;
}

/**
 * @brief 输出命令行帮助。
 */
void PrintUsage() {
    std::cout << "用法：xcp_slave_demo [选项]\n"
                 "  默认启动 UDP XCP Slave：127.0.0.1:5555\n"
                 "  --generate-a2l          只生成 A2L/AML，成功后退出\n"
                 "  --output-dir <目录>     生成文件目录（默认 ./a2l）\n"
                 "  --bind <IPv4>           绑定地址并写入 A2L 端点\n"
                 "  --port <端口>           UDP 端口（默认 5555）\n"
                 "  --overwrite             仅替换主 A2L 与 XCP_104.aml\n"
                 "  --help                  显示本帮助\n";
}

/**
 * @brief 将点分十进制 IPv4 地址转换为 XCPlite 所需的四字节数组。
 * @param text IPv4 地址文本。
 * @return 已验证的网络地址字节。
 */
std::array<std::uint8_t, 4> ToIpv4Bytes(const std::string& text) {
    std::array<std::uint8_t, 4> address{};
    (void)ParseIpv4(text, address);
    return address;
}

/**
 * @brief 已验证的 A2L 输出路径集合。
 */
struct OutputPaths {
    std::filesystem::path directory;  ///< 规范化后的输出目录绝对路径。
    std::filesystem::path a2l_file;   ///< 示例主 A2L 文件路径。
    std::filesystem::path aml_file;   ///< 外链 XCP AML 文件路径。
};

/**
 * @brief 判断规范化路径是否位于给定目录本身或其子目录中。
 * @param candidate 待检查的绝对规范化路径。
 * @param parent 受保护目录的绝对规范化路径。
 * @return candidate 等于 parent 或位于其子目录时返回 true。
 */
bool IsPathWithin(const std::filesystem::path& candidate,
                  const std::filesystem::path& parent) {
    auto candidate_component = candidate.begin();
    for (auto parent_component = parent.begin();
         parent_component != parent.end(); ++parent_component) {
        if (candidate_component == candidate.end() ||
            *candidate_component != *parent_component) {
            return false;
        }
        ++candidate_component;
    }
    return true;
}

/**
 * @brief 检查输出路径并按明确的覆盖策略准备目录。
 * @param options CLI 配置。
 * @param paths 用于接收解析后的输出路径。
 * @param error_message 失败时的中文说明。
 * @return 输出目录及两个约定文件均可安全写入时返回 true。
 */
bool PrepareOutput(const CommandLineOptions& options, OutputPaths& paths,
                   std::string& error_message) {
    std::error_code error;
    const std::filesystem::path aml_source{XCP_104_AML_FILE};
    if (!std::filesystem::is_regular_file(aml_source, error) || error) {
        error_message = "XCPlite AML 源文件不存在：" + aml_source.string();
        return false;
    }

    paths.directory = std::filesystem::absolute(options.output_dir, error);
    if (error) {
        error_message = "无法解析输出目录绝对路径：" + error.message();
        return false;
    }
    paths.directory = paths.directory.lexically_normal();
    const std::filesystem::path thirdparty_directory =
        std::filesystem::weakly_canonical(XCP_THIRDPARTY_DIR, error);
    if (error) {
        error_message = "无法解析 thirdparty 保护目录：" + error.message();
        return false;
    }
    paths.directory = std::filesystem::weakly_canonical(paths.directory, error);
    if (error) {
        error_message = "无法规范化输出目录：" + error.message();
        return false;
    }
    if (IsPathWithin(paths.directory, thirdparty_directory)) {
        error_message = "为保护第三方源码，拒绝在 thirdparty 目录内生成文件";
        return false;
    }

    const bool directory_exists =
        std::filesystem::exists(paths.directory, error);
    if (error) {
        error_message = "无法检查输出目录：" + paths.directory.string();
        return false;
    }
    if (!directory_exists) {
        std::filesystem::create_directories(paths.directory, error);
        if (error) {
            error_message = "无法创建输出目录：" + paths.directory.string();
            return false;
        }
    }
    if (!std::filesystem::is_directory(paths.directory, error) || error) {
        error_message = "输出路径不是目录：" + paths.directory.string();
        return false;
    }

    paths.a2l_file = paths.directory / "xcp_slave_demo.a2l";
    paths.aml_file = paths.directory / "XCP_104.aml";
    const std::array<std::filesystem::path, 2> outputs{paths.a2l_file,
                                                       paths.aml_file};
    for (const auto& output : outputs) {
        if (output.parent_path() != paths.directory) {
            error_message = "输出文件路径超出目标目录";
            return false;
        }
        const bool exists = std::filesystem::exists(output, error);
        if (error) {
            error_message = "无法检查输出文件：" + output.string();
            return false;
        }
        if (!exists) {
            continue;
        }
        if (!options.overwrite) {
            error_message = "输出文件已存在；如需替换请显式指定 --overwrite：" +
                            output.string();
            return false;
        }
        if (!std::filesystem::is_regular_file(output, error) || error) {
            error_message = "拒绝覆盖非普通文件：" + output.string();
            return false;
        }
    }

    if (options.overwrite) {
        for (const auto& output : outputs) {
            if (std::filesystem::exists(output, error) &&
                !std::filesystem::remove(output, error)) {
                error_message = "无法替换输出文件：" + output.string();
                return false;
            }
            if (error) {
                error_message = "无法移除旧输出文件：" + output.string();
                return false;
            }
        }
    }

    std::filesystem::copy_file(aml_source, paths.aml_file,
                               std::filesystem::copy_options::none, error);
    if (error) {
        error_message = "无法复制 XCP_104.aml 到输出目录：" + error.message();
        return false;
    }
    return true;
}

/**
 * @brief 在作用域内切换当前工作目录，并在析构时尽力恢复。
 */
class WorkingDirectoryGuard {
public:
    /**
     * @brief 保存当前目录并切换到指定目录。
     * @param directory 新的工作目录。
     */
    explicit WorkingDirectoryGuard(const std::filesystem::path& directory)
        : original_directory_(std::filesystem::current_path()) {
        std::filesystem::current_path(directory);
        active_ = true;
    }

    /** @brief 尽力恢复构造前的工作目录。 */
    ~WorkingDirectoryGuard() {
        if (active_) {
            std::error_code error;
            std::filesystem::current_path(original_directory_, error);
        }
    }

    WorkingDirectoryGuard(const WorkingDirectoryGuard&) =
        delete;  ///< 禁止复制目录守卫。
    WorkingDirectoryGuard& operator=(const WorkingDirectoryGuard&) =
        delete;  ///< 禁止赋值目录守卫。

private:
    std::filesystem::path original_directory_;  ///< 守卫构造前的工作目录。
    bool active_{false};  ///< 当前对象是否已成功切换目录。
};

/**
 * @brief 删除本次生成模式限定的两个输出文件。
 * @param paths 经过绝对路径校验的输出路径。
 */
void RemoveGeneratedOutputs(const OutputPaths& paths) noexcept {
    std::error_code error;
    std::filesystem::remove(paths.a2l_file, error);
    error.clear();
    std::filesystem::remove(paths.aml_file, error);
}

/**
 * @brief 生成 A2L，校验输出完整性后返回，不启动 Ethernet server。
 * @param options 已解析的命令行配置。
 * @return 成功返回 0，任何校验或写出失败返回非零。
 */
int GenerateA2l(const CommandLineOptions& options) {
    const auto address = ToIpv4Bytes(options.bind_address);
    if (address[0] == 0U && address[1] == 0U && address[2] == 0U &&
        address[3] == 0U) {
        std::cerr
            << "A2L 生成模式要求可连接的具体 IPv4 地址，不能使用 0.0.0.0\n";
        return 2;
    }

    OutputPaths paths;
    std::string error_message;
    if (!PrepareOutput(options, paths, error_message)) {
        std::cerr << error_message << '\n';
        return 2;
    }

    try {
        const WorkingDirectoryGuard working_directory(paths.directory);
        if (!XcpInit(kProjectName, kEpkVersion, XCP_MODE_LOCAL)) {
            error_message = "XcpInit 初始化失败";
            RemoveGeneratedOutputs(paths);
            std::cerr << error_message << '\n';
            return 1;
        }

        if (!A2lInit(address.data(), options.port, false,
                     A2L_MODE_WRITE_ALWAYS | A2L_MODE_AUTO_GROUPS)) {
            error_message = "A2lInit 初始化失败";
            RemoveGeneratedOutputs(paths);
            std::cerr << error_message << '\n';
            return 1;
        }

        SlaveRegistry registry;
        if (!InitializeSlaveRegistry(registry, true, &error_message)) {
            (void)A2lFinalize();
            RemoveGeneratedOutputs(paths);
            std::cerr << "注册 A2L 元数据失败：" << error_message << '\n';
            return 1;
        }

        const bool finalized = A2lFinalize();
        const bool a2l_exists =
            std::filesystem::is_regular_file(paths.a2l_file);
        const bool aml_exists =
            std::filesystem::is_regular_file(paths.aml_file);
        const bool a2l_nonempty =
            a2l_exists && std::filesystem::file_size(paths.a2l_file) > 0U;
        const bool aml_nonempty =
            aml_exists && std::filesystem::file_size(paths.aml_file) > 0U;
        if (!finalized || !a2l_nonempty || !aml_nonempty ||
            XcpEthServerStatus()) {
            RemoveGeneratedOutputs(paths);
            std::cerr
                << "A2L 输出完整性检查失败（Finalize、文件或无网络约束）\n";
            return 1;
        }

        auto loaded =
            calmcar::xcp::a2l::A2lBridge::Load(paths.a2l_file.string());
        if (!loaded.HasValue() || loaded.Value()->Database() == nullptr) {
            const std::string parse_error = loaded.HasValue()
                                                ? "桥接层未返回 A2L 数据库"
                                                : loaded.ErrorInfo().message;
            RemoveGeneratedOutputs(paths);
            std::cerr << "生成的 A2L 无法通过桥接层解析：" << parse_error
                      << '\n';
            return 1;
        }
        const auto* xcp_info = loaded.Value()->XcpInfo();
        if (xcp_info == nullptr || xcp_info->transports.size() != 1U ||
            xcp_info->transports.front().remote_port != options.port ||
            xcp_info->transports.front().remote_host != options.bind_address) {
            RemoveGeneratedOutputs(paths);
            std::cerr << "A2L 中的 XCP/UDP 端点与 CLI 配置不一致\n";
            return 1;
        }

        std::cout << "A2L 已生成并通过桥接解析：" << paths.a2l_file.string()
                  << '\n'
                  << "外链 AML：" << paths.aml_file.string() << '\n';
        return 0;
    } catch (const std::exception& exception) {
        RemoveGeneratedOutputs(paths);
        std::cerr << "A2L 生成异常：" << exception.what() << '\n';
        return 1;
    }
}

/**
 * @brief 接收进程终止信号。
 * @param signal_number 系统传递的信号编号。
 */
void HandleTerminationSignal(int signal_number) {
    (void)signal_number;
    g_keep_running = 0;
}

/**
 * @brief 启动常驻 UDP Slave，并按两个固定周期更新及触发 DAQ 事件。
 * @param options 已解析的命令行配置。
 * @return 成功运行并完成关闭返回 0，否则返回非零。
 */
int RunSlave(const CommandLineOptions& options) {
    if (!XcpInit(kProjectName, kEpkVersion, XCP_MODE_LOCAL)) {
        std::cerr << "XcpInit 初始化失败\n";
        return 1;
    }

    SlaveRegistry registry;
    std::string error_message;
    if (!InitializeSlaveRegistry(registry, false, &error_message)) {
        std::cerr << "初始化 XCPlite 事件/标定段失败：" << error_message
                  << '\n';
        return 1;
    }

    const auto address = ToIpv4Bytes(options.bind_address);
    if (!XcpEthServerInit(address.data(), options.port, false,
                          kMeasurementQueueSize)) {
        std::cerr << "XcpEthServerInit 失败，无法监听 " << options.bind_address
                  << ':' << options.port << '\n';
        return 1;
    }

    std::signal(SIGINT, HandleTerminationSignal);
    std::signal(SIGTERM, HandleTerminationSignal);
    g_keep_running = 1;
    std::cout << "XCPlite UDP Slave 正在监听 " << options.bind_address << ':'
              << options.port << "（快事件 10 ms，慢事件 100 ms）\n"
              << "按 Ctrl+C 退出。\n";

    using Clock = std::chrono::steady_clock;
    auto next_fast = Clock::now();
    auto next_slow = next_fast + kSlowPeriod;
    std::uint32_t tick = 0U;
    bool runtime_failure = false;
    while (g_keep_running != 0) {
        const auto now = Clock::now();
        if (now >= next_fast) {
            CalibrationParameters parameters =
                MakeDefaultCalibrationParameters();
            if (!CopyCalibrationSnapshot(registry, parameters)) {
                std::cerr << "无法读取标定段快照，Slave 将停止\n";
                runtime_failure = true;
                break;
            }
            UpdateFastMeasurements(GetFastMeasurements(), parameters, tick);
            TriggerFastEvent(registry);
            ++tick;
            next_fast += kFastPeriod;
            if (now - next_fast > kFastPeriod) {
                next_fast = now + kFastPeriod;
            }
        }

        if (now >= next_slow) {
            UpdateSlowMeasurements(GetSlowMeasurements(),
                                   GetFastMeasurements());
            TriggerSlowEvent(registry);
            next_slow += kSlowPeriod;
            if (now - next_slow > kSlowPeriod) {
                next_slow = now + kSlowPeriod;
            }
        }
        std::this_thread::sleep_until(std::min(next_fast, next_slow));
    }

    if (!XcpEthServerShutdown()) {
        std::cerr << "XcpEthServerShutdown 返回失败\n";
        return 1;
    }
    return runtime_failure ? 1 : 0;
}

}  // namespace
}  // namespace xcp_slave_demo

/**
 * @brief 解析配置并选择生成 A2L 或常驻服务模式。
 * @param argc 参数数量。
 * @param argv 参数数组。
 * @return 命令的退出码。
 */
int main(int argc, char* argv[]) {
    xcp_slave_demo::CommandLineOptions options;
    std::string error_message;
    if (!xcp_slave_demo::ParseArguments(argc, argv, options, error_message)) {
        std::cerr << error_message << '\n';
        xcp_slave_demo::PrintUsage();
        return 2;
    }
    if (options.show_help) {
        xcp_slave_demo::PrintUsage();
        return 0;
    }
    if (options.generate_a2l) {
        return xcp_slave_demo::GenerateA2l(options);
    }
    return xcp_slave_demo::RunSlave(options);
}
