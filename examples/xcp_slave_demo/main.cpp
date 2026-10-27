/**
 * @file main.cpp
 * @brief 独立 XCPlite UDP/TCP Slave 与离线 A2L 生成命令行入口。
 */
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "libxcp/a2l/a2l_bridge.hpp"
#include "libxcp/log.hpp"
#include "slave_registry.hpp"

#if LIBXCP_ENABLE_LOG
#include "program_logging.hpp"
#endif
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
/// @brief 默认监听端口（TCP 与 UDP 通用；两者端口空间独立，可用同一端口号）。
constexpr std::uint16_t kDefaultPort = 5555U;
/// @brief 默认 XCP 测量消息队列容量，单位为字节。
constexpr std::uint32_t kMeasurementQueueSize = 32768U;
/// @brief 心跳日志间隔，单位为墙钟时间。
/// @note 早期按 tick 计数（10 ms × 500 = 5 秒），但慢机上事件循环被调度拖慢时逻辑
///       时间跟不上墙钟：CI 实测 9 秒墙钟只推进 265 次事件，心跳因此迟迟不出现。
///       改用墙钟后语义仍是「固定间隔心跳」，且不受事件推进速度影响；心跳判断只读
///       当前时间、不参与 next_fast/next_slow 的补偿计算，故不会干扰实时循环。
constexpr std::chrono::seconds kHeartbeatInterval{5};
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
    /// @brief 是否使用 TCP 传输；false 表示 UDP（默认，保持既有脚本兼容）。
    /// @note XCPlite 的 XcpEthServerInit 是全局单例，传输方式在初始化时定死，
    /// 同一进程无法同时监听两种传输；需要两种时请启动两个进程并使用不同端口。
    bool use_tcp{false};
    std::string bind_address{"127.0.0.1"};  ///< IPv4 监听及 A2L XCP 端点地址。
    std::uint16_t port{kDefaultPort};       ///< 监听端口（UDP 或 TCP）。
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
            argument == "--output-dir" || argument == "--transport") {
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
            } else if (argument == "--transport") {
                // 只接受 udp / tcp 两个小写字面量：拼写错误时立刻失败，
                // 避免"以为切到 TCP 其实还在跑 UDP"这类难以察觉的配置错误。
                if (value == "udp") {
                    options.use_tcp = false;
                } else if (value == "tcp") {
                    options.use_tcp = true;
                } else {
                    error_message = "--transport 必须是 udp 或 tcp";
                    return false;
                }
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
                 "  --port <端口>           监听端口（默认 5555）\n"
                 "  --transport <udp|tcp>   传输方式（默认 udp）\n"
                 "  --overwrite             仅替换主 A2L 与 XCP_104.aml\n"
                 "  --help                  显示本帮助\n"
                 "\n"
                 "  XCPlite 的 XCP server 是全局单例，同一进程只能使用一种传输；\n"
                 "  需要同时提供 UDP 与 TCP 时，请启动两个进程并使用不同端口。\n";
}

/**
 * @brief 返回传输方式的英文短名，用于日志与提示。
 * @param use_tcp 是否使用 TCP。
 * @return TCP 或 UDP 字面量。
 */
