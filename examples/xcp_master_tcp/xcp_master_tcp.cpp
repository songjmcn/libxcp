/**
 * @file xcp_master_tcp.cpp
 * @brief TCP XCP Master 端到端示例：A2L 加载、物理测量与显式标定。
 */
#include "../xcp_master_udp/demo_process.hpp"

#include <libxcp/a2l/a2l_bridge.hpp>
#include <libxcp/tcp_transport.hpp>
#include <libxcp/xcp_master.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef XCP_SLAVE_DEMO_EXECUTABLE
#error "XCP_SLAVE_DEMO_EXECUTABLE must be supplied by CMake"
#endif

namespace {

namespace fs = std::filesystem;
namespace xcp = calmcar::xcp;
namespace a2l = calmcar::xcp::a2l;

/// @brief 应用运行模式。
enum class Mode { Automatic, External };

/**
 * @brief 命令行选项。
 */
struct Options {
    Mode mode{Mode::Automatic};                    ///< 自动从站或外部从站模式。
    std::string host{"127.0.0.1"};                 ///< TCP 从站 IPv4 地址。
    std::uint16_t port{5555};                      ///< TCP 从站端口。
    fs::path run_directory{"xcp_master_tcp_run"};  ///< 自动模式生成文件目录。
    fs::path a2l_file;                             ///< A2L 文件路径。
    std::string measurement{"xcp_slave_demo::meas_fast.input"};  ///< 测量符号。
    std::string calibration_symbol;  ///< 标定符号；自动模式可采用示例默认值。
    double calibration_value{0.0};   ///< 标定目标物理值。
    bool write_calibration{false};   ///< 是否明确启用标定写入。
    bool have_calibration_value{false};  ///< 是否传入标定目标值。
    bool have_measurement{false};        ///< 是否显式传入测量符号。
    bool have_profile{false};            ///< 是否显式传入 TCP profile。
    bool have_alignment{false};          ///< 是否显式传入 packet alignment。
    bool have_host{false};               ///< 是否显式传入目标地址。
    bool have_port{false};               ///< 是否显式传入目标端口。
    bool have_run_directory{false};      ///< 是否显式传入自动模式工作目录。
    xcp::TcpWireProfile profile{
        xcp::TcpWireProfile::Unknown};  ///< TCP framing profile。
    xcp::PacketAlignment alignment{
        xcp::PacketAlignment::Unknown};  ///< 包对齐。
};

/// @brief 打印完整命令行用法。
void PrintUsage() {
    std::cout
        << "用法：xcp_master_tcp [--mode auto|external] [选项]\n"
        << "自动模式（默认）：--run-dir DIR [--port N] [--write-calibration "
           "--calibration-value VALUE [--calibration-symbol NAME]]\n"
        << "外部模式：--mode external --host IPv4 --port N --a2l FILE "
           "--tcp-profile asam-v1.4|asam-v1.5 --alignment 8|16|32 "
           "--measurement SYMBOL\n"
        << "外部标定：--write-calibration --calibration-symbol SYMBOL "
           "--calibration-value VALUE\n";
}

/// @brief 将十进制端口文本解析为非零 TCP 端口。
std::optional<std::uint16_t> ParsePort(std::string_view text) {
    try {
        std::size_t consumed = 0;
        const unsigned long value = std::stoul(std::string(text), &consumed);
        if (consumed != text.size() || value == 0 || value > 65535UL) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// @brief 按命令行参数解析 TCP profile。
std::optional<xcp::TcpWireProfile> ParseProfile(std::string_view text) {
    if (text == "asam-v1.4" || text == "v1.4") {
        return xcp::TcpWireProfile::AsamV14;
    }
    if (text == "asam-v1.5" || text == "v1.5") {
        return xcp::TcpWireProfile::AsamV15;
    }
    return std::nullopt;
}

/// @brief 按命令行参数解析 packet alignment。
std::optional<xcp::PacketAlignment> ParseAlignment(std::string_view text) {
    if (text == "8") return xcp::PacketAlignment::Bit8;
    if (text == "16") return xcp::PacketAlignment::Bit16;
    if (text == "32") return xcp::PacketAlignment::Bit32;
    return std::nullopt;
}

/// @brief 解析有限数值标定目标，拒绝 NaN 与无穷值。
std::optional<double> ParseFiniteDouble(std::string_view text) {
    try {
        std::size_t consumed = 0;
        const double value = std::stod(std::string(text), &consumed);
        if (consumed != text.size() || !std::isfinite(value))
            return std::nullopt;
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// @brief 从 argv 解析配置并 fail-closed 校验模式之间的互斥参数。
bool ParseArgs(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        const auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string_view(argv[++i]);
        };
        if (argument == "--help" || argument == "-h") {
            PrintUsage();
            return false;
        }
        if (argument == "--mode") {
            const auto text = value();
            if (!text) {
                std::cerr << "--mode 缺少参数\n";
                return false;
            }
            if (*text == "auto")
                options.mode = Mode::Automatic;
            else if (*text == "external")
                options.mode = Mode::External;
            else {
                std::cerr << "--mode 仅支持 auto 或 external\n";
                return false;
            }
        } else if (argument == "--host") {
            const auto text = value();
            if (!text || text->empty()) {
                std::cerr << "--host 缺少参数\n";
                return false;
            }
            options.host = *text;
            options.have_host = true;
        } else if (argument == "--port") {
            const auto text = value();
            const auto port = text ? ParsePort(*text) : std::nullopt;
            if (!port) {
                std::cerr << "--port 必须是 1..65535\n";
                return false;
            }
            options.port = *port;
            options.have_port = true;
        } else if (argument == "--run-dir") {
            const auto text = value();
            if (!text || text->empty()) {
                std::cerr << "--run-dir 缺少参数\n";
                return false;
            }
            options.run_directory = *text;
            options.have_run_directory = true;
        } else if (argument == "--a2l") {
            const auto text = value();
            if (!text || text->empty()) {
                std::cerr << "--a2l 缺少参数\n";
                return false;
            }
            options.a2l_file = *text;
        } else if (argument == "--measurement") {
            const auto text = value();
            if (!text || text->empty()) {
                std::cerr << "--measurement 缺少参数\n";
                return false;
            }
            options.measurement = *text;
            options.have_measurement = true;
        } else if (argument == "--tcp-profile") {
            const auto text = value();
            const auto profile = text ? ParseProfile(*text) : std::nullopt;
            if (!profile) {
                std::cerr << "--tcp-profile 仅支持 asam-v1.4 或 asam-v1.5\n";
                return false;
            }
            options.profile = *profile;
            options.have_profile = true;
        } else if (argument == "--alignment") {
            const auto text = value();
            const auto alignment = text ? ParseAlignment(*text) : std::nullopt;
            if (!alignment) {
                std::cerr << "--alignment 仅支持 8、16 或 32\n";
                return false;
            }
            options.alignment = *alignment;
            options.have_alignment = true;
        } else if (argument == "--write-calibration") {
            options.write_calibration = true;
        } else if (argument == "--calibration-symbol") {
            const auto text = value();
            if (!text || text->empty()) {
                std::cerr << "--calibration-symbol 缺少参数\n";
                return false;
            }
            options.calibration_symbol = *text;
        } else if (argument == "--calibration-value") {
            const auto text = value();
            const auto parsed = text ? ParseFiniteDouble(*text) : std::nullopt;
            if (!parsed) {
                std::cerr << "--calibration-value 必须为有限数值\n";
                return false;
            }
            options.calibration_value = *parsed;
            options.have_calibration_value = true;
        } else {
            std::cerr << "未知参数：" << argument << '\n';
            return false;
        }
    }

    if (options.mode == Mode::External) {
        if (options.a2l_file.empty() || !options.have_profile ||
            !options.have_alignment || !options.have_measurement ||
            !options.have_host || !options.have_port ||
            options.have_run_directory) {
            std::cerr << "外部模式必须提供 "
                         "--host、--port、--a2l、--tcp-profile、--alignment 和 "
                         "--measurement，且不能传 --run-dir\n";
            return false;
        }
    } else if (!options.a2l_file.empty() || options.have_profile ||
               options.have_alignment || options.have_measurement ||
               options.have_host) {
        std::cerr << "外部端点/A2L/Profile 参数不能与自动模式混用；请指定 "
                     "--mode external\n";
        return false;
    }
    if (options.write_calibration && !options.have_calibration_value) {
        std::cerr << "启用标定写入时必须同时提供 --calibration-value\n";
        return false;
    }
    if (!options.write_calibration && (options.have_calibration_value ||
                                       !options.calibration_symbol.empty())) {
        std::cerr << "标定参数仅在显式提供 --write-calibration 时有效\n";
        return false;
    }
    if (options.mode == Mode::External && options.write_calibration &&
        options.calibration_symbol.empty()) {
        std::cerr << "外部标定必须提供 --calibration-symbol\n";
        return false;
    }
    return true;
}

/// @brief 以 UTF-8 可读形式渲染数值型 A2L 物理值。
std::optional<double> NumericValue(const a2l::PhysicalValue& value) {
    if (const auto* number = std::get_if<double>(&value)) return *number;
    if (const auto* number = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*number);
    if (const auto* number = std::get_if<std::uint64_t>(&value))
        return static_cast<double>(*number);
    return std::nullopt;
}

/// @brief 输出 A2L 数值物理值。
void PrintValue(std::string_view label, const a2l::PhysicalValue& value) {
    const auto number = NumericValue(value);
    if (number)
        std::cout << label << *number;
    else if (const auto* text = std::get_if<std::string>(&value))
        std::cout << label << *text;
    else if (const auto* boolean = std::get_if<bool>(&value))
        std::cout << label << (*boolean ? "true" : "false");
}

/// @brief 判断 A2L 数据类型是否为可读写的数值标量。
bool IsNumericType(a2l::AsamDataType type) {
    switch (type) {
        case a2l::AsamDataType::UByte:
        case a2l::AsamDataType::SByte:
        case a2l::AsamDataType::UWord:
        case a2l::AsamDataType::SWord:
        case a2l::AsamDataType::ULong:
        case a2l::AsamDataType::SLong:
        case a2l::AsamDataType::ULong64:
        case a2l::AsamDataType::SLong64:
        case a2l::AsamDataType::Float16:
        case a2l::AsamDataType::Float32:
        case a2l::AsamDataType::Float64:
            return true;
        default:
            return false;
    }
}

/// @brief 拒绝不能由 XCP 32-bit 地址 API 表示的 A2L 地址。
void ValidateAddress(const a2l::SymbolInfo& symbol, std::string_view name) {
    if (symbol.xcp_address > std::numeric_limits<xcp::Address>::max()) {
        throw std::runtime_error("A2L 地址超出 XCP Address 宽度，拒绝访问：" +
                                 std::string(name));
    }
}

/// @brief 按 A2L 元数据读取标量并换算物理值。
a2l::PhysicalValue ReadPhysical(xcp::XcpMaster& master,
                                const a2l::IA2lDatabase& database,
                                std::string_view name,
                                a2l::SymbolKind required_kind) {
    auto symbol_result = database.Find(name);
    if (!symbol_result.HasValue()) {
        throw std::runtime_error(
            "A2L 找不到符号 " + std::string(name) + "：" +
            std::string(a2l::ToString(symbol_result.ErrorInfo().code)));
    }
    const auto& symbol = symbol_result.Value();
    if (symbol.kind != required_kind || !symbol.dimensions.empty() ||
        !IsNumericType(symbol.data_type) || symbol.element_size_bytes == 0) {
        throw std::runtime_error("符号必须是 A2L 中可直接读写的数值标量：" +
                                 std::string(name));
    }
    ValidateAddress(symbol, name);
    const auto raw = master.ReadMemoryBytes(
        static_cast<xcp::Address>(symbol.xcp_address),
        static_cast<xcp::AddressExtension>(symbol.address_extension),
        static_cast<xcp::ByteCount>(symbol.element_size_bytes));
    if (raw.size() != symbol.element_size_bytes) {
        throw std::runtime_error("A2L 符号读取长度与元素宽度不一致：" +
                                 std::string(name));
    }
    auto physical = database.ToPhysical(name, raw);
    if (!physical.HasValue()) {
        throw std::runtime_error(
            "A2L ToPhysical 失败：" +
            std::string(a2l::ToString(physical.ErrorInfo().code)));
    }
    return physical.Value();
}

/// @brief 为自动模式准备运行目录并用 Slave 工具生成 A2L 与 AML。
fs::path GenerateDemoA2l(const Options& options) {
    std::error_code error;
    const fs::path directory = fs::absolute(options.run_directory, error);
    if (error)
        throw std::runtime_error("无法解析自动模式运行目录：" +
                                 error.message());
    fs::create_directories(directory, error);
    if (error)
        throw std::runtime_error("无法创建自动模式运行目录：" +
                                 error.message());

    xcp_example::DemoProcess generator;
    const std::vector<std::string> arguments{"--generate-a2l",
                                             "--transport",
                                             "tcp",
                                             "--bind",
                                             options.host,
                                             "--port",
                                             std::to_string(options.port),
                                             "--output-dir",
                                             directory.string(),
                                             "--overwrite"};
    std::string error_message;
    if (!generator.Start(XCP_SLAVE_DEMO_EXECUTABLE, directory, arguments,
                         &error_message)) {
        throw std::runtime_error("启动 A2L 生成器失败：" + error_message);
    }
    int generator_exit_code = -1;
    if (!generator.WaitForExit(std::chrono::seconds(30),
                               &generator_exit_code)) {
        generator.Stop();
        throw std::runtime_error("示例 Slave A2L 生成器超时");
    }
    if (generator_exit_code != 0) {
        throw std::runtime_error("示例 Slave A2L 生成器异常退出，退出码=" +
                                 std::to_string(generator_exit_code));
    }
    const fs::path a2l_path = directory / "xcp_slave_demo.a2l";
    const fs::path aml_path = directory / "XCP_104.aml";
    if (!fs::is_regular_file(a2l_path) || !fs::is_regular_file(aml_path)) {
        throw std::runtime_error("示例 Slave 未生成 A2L/AML；请检查子进程输出");
    }
    return a2l_path;
}

/// @brief 使用显式 profile/alignment 创建 TCP Master，并有界重试初次连接。
std::unique_ptr<xcp::XcpMaster> Connect(const Options& options) {
    std::string last_error;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        xcp::TcpTransportConfig config;
        config.remote_host = options.host;
        config.remote_port = options.port;
        config.wire_profile = options.profile;
        config.packet_alignment = options.alignment;
        config.connect_timeout_ms = 1000;
        auto master = std::make_unique<xcp::XcpMaster>(
            std::make_unique<xcp::TcpTransport>(config));
        try {
            master->Connect();
            return master;
        } catch (const std::exception& error) {
            last_error = error.what();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("连接 TCP Slave 超时：" + last_error);
}

/// @brief 按 A2L 标量符号写入物理值并回读；写入后绝不自动恢复。
void WriteCalibration(xcp::XcpMaster& master, const a2l::IA2lDatabase& database,
                      std::string_view symbol_name, double target_value) {
    auto symbol_result = database.Find(symbol_name);
    if (!symbol_result.HasValue()) {
        throw std::runtime_error("A2L 找不到标定符号 " +
                                 std::string(symbol_name));
    }
    const auto& symbol = symbol_result.Value();
    if (symbol.kind != a2l::SymbolKind::Characteristic ||
        symbol.characteristic_type != a2l::CharacteristicType::Value ||
        !symbol.dimensions.empty() || !symbol.read_write ||
        !IsNumericType(symbol.data_type) || symbol.element_size_bytes == 0) {
        throw std::runtime_error(
            "拒绝写入：标定目标必须是 A2L READ_WRITE 的直接数值 VALUE 标量：" +
            std::string(symbol_name));
    }
    ValidateAddress(symbol, symbol_name);
    const a2l::PhysicalValue desired{target_value};
    auto encoded = database.FromPhysical(symbol_name, desired);
    if (!encoded.HasValue()) {
        throw std::runtime_error(
            "A2L FromPhysical 拒绝目标值（类型/范围/可逆性）：" +
            std::string(a2l::ToString(encoded.ErrorInfo().code)));
    }
    if (encoded.Value().size() != symbol.element_size_bytes) {
        throw std::runtime_error("A2L 编码长度与标定符号宽度不一致，拒绝写入");
    }

    std::cout << "即将写入并保留目标值：" << symbol_name << " = "
              << target_value << "（不会恢复原值）\n";
    master.WriteMemoryBytes(
        static_cast<xcp::Address>(symbol.xcp_address),
        static_cast<xcp::AddressExtension>(symbol.address_extension),
        encoded.Value());
    try {
        const auto actual = ReadPhysical(master, database, symbol_name,
                                         a2l::SymbolKind::Characteristic);
        const auto actual_number = NumericValue(actual);
        if (!actual_number ||
            std::abs(*actual_number - target_value) >
                std::max(1.0e-9, std::abs(target_value) * 1.0e-9)) {
            throw std::runtime_error("回读值与目标物理值不一致");
        }
        PrintValue("标定回读值：", actual);
        std::cout << "（目标值保留）\n";
    } catch (const std::exception& error) {
        std::cerr << "警告：写命令已成功发送，但回读失败；目标值可能已改变，且"
                     "不会自动恢复。原因："
                  << error.what() << '\n';
        throw;
    }
}

/// @brief 执行自动或外部模式的完整会话。
int Run(const Options& options) {
    xcp_example::DemoProcess slave;
    fs::path a2l_path = options.a2l_file;
    xcp::TcpWireProfile profile = options.profile;
    xcp::PacketAlignment alignment = options.alignment;

    if (options.mode == Mode::Automatic) {
        profile = xcp::TcpWireProfile::AsamV14;
        alignment = xcp::PacketAlignment::Bit8;
        a2l_path = GenerateDemoA2l(options);
        const std::vector<std::string> arguments{
            "--transport", "tcp",    "--bind",
            options.host,  "--port", std::to_string(options.port)};
        std::string error_message;
        if (!slave.Start(XCP_SLAVE_DEMO_EXECUTABLE,
                         fs::absolute(options.run_directory), arguments,
                         &error_message)) {
            throw std::runtime_error("启动示例 TCP Slave 失败：" +
                                     error_message);
        }
        std::cout << "已启动内置 TCP Slave：" << options.host << ':'
                  << options.port << '\n';
    }

    a2l::LoadOptions load_options;
    auto loaded = a2l::A2lBridge::Load(a2l_path.string(), load_options);
    if (!loaded.HasValue()) {
        throw std::runtime_error(
            "加载 A2L 失败：" +
            std::string(a2l::ToString(loaded.ErrorInfo().code)));
    }
    const a2l::IA2lDatabase* database = loaded.Value()->Database();
    if (database == nullptr) throw std::runtime_error("A2L 数据库尚未就绪");

    Options connection_options = options;
    connection_options.profile = profile;
    connection_options.alignment = alignment;
    auto master = Connect(connection_options);
    try {
        const auto measurement =
            ReadPhysical(*master, *database, options.measurement,
                         a2l::SymbolKind::Measurement);
        PrintValue("测量物理值：", measurement);
        std::cout << "（" << options.measurement << "）\n";
        if (options.write_calibration) {
            const std::string symbol = options.calibration_symbol.empty()
                                           ? "xcp_slave_demo::cal.basic.real_32"
                                           : options.calibration_symbol;
            WriteCalibration(*master, *database, symbol,
                             options.calibration_value);
        }
        master->Disconnect();
    } catch (...) {
        try {
            master->Disconnect();
        } catch (...) {
        }
        throw;
    }
    return 0;
}

}  // namespace

/// @brief 程序入口：参数错误或任一 XCP/A2L 操作失败均返回非零状态。
int main(int argc, char** argv) {
    if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                      std::string_view(argv[1]) == "-h")) {
        PrintUsage();
        return 0;
    }
    Options options;
    if (!ParseArgs(argc, argv, options)) return 2;
    try {
        return Run(options);
    } catch (const std::exception& error) {
        std::cerr << "xcp_master_tcp 失败：" << error.what() << '\n';
        return 1;
    }
}
