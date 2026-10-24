/**
 * @file main.cpp
 * @brief 基于 XCPlite V1.2（协议层 1.2 / 传输层 1.4）的 XCP 从站测试程序入口。
 *
 * 本程序作为 libxcp Master 的对手端，用于验证低版本 XCP 协议上的互操作性。
 * 与仓库现有 examples/xcp_slave_demo 的区别：后者链接 XCPlite V2.2.x，
 * 本程序链接 thirdparty/XCPliteV1.2，因此 API 与能力集均按 V1.2 编写。
 *
 * 阶段说明（计划文档 §10）：P1 已完成构建骨架；本文件现已包含 P2（模型与
 * A2L 生成）与 P3（事件线程、DAQ、TCP 传输）内容，标定双页链路见 §5.4。
 */
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "a2l.h"
#include "libxcp/a2l/a2l_bridge.hpp"
#include "slave_model.hpp"
#include "slave_registry.hpp"
#include "xcplib.h"

namespace xcp_slave_demo_v12 {
namespace {

/// @brief 固定项目名；V1.2 的 A2L 文件名由它派生为 "<项目名>.a2l"。
constexpr char kProjectName[] = "xcp_slave_demo_v12";
/// @brief EPK 版本字符串，用于 A2L 与可执行文件的版本标记。
constexpr char kEpkVersion[] = "1.2.0";
/// @brief 默认监听端口。
constexpr std::uint16_t kDefaultPort = 5555U;
/// @brief 默认测量队列容量，单位字节（传输层发送队列，非 DAQ 内存池）。
constexpr std::uint32_t kDefaultQueueSize = 64U * 1024U;
/// @brief 默认 XCPlite 日志级别（1=Error 2=Warn 3=Info 4=Trace）。
constexpr std::uint8_t kDefaultLogLevel = 3U;
/// @brief 启动成功后向 stdout 报告的固定前缀，供父进程/脚本探测。
constexpr char kReadyPrefix[] = "xcp_slave_demo_v12 ready";
/// 标定段在 A2L 中的 INSTANCE 名称（与 slave_registry.cpp 的段名一致）。
constexpr char kCalibrationSegmentName[] = "cal";
/// 标定段在 A2L 中的 INSTANCE 类型名。
constexpr char kCalibrationInstanceType[] = "CalibrationParameters";
/// @brief 主循环轮询间隔，单位毫秒；仅用于检查退出条件，无实时性要求。
constexpr std::chrono::milliseconds kIdlePollInterval{10};
/// @brief 快速事件周期（与 slave_registry.hpp 的 kFastCycleNs 一致）。
constexpr std::chrono::nanoseconds kFastPeriod{kFastCycleNs};
/// @brief 慢速事件周期（与 slave_registry.hpp 的 kSlowCycleNs 一致）。
constexpr std::chrono::nanoseconds kSlowPeriod{kSlowCycleNs};
/// @brief 收到 SIGINT/SIGTERM 后置 0，驱动主循环退出。
volatile std::sig_atomic_t g_keep_running = 1;

/**
 * @brief 传输方式。
 */
enum class Transport {
    Udp,  ///< XCPlite 以太网传输层的 UDP 模式。
    Tcp   ///< XCPlite 以太网传输层的 TCP 模式（帧格式 [LEN][CTR][Packet]）。
};

/**
 * @brief 命令行解析后的运行配置。
 */
struct CommandLineOptions {
    Transport transport{Transport::Udp};           ///< 传输方式，默认 UDP。
    std::string bind_address{"127.0.0.1"};         ///< 监听地址，禁止 0.0.0.0。
    std::uint16_t port{kDefaultPort};              ///< 监听端口。
    std::uint32_t queue_size{kDefaultQueueSize};   ///< 测量队列容量（字节）。
    std::uint8_t log_level{kDefaultLogLevel};      ///< XCPlite 日志级别。
    std::uint32_t max_runtime_ms{0U};              ///< 自动退出时限；0 表示不限制。
    std::string ready_token;                       ///< 非空则在就绪后打印该标记。
    bool a2l_write_once{false};                    ///< A2L 生成模式：false=WRITE_ALWAYS。
    bool generate_a2l{false};                      ///< 仅生成 A2L 后退出，不启动服务。
    bool verify_a2l{false};                        ///< 生成后经桥接层解析并打印摘要。
    bool overwrite{false};                         ///< 允许覆盖已存在的输出文件。
    std::string output_dir;                        ///< A2L 输出目录；空表示当前目录。
    bool show_help{false};                         ///< 是否只打印帮助后退出。
};

/**
 * @brief 将传输方式渲染为命令行文本。
 * @param transport 传输方式。
 * @return "udp" 或 "tcp"。
 */
const char* TransportName(Transport transport) noexcept {
    return transport == Transport::Tcp ? "tcp" : "udp";
}

/**
 * @brief 解析点分十进制 IPv4 字符串。
 * @param text 待解析地址。
 * @param address 用于接收四个地址字节。
 * @return 文本恰为四段 0..255 的十进制数字时返回 true。
 */
bool ParseIpv4(std::string_view text, std::array<std::uint8_t, 4>& address) {
    std::size_t begin = 0;
    for (std::size_t index = 0; index < address.size(); ++index) {
        const std::size_t separator = text.find('.', begin);
        const bool final_part = index + 1U == address.size();
        if ((final_part && separator != std::string_view::npos) ||
            (!final_part && separator == std::string_view::npos)) {
            return false;
        }
        const std::size_t part_end = final_part ? text.size() : separator;
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
 * @brief 把已校验的点分十进制地址转换为 XCPlite 需要的四字节数组。
 * @param text 已验证的地址文本。
 * @return 四字节地址；解析失败时返回全 0。
 */
std::array<std::uint8_t, 4> ToIpv4Bytes(const std::string& text) {
    std::array<std::uint8_t, 4> address{};
    (void)ParseIpv4(text, address);
    return address;
}

/**
 * @brief 解析无符号十进制整数并校验取值范围。
 * @param text 待解析文本。
 * @param minimum 允许的最小值。
 * @param maximum 允许的最大值。
 * @param value 用于接收解析结果。
 * @return 解析成功且落在闭区间内时返回 true。
 */
bool ParseUnsigned(std::string_view text, std::uint64_t minimum,
                   std::uint64_t maximum, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

/**
 * @brief 解析命令行参数。
 * @param argc 参数数量。
 * @param argv 参数数组。
 * @param options 用于接收解析结果。
 * @param error_message 失败时的中文说明。
 * @return 参数合法时返回 true。
 */
bool ParseArguments(int argc, char* argv[], CommandLineOptions& options,
                    std::string& error_message) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
            continue;
        }
        // 布尔开关：不带取值。
        if (argument == "--generate-a2l" || argument == "--verify-a2l" ||
            argument == "--overwrite") {
            if (argument == "--generate-a2l") {
                options.generate_a2l = true;
            } else if (argument == "--verify-a2l") {
                options.verify_a2l = true;
            } else {
                options.overwrite = true;
            }
            continue;
        }
        if (argument == "--transport" || argument == "--bind" ||
            argument == "--port" || argument == "--queue-size" ||
            argument == "--log-level" || argument == "--max-runtime-ms" ||
            argument == "--ready-token" || argument == "--a2l-mode" ||
            argument == "--output-dir") {
            if (index + 1 >= argc) {
                error_message = "参数缺少取值：" + std::string(argument);
                return false;
            }
            const std::string_view value(argv[++index]);
            if (argument == "--transport") {
                if (value == "udp") {
                    options.transport = Transport::Udp;
                } else if (value == "tcp") {
                    options.transport = Transport::Tcp;
                } else {
                    error_message = "--transport 只接受 udp 或 tcp";
                    return false;
                }
            } else if (argument == "--bind") {
                std::array<std::uint8_t, 4> parsed_address{};
                if (!ParseIpv4(value, parsed_address)) {
                    error_message = "--bind 必须是有效的点分十进制 IPv4 地址";
                    return false;
                }
                options.bind_address.assign(value);
            } else if (argument == "--port") {
                std::uint64_t parsed = 0;
                if (!ParseUnsigned(value, 1U, 65535U, parsed)) {
                    error_message = "--port 必须是 1 到 65535 之间的十进制端口";
                    return false;
                }
                options.port = static_cast<std::uint16_t>(parsed);
            } else if (argument == "--queue-size") {
                std::uint64_t parsed = 0;
                if (!ParseUnsigned(value, 1024U, 16U * 1024U * 1024U, parsed)) {
                    error_message =
                        "--queue-size 必须是 1024 到 16777216 之间的字节数";
                    return false;
                }
                options.queue_size = static_cast<std::uint32_t>(parsed);
            } else if (argument == "--log-level") {
                std::uint64_t parsed = 0;
                if (!ParseUnsigned(value, 0U, 5U, parsed)) {
                    error_message = "--log-level 必须是 0 到 5 之间的整数";
                    return false;
                }
                options.log_level = static_cast<std::uint8_t>(parsed);
            } else if (argument == "--max-runtime-ms") {
                std::uint64_t parsed = 0;
                if (!ParseUnsigned(value, 0U, 86'400'000U, parsed)) {
                    error_message =
                        "--max-runtime-ms 必须是 0 到 86400000 之间的毫秒数";
                    return false;
                }
                options.max_runtime_ms = static_cast<std::uint32_t>(parsed);
            } else if (argument == "--a2l-mode") {
                if (value == "write-always") {
                    options.a2l_write_once = false;
                } else if (value == "write-once") {
                    options.a2l_write_once = true;
                } else {
                    error_message = "--a2l-mode 只接受 write-always 或 write-once";
                    return false;
                }
            } else if (argument == "--output-dir") {
                if (value.empty()) {
                    error_message = "--output-dir 不能为空";
                    return false;
                }
                options.output_dir.assign(value);
            } else {
                if (value.empty()) {
                    error_message = "--ready-token 不能为空";
                    return false;
                }
                options.ready_token.assign(value);
            }
            continue;
        }
        error_message = "无法识别的命令行参数：" + std::string(argument);
        return false;
    }

    if (options.show_help) {
        return true;
    }
    // 绑定 0.0.0.0 时 XCPlite 的 A2L 生成器不会写出传输层端点（a2l.c:628 要求
    // 地址首字节非 0），主站将无法从 A2L 得到连接信息，故直接拒绝。
    const auto address = ToIpv4Bytes(options.bind_address);
    if (address[0] == 0U && address[1] == 0U && address[2] == 0U &&
        address[3] == 0U) {
        error_message =
            "--bind 必须是具体可连接地址（例如 127.0.0.1），不能使用 0.0.0.0";
        return false;
    }
    // OVERWRITE 只作用于生成本身；常驻服务模式下 A2L 每次启动都重写。
    if (options.overwrite && !options.generate_a2l) {
        error_message = "--overwrite 只用于 --generate-a2l；服务模式总是重写 A2L";
        return false;
    }
    return true;
}

/**
 * @brief 打印命令行帮助。
 */
void PrintUsage() {
    std::cout
        << "用法：xcp_slave_demo_v12 [选项]\n"
           "  基于 XCPlite V1.2 的 XCP 从站（协议层 1.2 / 传输层 1.4）\n"
           "  --transport udp|tcp          传输方式（默认 udp）；两种同时需要则起两个进程\n"
           "  --bind <IPv4>                监听地址（默认 127.0.0.1，禁止 0.0.0.0）\n"
           "  --port <端口>                监听端口（默认 5555）\n"
           "  --queue-size <字节>          测量队列容量（默认 65536）\n"
           "  --a2l-mode write-always|write-once  A2L 生成模式（默认 write-always）\n"
           "  --output-dir <目录>          A2L 输出目录（默认当前目录）\n"
           "  --generate-a2l               仅生成 A2L 与 AML 空壳后退出\n"
           "  --overwrite                  允许覆盖已存在的 A2L（仅生成模式）\n"
           "  --verify-a2l                 生成后用 libxcp::a2lbridge 解析并打印摘要\n"
           "  --log-level <0..5>           XCPlite 日志级别（默认 3）\n"
           "  --max-runtime-ms <n>         运行 n 毫秒后自动退出（0=不限制，默认 0）\n"
           "  --ready-token <标记>         就绪后向 stdout 打印该标记，供脚本探测\n"
           "  --help                       显示本帮助\n";
}

/**
 * @brief 进程终止信号处理：请求主循环退出。
 * @param signal_number 系统信号编号（未使用）。
 */
void HandleTerminationSignal(int signal_number) {
    (void)signal_number;
    g_keep_running = 0;
}

/**
 * @brief 安装 SIGINT/SIGTERM 处理并复位运行标志。
 */
void InstallSignalHandlers() {
    g_keep_running = 1;
    std::signal(SIGINT, HandleTerminationSignal);
    std::signal(SIGTERM, HandleTerminationSignal);
}

/**
 * @brief 在作用域内切换当前工作目录，并在析构时尽力恢复。
 * @note XCPlite V1.2 的 A2L 文件名是**相对路径**（`<项目名>.a2l`），且没有
 *       V2.2 的 `A2lGetFilename` 可用来改路径，因此只能靠切换工作目录把生成
 *       物落到目标目录。
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
 * @brief A2L 输出位置（绝对路径）。
 */
struct A2lPaths {
    std::filesystem::path directory;  ///< 输出目录。
    std::filesystem::path a2l_file;   ///< 主 A2L 文件。
    std::filesystem::path aml_file;    ///< 外链 AML 空壳。
};

/**
 * @brief 判断路径是否位于第三方源码树内。
 * @param candidate 待检查路径（已规范化）。
 * @param thirdparty 第三方根目录（已规范化）。
 * @return candidate 在 thirdparty 之内或与之相同时返回 true。
 */
bool IsPathWithin(const std::filesystem::path& candidate,
                  const std::filesystem::path& thirdparty) {
    auto candidate_it = candidate.begin();
    for (auto base_it = thirdparty.begin(); base_it != thirdparty.end();
         ++base_it, ++candidate_it) {
        if (base_it->empty()) {
            continue;
        }
        if (candidate_it == candidate.end() || *candidate_it != *base_it) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 准备 A2L 输出目录并把 AML 空壳复制进去。
 * @param options 命令行配置。
 * @param paths 输出：解析后的输出路径。
 * @param error_message 失败时的中文说明。
 * @return 成功返回 true。
 * @note AML 源路径来自 CMake 注入的 `XCP_104_AML_FILE`，不写死开发机路径。
 */
bool PrepareA2lOutput(const CommandLineOptions& options, A2lPaths& paths,
                      std::string& error_message) {
    std::error_code error;
    const std::filesystem::path base =
        options.output_dir.empty() ? std::filesystem::current_path()
                                   : std::filesystem::path(options.output_dir);
    paths.directory = std::filesystem::absolute(base, error);
    if (error) {
        error_message = "无法解析 A2L 输出目录：" + base.string();
        return false;
    }
    paths.directory = paths.directory.lexically_normal();

#ifdef XCP_THIRDPARTY_DIR
    const std::filesystem::path thirdparty =
        std::filesystem::absolute(XCP_THIRDPARTY_DIR, error);
    if (!error &&
        IsPathWithin(paths.directory, thirdparty.lexically_normal())) {
        error_message = "为保护第三方源码，拒绝在 thirdparty 目录内生成文件";
        return false;
    }
#endif

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

    // 文件名规则见计划 §1.4：WRITE_ALWAYS → "<项目名>.a2l"；
    // WRITE_ONCE → "<项目名>_<EPK>.a2l"。
    paths.a2l_file =
        paths.directory /
        (options.a2l_write_once
             ? std::filesystem::path(std::string(kProjectName) + "_" +
                                     kEpkVersion + ".a2l")
             : std::filesystem::path(std::string(kProjectName) + ".a2l"));
    paths.aml_file = paths.directory / "XCP_104.aml";

#ifdef XCP_104_AML_FILE
    const std::filesystem::path aml_source = XCP_104_AML_FILE;
#else
    const std::filesystem::path aml_source = "XCP_104.aml";
#endif

    // AML 每次都重写：它是静态空壳，主 A2L 每次启动可能变（绝对地址随进程变）。
    std::filesystem::copy_file(
        aml_source, paths.aml_file,
        std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        error_message = "无法复制 XCP_104.aml 到输出目录：" + error.message();
        return false;
    }
    return true;
}

/**
 * @brief 检查已存在的 A2L 是否允许被覆盖。
 * @param paths 输出路径。
 * @param overwrite 是否显式允许覆盖。
 * @param error_message 失败时的中文说明。
 * @return 允许继续时返回 true。
 * @note 仅在 `write-once` 且未指定 `--overwrite` 时可能拒绝：该模式下生成器
 *       自身也不会重写已存在的文件，此处提前给出可读错误而不是让库静默跳过。
 */
bool CheckOverwritePolicy(const A2lPaths& paths, bool overwrite,
                          std::string& error_message) {
    std::error_code error;
    const bool exists = std::filesystem::exists(paths.a2l_file, error);
    if (error) {
        error_message = "无法检查 A2L 输出文件：" + paths.a2l_file.string();
        return false;
    }
    if (!exists || overwrite) {
        return true;
    }
    error_message = "A2L 已存在；如需替换请显式指定 --overwrite：" +
                    paths.a2l_file.string();
    return false;
}

/**
 * @brief 判断字符是否为 A2L 文本中的空白分隔符。
 * @param c 待判断字符。
 * @return 是空格/制表/回车/换行时返回 true。
 */
bool IsA2lSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/**
 * @brief 为标定段 INSTANCE 补上 READ_WRITE 写入许可。
 * @param a2l_file 生成后的主 A2L 文件路径。
 * @param error_message 失败时的中文说明。
 * @return 已补齐（或本就带标记）时返回 true。
 * @note 存在的理由：V1.2 生成器在段相对寻址模式下不输出 `READ_WRITE`
 *       （`thirdparty/XCPliteV1.2/src/a2l.c` 的 INSTANCE 写出分支），而本仓库
 *       主站把 A2L 的 `READ_WRITE` 当作标定写入资格的前提
 *       （`tools/xcp_master/symbol_policy.hpp:137`）。V2.2 示例
 *       `examples/xcp_slave_demo` 对同类问题采用了同样的补丁，此处保持一致。
 *       补丁只改写这一行，仍然保留 XCPlite 生成的其余全部内容。
 */
bool PatchCalibrationInstanceWriteAccess(const std::filesystem::path& a2l_file,
                                         std::string& error_message) {
    std::ifstream input(a2l_file, std::ios::binary);
    if (!input) {
        error_message = "无法读取 A2L 以补写 READ_WRITE：" + a2l_file.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string text = buffer.str();

    constexpr char kInstanceToken[] = "/begin INSTANCE";
    constexpr char kReadWriteToken[] = "READ_WRITE";
    constexpr char kEndInstanceToken[] = " /end INSTANCE";

    std::size_t matches = 0U;
    std::size_t insert_at = std::string::npos;
    std::size_t search_from = 0U;
    while (true) {
        const std::size_t found = text.find(kInstanceToken, search_from);
        if (found == std::string::npos) {
            break;
        }
        search_from = found + sizeof(kInstanceToken) - 1U;
        // 要求该记号起始处为空白，避免匹配到 "/begin INSTANCE_FOO" 之类。
        if (found > 0U && !IsA2lSpace(text[found - 1U])) {
            continue;
        }
        const std::size_t line_end = text.find('\n', found);
        const std::string line =
            text.substr(found, line_end == std::string::npos
                                   ? std::string::npos
                                   : line_end - found);
        // 只在标定段的 INSTANCE 上动手：第二个字段是段名，第三个字段是类型名。
        if (line.find(std::string(" ") + kCalibrationSegmentName + " ") ==
                std::string::npos ||
            line.find(std::string(" ") + kCalibrationInstanceType + " ") ==
                std::string::npos) {
            continue;
        }
        ++matches;
        if (line.find(kReadWriteToken) != std::string::npos) {
            continue;  // 已带标记：幂等命中。
        }
        const std::size_t closing = line.find(kEndInstanceToken);
        if (closing == std::string::npos) {
            error_message =
                "A2L 修正失败：标定段 INSTANCE 声明未以 /end INSTANCE 闭合";
            return false;
        }
        insert_at = found + closing;
    }

    if (matches == 0U) {
        error_message = "A2L 修正失败：未找到标定段 INSTANCE 声明";
        return false;
    }
    if (matches > 1U) {
        error_message = "A2L 修正失败：匹配到多个标定段 INSTANCE 声明";
        return false;
    }
    if (insert_at == std::string::npos) {
        return true;  // 唯一匹配且已带 READ_WRITE。
    }

    text.insert(insert_at, std::string(" ") + kReadWriteToken);

    // 先写临时文件并回读校验，再原子替换，避免半截文件。
    const std::filesystem::path temporary = a2l_file.string() + ".patch.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error_message = "无法写入临时文件：" + temporary.string();
            return false;
        }
        output << text;
        if (!output) {
            error_message = "写入临时文件失败：" + temporary.string();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    {
        std::ifstream check(temporary, std::ios::binary);
        std::ostringstream again;
        again << check.rdbuf();
        if (again.str() != text) {
            error_message = "临时文件回读校验失败：" + temporary.string();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, a2l_file, error);
    if (error) {
        error_message = "替换 A2L 失败：" + error.message();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

/**
 * @brief 用桥接层解析生成的 A2L 并打印摘要。
 * @param a2l_file 主 A2L 文件路径。
 * @param options 命令行配置（用于比对端点）。
 * @param error_message 失败/不一致时的中文说明。
 * @return 解析成功且端点与 CLI 一致时返回 true。
 */
bool VerifyGeneratedA2l(const std::filesystem::path& a2l_file,
                        const CommandLineOptions& options,
                        std::string& error_message) {
    auto loaded = calmcar::xcp::a2l::A2lBridge::Load(a2l_file.string());
    if (!loaded.HasValue()) {
        error_message = "桥接层解析失败：" + loaded.ErrorInfo().message;
        return false;
    }
    const auto* database = loaded.Value()->Database();
    if (database == nullptr) {
        error_message = "桥接层未返回 A2L 数据库";
        return false;
    }
    const auto* xcp_info = loaded.Value()->XcpInfo();
    if (xcp_info == nullptr || !xcp_info->ok) {
        error_message = "生成的 A2L 缺少有效的 IF_DATA XCP 段";
        return false;
    }

    const auto symbol_count = database->Count();
    std::cout << "A2L 自校验：符号数="
              << (symbol_count.HasValue()
                      ? std::to_string(symbol_count.Value())
                      : std::string("不可用"))
              << "，协议层版本=0x"
              << [](std::uint16_t version) {
                     char text[8] = {};
                     std::snprintf(text, sizeof(text), "%04X",
                                   static_cast<unsigned int>(version));
                     return std::string(text);
                 }(xcp_info->protocol_layer.version)
              << "，MAX_CTO="
              << static_cast<unsigned int>(xcp_info->protocol_layer.max_cto)
              << "，MAX_DTO=" << xcp_info->protocol_layer.max_dto << '\n';

    std::cout << "A2L 传输端点数量=" << xcp_info->transports.size();
    for (const auto& endpoint : xcp_info->transports) {
        std::cout << " [" << (endpoint.kind ==
                                      calmcar::xcp::a2l::TransportEndpoint::Kind::
                                          TcpIp
                                  ? "TCP"
                                  : "UDP")
                  << ' ' << endpoint.remote_host << ':' << endpoint.remote_port
                  << " TL=0x"
                  << [](std::uint16_t version) {
                         char text[8] = {};
                         std::snprintf(text, sizeof(text), "%04X",
                                       static_cast<unsigned int>(version));
                         return std::string(text);
                     }(endpoint.version)
                  << " align=" << static_cast<unsigned int>(endpoint.packet_alignment)
                  << "bit]";
    }
    std::cout << '\n' << "A2L 事件通道=" << xcp_info->event_channels.size() << "：";
    for (const auto& event : xcp_info->event_channels) {
        std::cout << ' ' << event.name << "(DAQ_LIST=" << event.daq_list_numbers.size()
                  << ')';
    }
    std::cout << '\n' << "A2L DAQ 能力：STATIC="
              << (xcp_info->daq.has_value() && xcp_info->daq->static_supported
                      ? "支持"
                      : "未声明")
              << "，DYNAMIC="
              << (xcp_info->daq.has_value() && xcp_info->daq->dynamic_supported
                      ? "支持"
                      : "未声明")
              << "，TIMESTAMP 位宽="
              << (xcp_info->daq.has_value() &&
                          xcp_info->daq->timestamp_max_size_bits.has_value()
                      ? std::to_string(*xcp_info->daq->timestamp_max_size_bits)
                      : std::string("未声明"))
              << '\n';

    if (xcp_info->transports.size() != 1U) {
        error_message = "A2L 中的传输端点数量应为 1，实际为 " +
                        std::to_string(xcp_info->transports.size());
        return false;
    }
    const auto& endpoint = xcp_info->transports.front();
    const auto expected_kind =
        options.transport == Transport::Tcp
            ? calmcar::xcp::a2l::TransportEndpoint::Kind::TcpIp
            : calmcar::xcp::a2l::TransportEndpoint::Kind::UdpIp;
    if (endpoint.kind != expected_kind || endpoint.remote_port != options.port ||
        endpoint.remote_host != options.bind_address) {
        error_message =
            "A2L 中的传输端点与 CLI 配置不一致（期望 " +
            std::string(TransportName(options.transport)) + " " +
            options.bind_address + ":" + std::to_string(options.port) + "）";
        return false;
    }
    if (xcp_info->event_channels.size() != 2U) {
        error_message = "A2L 中的事件通道数量应为 2，实际为 " +
                        std::to_string(xcp_info->event_channels.size());
        return false;
    }
    return true;
}

/**
 * @brief 按固定周期推进模型并触发一个 DAQ 事件。
 * @param registry 已初始化的注册表。
 * @param model 从站模型。
 * @param model_mutex 保护模型的互斥量（两个事件线程共享）。
 * @param stop 停止标志。
 * @param period 事件周期。
 * @param slow true 表示慢速事件（调用 `StepSlow`）。
 * @note 锁的覆盖范围包含「读标定页 → 推进模型 → 触发事件」三步：XCPlite 的
 *       DAQ 采样发生在 `XcpEvent()` 内部，因此必须与模型更新处于同一临界区，
 *       否则会采到更新到一半的结构体。
 */
void RunEventLoop(const SlaveRegistry& registry, SlaveModel& model,
                  std::mutex& model_mutex, const std::atomic<bool>& stop,
                  std::chrono::nanoseconds period, bool slow) {
    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();
    while (!stop.load(std::memory_order_relaxed)) {
        next += period;
        std::this_thread::sleep_until(next);
        if (stop.load(std::memory_order_relaxed)) {
            break;
        }
        std::lock_guard<std::mutex> lock(model_mutex);
        CalibrationParameters parameters{};
        if (CopyCalibrationSnapshot(registry, parameters)) {
            // 每个周期都从**当前活动标定页**取参数，页切换后立刻生效。
            model.SetCalibration(parameters);
        }
        if (slow) {
            model.StepSlow();
            TriggerSlowEvent(registry);
        } else {
            model.StepFast();
            TriggerFastEvent(registry);
        }
        // 若本轮处理已经落后于计划时刻，丢弃积压而不是补跑（避免突发）。
        const auto now = Clock::now();
        if (next < now) {
            next = now;
        }
    }
}

/**
 * @brief 启动常驻 XCP 从站，直到收到终止信号或到达运行时限。
 * @param options 已解析的命令行配置。
 * @return 0 正常；1 关闭失败；2 参数错误；3 初始化失败；
 *         4 A2L 输出准备、生成或自校验失败。
 */
int RunSlave(const CommandLineOptions& options) {
    const auto address = ToIpv4Bytes(options.bind_address);

    A2lPaths paths;
    std::string error_message;
    if (!PrepareA2lOutput(options, paths, error_message)) {
        std::cerr << error_message << '\n';
        return 4;
    }
    if (options.a2l_write_once && !options.generate_a2l &&
        !CheckOverwritePolicy(paths, false, error_message)) {
        std::cerr << error_message << '\n';
        return 4;
    }

    const WorkingDirectoryGuard working_directory(paths.directory);

    // V1.2 的 XcpInit 返回 void（V2.2 返回 bool），第三参 activate 为 true
    // 表示同时激活协议层；因此用 XcpIsActivated() 反查是否成功。
    XcpInit(kProjectName, kEpkVersion, true);
    if (!XcpIsActivated()) {
        std::cerr << "XcpInit 初始化失败（项目名=" << kProjectName << "）\n";
        return 3;
    }
    XcpSetLogLevel(options.log_level);

    const bool use_tcp = options.transport == Transport::Tcp;
    const std::uint8_t a2l_mode =
        static_cast<std::uint8_t>((options.a2l_write_once ? A2L_MODE_WRITE_ONCE
                                                         : A2L_MODE_WRITE_ALWAYS) |
                                  A2L_MODE_AUTO_GROUPS);
    if (!A2lInit(address.data(), options.port, use_tcp, a2l_mode)) {
        std::cerr << "A2lInit 初始化失败\n";
        return 3;
    }

    SlaveModel model;
    const FastMeasurements& fast = model.Fast();
    const SlowMeasurements& slow = model.Slow();

    SlaveRegistry registry;
    if (!InitializeSlaveRegistry(registry, fast, slow, true, &error_message)) {
        (void)A2lFinalize();
        std::cerr << "注册 A2L 元数据失败：" << error_message << '\n';
        return 3;
    }

    if (!A2lFinalize()) {
        std::cerr << "A2lFinalize 失败\n";
        return 4;
    }
    if (!std::filesystem::is_regular_file(paths.a2l_file)) {
        std::cerr << "A2L 未生成：" << paths.a2l_file.string() << '\n';
        return 4;
    }
    std::cout << "A2L 已生成：" << paths.a2l_file.string()
              << "（外链 " << paths.aml_file.filename().string() << "）\n";

    // 补上标定段的写入许可，使本仓库主站认可标定段可写（见函数注释）。
    if (!PatchCalibrationInstanceWriteAccess(paths.a2l_file, error_message)) {
        std::cerr << error_message << '\n';
        return 4;
    }

    if (options.verify_a2l &&
        !VerifyGeneratedA2l(paths.a2l_file, options, error_message)) {
        std::cerr << error_message << '\n';
        return 4;
    }

    if (options.generate_a2l) {
        std::cout << "仅生成模式：A2L 输出完成，不启动服务\n";
        return 0;
    }

    // TCP 与 UDP 在 XCPlite 中是全局单例二选一：XcpEthServerInit 内部的
    // XcpEthTlInit 只能成功一次（xcpEthServer.c:76-121）。
    if (!XcpEthServerInit(address.data(), options.port, use_tcp,
                          options.queue_size)) {
        std::cerr << "XcpEthServerInit 失败，无法在 " << options.bind_address
                  << ':' << options.port << " 上监听 "
                  << TransportName(options.transport) << '\n';
        return 3;
    }

    InstallSignalHandlers();
    std::cout << kReadyPrefix << "：" << TransportName(options.transport) << ' '
              << options.bind_address << ':' << options.port
              << "（协议层 1.2 / 传输层 1.4）\n"
              << "按 Ctrl+C 退出。\n";
    if (!options.ready_token.empty()) {
        std::cout << options.ready_token << '\n' << std::flush;
    }

    std::atomic<bool> stop{false};
    std::mutex model_mutex;
    std::thread fast_thread(RunEventLoop, std::cref(registry), std::ref(model),
                            std::ref(model_mutex), std::cref(stop), kFastPeriod,
                            false);
    std::thread slow_thread(RunEventLoop, std::cref(registry), std::ref(model),
                            std::ref(model_mutex), std::cref(stop), kSlowPeriod,
                            true);

    using Clock = std::chrono::steady_clock;
    const auto started_at = Clock::now();
    // --max-runtime-ms 是**整个进程**的时限，故把已经消耗在启动上的时间算进去。
    const auto deadline =
        options.max_runtime_ms == 0U
            ? Clock::time_point::max()
            : started_at + std::chrono::milliseconds(options.max_runtime_ms);

    while (g_keep_running != 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(kIdlePollInterval);
    }

    stop.store(true, std::memory_order_relaxed);
    fast_thread.join();
    slow_thread.join();

    // 服务状态用于在关闭前区分「正常关闭」与「服务已提前失效」。
    const bool was_running = XcpEthServerStatus();
    if (!XcpEthServerShutdown()) {
        std::cerr << "XcpEthServerShutdown 返回失败\n";
        return 1;
    }
    std::cout << "从站已退出：" << TransportName(options.transport) << ' '
              << options.bind_address << ':' << options.port
              << "，关闭前服务状态=" << (was_running ? "运行中" : "已停止")
              << "，快速周期=" << model.FastTick()
              << "，慢速周期=" << model.SlowTick() << '\n';
    return 0;
}

}  // namespace
}  // namespace xcp_slave_demo_v12

/**
 * @brief 进程入口：解析参数并运行常驻从站（或仅生成 A2L）。
 * @param argc 参数数量。
 * @param argv 参数数组。
 * @return 退出码：0 正常、2 参数错误、3 初始化失败、4 A2L 生成失败、1 关闭失败。
 */
int main(int argc, char* argv[]) {
    xcp_slave_demo_v12::CommandLineOptions options;
    std::string error_message;
    if (!xcp_slave_demo_v12::ParseArguments(argc, argv, options,
                                            error_message)) {
        std::cerr << error_message << '\n';
        xcp_slave_demo_v12::PrintUsage();
        return 2;
    }
    if (options.show_help) {
        xcp_slave_demo_v12::PrintUsage();
        return 0;
    }
    return xcp_slave_demo_v12::RunSlave(options);
}