std::string_view TransportName(bool use_tcp) {
    return use_tcp ? std::string_view{"TCP"} : std::string_view{"UDP"};
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

/// @brief 标定段 INSTANCE 声明的期望名字（项目前缀之后的部分）。
constexpr char kCalibrationInstanceName[] = "cal";
/// @brief 标定段 INSTANCE 声明的期望类型名。
constexpr char kCalibrationInstanceType[] = "CalibrationParameters";
/// @brief 标定段 INSTANCE 声明的期望描述文本。
constexpr char kCalibrationInstanceComment[] = "独立标定段";

/**
 * @brief 判断字符是否可作为 A2L 记号的分隔空白。
 * @param ch 待判断字符。
 * @return 为空格、制表符、回车或换行时返回 true。
 */
bool IsA2lSpace(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

/**
 * @brief 解析以 "0x" 开头的十六进制无符号整数。
 * @param text 待解析文本，长度至少为 3。
 * @return 解析成功返回值；文本非法或溢出时返回 std::nullopt。
 */
std::optional<std::uint64_t> ParseHexAddress(std::string_view text) noexcept {
    if (text.size() < 3U || text[0] != '0' ||
        (text[1] != 'x' && text[1] != 'X')) {
        return std::nullopt;
    }
    std::uint64_t value = 0U;
    const char* const begin = text.data() + 2;
    const char* const end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, 16);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

/**
 * @brief 在生成模式下为独立标定段的 INSTANCE 声明补齐 READ_WRITE。
 *
 * XCPlite 只在绝对、应用与动态寻址下输出 READ_WRITE（见
 * `thirdparty/XCPlite/src/a2l.c` 的 A2lCreateInstance_），标定段使用的段相对
 * 寻址拿不到该标记，导致主站按 A2L 权限拒绝 cal.* 的静态写资格。本函数只对
 * 唯一一个同时满足「名字 cal + 类型 CalibrationParameters + 描述匹配」的
 * INSTANCE 声明做局部补齐，绝不改动 meas_* 或其他段。
 *
 * 为什么这里补齐、而 tests/a2l_golden_test.cpp 的 CalSeg 语料不补：
 * 段相对寻址（当前默认构建 XCPLITE__CASDD，XCP_ADDR_EXT_SEG=0x00）的标定段
 * 在物理上完全可写——xcplite.c 的 XcpWriteMta/XcpReadMta 都为
 * XcpAddrIsSeg 开了专门分支（XcpCalSegWriteMemory/XcpCalSegReadMemory）。
 * a2l.c 未给 SEG 打 READ_WRITE 属于该白名单只枚举 ABS/APP/DYN 的实现遗漏，
 * 并非「该段不可写」。A2L 是交付给主站的能力描述，标成只读会造成虚假的能力
 * 缺失（本次「DAQ 模式下无法标定」即由此引发），因此本示例主动纠正。
 * a2l_golden_test 的语料是对上游输出的忠实快照（回归基线），职责是记录而非
 * 纠正上游行为，故保持 rw=0 不动。两者目的不同，差异在此显式记录。
 *
 * @param a2l_file 本次生成的 A2L 文件路径。
 * @param error_message 用于接收失败原因的字符串。
 * @return 修正并发布成功返回 true；定位不唯一、字段不符或 I/O 失败返回 false。
 */
bool PatchCalibrationInstanceWriteAccess(const std::filesystem::path& a2l_file,
                                         std::string* error_message) {
    std::string text;
    {
        std::ifstream input(a2l_file, std::ios::binary);
        if (!input) {
            *error_message = "A2L 修正失败：无法打开生成文件";
            return false;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        if (input.bad()) {
            *error_message = "A2L 修正失败：读取生成文件不完整";
            return false;
        }
        text = buffer.str();
    }

    const std::string begin_token = "/begin INSTANCE";
    std::size_t match_count = 0U;
    std::size_t match_insert = std::string::npos;

    std::size_t cursor = 0U;
    while (true) {
        const std::size_t found = text.find(begin_token, cursor);
        if (found == std::string::npos) {
            break;
        }
        // 确认这是独立记号：前一个字符必须是空白或文件起始。
        if (found != 0U && !IsA2lSpace(text[found - 1U])) {
            cursor = found + begin_token.size();
            continue;
        }
        cursor = found + begin_token.size();

        std::vector<std::string_view> fields;
        std::size_t scan = cursor;
        while (scan < text.size() && IsA2lSpace(text[scan])) {
            ++scan;
        }
        // 声明内所有字段都在同一行内以空白分隔，直到 /end INSTANCE。
        const std::size_t line_end = text.find('\n', scan);
        const std::size_t bounds =
            line_end == std::string::npos ? text.size() : line_end;
        while (scan < bounds) {
            std::size_t token_end = scan;
            while (token_end < bounds && !IsA2lSpace(text[token_end])) {
                ++token_end;
            }
            if (token_end == scan) {
                ++scan;
                continue;
            }
            fields.emplace_back(text.data() + scan, token_end - scan);
            scan = token_end;
            while (scan < bounds && IsA2lSpace(text[scan])) {
                ++scan;
            }
        }

        // 声明必须完整闭合：末尾两个字段是 "/end INSTANCE"。
        if (fields.size() < 5U ||
            fields[fields.size() - 2U] != "/end" ||
            fields.back() != "INSTANCE") {
            continue;
        }
        const std::string_view name = fields[0];
        const std::string_view comment_literal = fields[1];
        const std::string_view type_name = fields[2];
        const std::string_view address_text = fields[3];

        if (name != kCalibrationInstanceName ||
            type_name != kCalibrationInstanceType) {
            continue;
        }
        // 描述必须完整匹配带引号的期望文本。
        const std::string expected_literal =
            std::string("\"") + kCalibrationInstanceComment + "\"";
        if (comment_literal != expected_literal) {
            continue;
        }
        // 段相对寻址下该地址只是模式标识值，不具备基址语义，
        // 因此只校验它确实是合法的 0x 十六进制字面量。
        if (!ParseHexAddress(address_text).has_value()) {
            continue;
        }
        // 已经带 READ_WRITE 时视为幂等命中：无需再插入。
        bool already_writable = false;
        for (const auto& field : fields) {
            if (field == "READ_WRITE") {
                already_writable = true;
                break;
            }
        }
        ++match_count;
        if (match_count > 1U) {
            break;
        }
        // 插入点取地址字段结束处，紧跟其后补 READ_WRITE。
        match_insert = static_cast<std::size_t>(address_text.data() -
                                                text.data()) +
                       address_text.size();
        if (already_writable) {
            // 幂等命中：记录后直接成功返回，不做任何改写。
            *error_message = "";
            return true;
        }
    }

    if (match_count == 0U) {
        *error_message = "A2L 修正失败：未找到唯一匹配的标定段 INSTANCE 声明";
        return false;
    }
    if (match_count > 1U) {
        *error_message = "A2L 修正失败：匹配到多个标定段 INSTANCE 声明";
        return false;
    }

    // 先写同目录临时产物，全部成功后再整体替换，避免部分覆写。
    const std::filesystem::path temporary_file =
        a2l_file.parent_path() /
        (a2l_file.filename().string() + ".patch.tmp");
    std::string patched = text;
    patched.insert(match_insert, " READ_WRITE");
    {
        std::ofstream output(temporary_file,
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            *error_message = "A2L 修正失败：无法创建临时文件";
            return false;
        }
        output << patched;
        output.flush();
        if (!output) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temporary_file, ignored);
            *error_message = "A2L 修正失败：写入临时文件不完整";
            return false;
        }
    }
    // 校验临时产物与期望内容完全一致后再发布。
    {
        std::ifstream verify(temporary_file, std::ios::binary);
        std::ostringstream buffer;
        buffer << verify.rdbuf();
        if (!verify || buffer.str() != patched) {
            std::error_code ignored;
            std::filesystem::remove(temporary_file, ignored);
            *error_message = "A2L 修正失败：临时文件内容校验不一致";
            return false;
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary_file, a2l_file, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary_file, ignored);
        *error_message = "A2L 修正失败：发布替换失败";
        return false;
    }
    *error_message = "";
    return true;
}

/// @brief 修正后必须验证可写的标定叶子样例（值类型与结构体成员各一）。
constexpr char kCalibrationProbeLeaves[][2][48] = {
    {"xcp_slave_demo::cal.basic.real_32", "real_32"},
    {"xcp_slave_demo::cal.model.offset", "offset"},
};

/**
 * @brief 断言修正后的 A2L 中标定叶子确实已获得写权限。
 *
 * 只验证解析成功并不足以证明修正生效：必须回到桥接层按名字查到具体叶子，
 * 确认 read_write 已为 true。任一叶子缺失或仍不可写即判定修正失败。
 *
 * @param database 修正后重新加载得到的 A2L 数据库。
 * @param error_message 用于接收失败原因的字符串。
 * @return 全部探针叶子均可写返回 true，否则返回 false。
 */
bool VerifyCalibrationWriteAccess(const calmcar::xcp::a2l::IA2lDatabase& database,
                                  std::string* error_message) {
    for (const auto& probe : kCalibrationProbeLeaves) {
        const auto found = database.Find(probe[0]);
        if (!found.HasValue()) {
            *error_message =
                std::string("A2L 修正后校验失败：找不到标定叶子 ") + probe[0];
            return false;
        }
        if (!found.Value().read_write) {
            *error_message =
                std::string("A2L 修正后校验失败：标定叶子仍不可写 ") + probe[0];
            return false;
        }
    }
    return true;
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
        LIBXCP_LOG_ERROR("A2L 生成失败：要求具体 IPv4 地址，实际传入 0.0.0.0");
        return 2;
    }

    OutputPaths paths;
    std::string error_message;
    if (!PrepareOutput(options, paths, error_message)) {
        std::cerr << error_message << '\n';
        LIBXCP_LOG_ERROR("A2L 生成失败：输出目录准备失败，原因={}",
                         error_message);
        return 2;
    }

    try {
        const WorkingDirectoryGuard working_directory(paths.directory);
        if (!XcpInit(kProjectName, kEpkVersion, XCP_MODE_LOCAL)) {
            error_message = "XcpInit 初始化失败";
            RemoveGeneratedOutputs(paths);
            std::cerr << error_message << '\n';
            LIBXCP_LOG_ERROR("A2L 生成失败：XcpInit 初始化失败，项目名={}",
                             kProjectName);
            return 1;
        }

        // 第三参决定 A2L 写出 XCP_ON_TCP_IP 还是 XCP_ON_UDP_IP
        // （thirdparty/XCPlite/src/a2l_writer.c 的 prot = useTCP ? "TCP" : "UDP"）。
        if (!A2lInit(address.data(), options.port, options.use_tcp,
                     A2L_MODE_WRITE_ALWAYS | A2L_MODE_AUTO_GROUPS)) {
            error_message = "A2lInit 初始化失败";
            RemoveGeneratedOutputs(paths);
            std::cerr << error_message << '\n';
            LIBXCP_LOG_ERROR("A2L 生成失败：A2lInit 初始化失败，端点={}:{}",
                             options.bind_address, options.port);
            return 1;
        }

        SlaveRegistry registry;
        if (!InitializeSlaveRegistry(registry, true, &error_message)) {
            (void)A2lFinalize();
            RemoveGeneratedOutputs(paths);
            std::cerr << "注册 A2L 元数据失败：" << error_message << '\n';
            LIBXCP_LOG_ERROR("A2L 生成失败：注册元数据失败，原因={}",
                             error_message);
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
            LIBXCP_LOG_ERROR(
                "A2L 生成失败：输出完整性检查未通过，finalize={} A2L非空={} "
                "AML非空={} 服务已启动={}",
                finalized, a2l_nonempty, aml_nonempty, XcpEthServerStatus());
            return 1;
        }

        // 在桥接层解析之前补齐标定段的写权限标记：段相对寻址拿不到
        // XCPlite 自动输出的 READ_WRITE，必须在此处严格局部修正。
        if (!PatchCalibrationInstanceWriteAccess(paths.a2l_file,
                                                 &error_message)) {
            RemoveGeneratedOutputs(paths);
            std::cerr << error_message << '\n';
            LIBXCP_LOG_ERROR("A2L 生成失败：{}", error_message);
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
            LIBXCP_LOG_ERROR("A2L 生成失败：桥接层无法解析，原因={}",
                             parse_error);
            return 1;
        }
        const auto* xcp_info = loaded.Value()->XcpInfo();
        // 传输类别必须与 --transport 一致：既校验地址/端口，也校验"这台从站
        // 到底声明成了 UDP 还是 TCP"，否则会把 UDP 的 A2L 交给 TCP 主站使用。
        const auto expected_kind =
            options.use_tcp
                ? calmcar::xcp::a2l::TransportEndpoint::Kind::TcpIp
                : calmcar::xcp::a2l::TransportEndpoint::Kind::UdpIp;
        if (xcp_info == nullptr || xcp_info->transports.size() != 1U ||
            xcp_info->transports.front().kind != expected_kind ||
            xcp_info->transports.front().remote_port != options.port ||
            xcp_info->transports.front().remote_host != options.bind_address) {
            RemoveGeneratedOutputs(paths);
            std::cerr << "A2L 中的 XCP 端点（地址/端口/传输类别）与 CLI 配置不一致\n";
            LIBXCP_LOG_ERROR(
                "A2L 生成失败：A2L 端点与 CLI 配置不一致，期望 {}:{}",
                options.bind_address, options.port);
            return 1;
        }

        // 解析成功不等于权限修正生效：回到桥接层确认标定叶子真的可写。
        if (!VerifyCalibrationWriteAccess(*loaded.Value()->Database(),
                                          &error_message)) {
            RemoveGeneratedOutputs(paths);
            std::cerr << error_message << '\n';
            LIBXCP_LOG_ERROR("A2L 生成失败：{}", error_message);
            return 1;
        }

        std::cout << "A2L 已生成并通过桥接解析：" << paths.a2l_file.string()
                  << '\n'
                  << "外链 AML：" << paths.aml_file.string() << '\n';
        LIBXCP_LOG_INFO("A2L 生成成功：A2L={} AML={} 端点={}:{}",
                        paths.a2l_file.string(), paths.aml_file.string(),
                        options.bind_address, options.port);
        return 0;
    } catch (const std::exception& exception) {
        RemoveGeneratedOutputs(paths);
        std::cerr << "A2L 生成异常：" << exception.what() << '\n';
        LIBXCP_LOG_ERROR("A2L 生成失败：抛出异常，原因={}", exception.what());
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
 * @brief 启动常驻 UDP/TCP Slave，并按两个固定周期更新及触发 DAQ 事件。
 * @param options 已解析的命令行配置。
 * @return 成功运行并完成关闭返回 0，否则返回非零。
 */
int RunSlave(const CommandLineOptions& options) {
    if (!XcpInit(kProjectName, kEpkVersion, XCP_MODE_LOCAL)) {
        std::cerr << "XcpInit 初始化失败\n";
        LIBXCP_LOG_ERROR("从站启动失败：XcpInit 初始化失败，项目名={} EPK={}",
                         kProjectName, kEpkVersion);
        return 1;
    }

    SlaveRegistry registry;
    std::string error_message;
    if (!InitializeSlaveRegistry(registry, false, &error_message)) {
        std::cerr << "初始化 XCPlite 事件/标定段失败：" << error_message
                  << '\n';
        LIBXCP_LOG_ERROR("从站启动失败：事件/标定段初始化失败，原因={}",
                         error_message);
        return 1;
    }

    const auto address = ToIpv4Bytes(options.bind_address);
    // XCPlite 的 XCP server 是全局单例：这里传入的传输方式在进程生命周期内
    // 不可更改，第二次初始化会被 "XCP server already running!" 拒绝。
    if (!XcpEthServerInit(address.data(), options.port, options.use_tcp,
                          kMeasurementQueueSize)) {
        std::cerr << "XcpEthServerInit 失败，无法监听 " << options.bind_address
                  << ':' << options.port << '\n';
        LIBXCP_LOG_ERROR(
            "从站启动失败：XcpEthServerInit 无法监听 {}:{}，队列容量={} 字节",
            options.bind_address, options.port, kMeasurementQueueSize);
        return 1;
    }

    RegisterSlaveProtocolCallbacks();

    std::signal(SIGINT, HandleTerminationSignal);
    std::signal(SIGTERM, HandleTerminationSignal);
    g_keep_running = 1;
    LIBXCP_LOG_INFO(
        "从站已启动：{} {}:{}，快事件 {} ns，慢事件 {} ns，测量队列 {} 字节",
        TransportName(options.use_tcp),
        options.bind_address, options.port, kFastEventCycleNs, kSlowEventCycleNs,
        kMeasurementQueueSize);
    std::cout << "XCPlite " << TransportName(options.use_tcp) << " Slave 正在监听 "
              << options.bind_address << ':'
              << options.port << "（快事件 10 ms，慢事件 100 ms）\n"
              << "按 Ctrl+C 退出。\n";

    using Clock = std::chrono::steady_clock;
    auto next_fast = Clock::now();
    auto next_slow = next_fast + kSlowPeriod;
    auto next_heartbeat = next_fast + kHeartbeatInterval;
    std::uint32_t tick = 0U;
    bool runtime_failure = false;
    while (g_keep_running != 0) {
        const auto now = Clock::now();
        if (now >= next_fast) {
            CalibrationParameters parameters =
                MakeDefaultCalibrationParameters();
            if (!CopyCalibrationSnapshot(registry, parameters)) {
                std::cerr << "无法读取标定段快照，Slave 将停止\n";
                LIBXCP_LOG_ERROR(
                    "从站运行期失败：无法读取标定段快照，累计快事件次数={}",
                    tick);
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

        // 按周期观察协议状态边沿，只在跳变时写日志，不进入实时敏感临界区。
        ObserveSlaveProtocolState();

        // 降频心跳：确认从站在无主站连接时仍持续运行；与边沿日志相互独立。
        if (now >= next_heartbeat) {
            LIBXCP_LOG_INFO(
                "从站心跳：累计快事件次数={}，已连接={}，DAQ 运行中={}",
                tick, XcpIsConnected(), XcpIsDaqRunning());
            next_heartbeat += kHeartbeatInterval;
            if (now - next_heartbeat > kHeartbeatInterval) {
                // 落后超过一个间隔说明进程被长时间挂起，从当前时间重新起算，避免连发。
                next_heartbeat = now + kHeartbeatInterval;
            }
        }
        std::this_thread::sleep_until(std::min(next_fast, next_slow));
    }

    if (!XcpEthServerShutdown()) {
        std::cerr << "XcpEthServerShutdown 返回失败\n";
        LIBXCP_LOG_ERROR("从站关闭失败：XcpEthServerShutdown 返回失败");
        return 1;
    }
    LIBXCP_LOG_INFO("从站已正常退出：累计快事件次数={}，运行期故障={}", tick,
                    runtime_failure);
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
#if LIBXCP_ENABLE_LOG
    calmcar::xcp::internal::ProgramLogSession logging;
    std::string log_error;
    if (!logging.Initialize(argv[0], "examples", log_error)) {
        std::cerr << "日志初始化失败: " << log_error << '\n';
        return 2;
    }
#endif
    LIBXCP_LOG_INFO(
        "从站进程启动：模式={} 绑定={}:{} 输出目录={} A2L版本={}",
        options.generate_a2l ? "生成A2L" : "常驻服务", options.bind_address,
        options.port, options.output_dir.string(), xcp_slave_demo::kEpkVersion);
    if (options.generate_a2l) {
        return xcp_slave_demo::GenerateA2l(options);
    }
    return xcp_slave_demo::RunSlave(options);
}
