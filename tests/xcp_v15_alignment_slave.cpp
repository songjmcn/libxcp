/**
 * @file xcp_v15_alignment_slave.cpp
 * @brief V1.5 对齐矩阵验收用的最小测试从站可执行文件。
 *
 * @par 这是什么
 *   把 `tests/tcp_test_slave.*` 中**独立于生产 decoder** 的 ASAM V1.5 framing
 *   实现包装成一个可被父进程启动的独立进程，用于验证本库 Master 在
 *   PACKET_ALIGNMENT = 8 / 16 / 32 下的真实线路行为。
 *
 * @par 它不是什么（必须如实标注）
 *   - 不是生产从站，不对外提供任何 SDK 能力；
 *   - 不做 ASAM V1.5 一致性认证，也不代表第三方实现；
 *   - 只绑定 127.0.0.1，仅用于本机回环验收。
 *   V1.5 的 LEN 外 Tail 语义在仓库内没有第三方实现可对照，因此本进程参与的
 *   用例结论只能表述为"本库 Master 与独立于此实现的测试从站在 N 位对齐下
 *   互通"，**不得**表述为"已与第三方 V1.5 实现互操作"。
 *
 * @par 为什么就绪信息写文件而不是打印到 stdout 管道
 *   本仓库的测试运行环境（DSH 沙箱）禁止子进程通过命名管道回传输出，
 *   父进程若用管道捕获 stdout 会直接失败。因此从站把实际端口写入
 *   `--ready-file` 指定的文件，父进程轮询该文件判断就绪。
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "tcp_test_slave.hpp"

namespace {

/// @brief 从站模拟内存基地址，父进程用同一常量构造读取请求。
constexpr std::uint16_t kMemoryBase = 0x1000;

/// @brief 从站模拟内存长度（字节）。
constexpr std::size_t kMemorySize = 64;

/// @brief 默认模拟内存填充值。
constexpr std::uint8_t kDefaultContentByte = 0x5A;

/// @brief 启动参数。
struct Options {
    /// @brief 绑定端口，0 表示由操作系统分配。
    std::uint16_t port{0};

    /// @brief 宣告并使用的 PACKET_ALIGNMENT 位宽（8/16/32）。
    std::uint8_t alignment{8};

    /// @brief 就绪文件路径（写入实际端口）。
    std::string ready_file;

    /// @brief 运行时长上限（毫秒），到时自动退出，避免残留进程。
    std::uint32_t max_runtime_ms{60000};

    /// @brief 模拟内存填充值。
    std::uint8_t content_byte{kDefaultContentByte};

    /// @brief 把第 N 个响应的 LEN 写大（0 = 不注入）。
    /// @details 复用 `TcpFaultInjection::corrupt_len_n`：Master 会认为后继字节
    ///          不足而失步，用于验证它确实按 LEN 校验帧边界。之所以不用"事后
    ///          追加一段畸形帧"，是因为那取决于竞态——畸形帧可能与正常响应
    ///          交叉到达，导致用例时过时不过（首次实现即踩到该坑）。
    std::size_t corrupt_len_at{0};

    /// @brief 可选日志文件路径，记录启动、连接与退出事件。
    std::string log_file;
};

/// @brief 打印用法。
void PrintUsage() {
    std::cerr << "用法: xcp_v15_alignment_slave --alignment <8|16|32>"
                 " --ready-file <path> [--port <n>] [--max-runtime-ms <n>]"
                 " [--content-byte <0..255>] [--corrupt-len-at <n>]"
                 " [--log-file <path>]\n";
}

/**
 * @brief 解析单个无符号整数参数。
 * @param text 参数文本
 * @param max_value 允许的最大值
 * @return 解析成功时的值，失败返回 std::nullopt
 */
std::optional<std::uint32_t> ParseUnsigned(const char* text,
                                           std::uint32_t max_value) {
    if (text == nullptr || *text == '\0') {
        return std::nullopt;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text || (end != nullptr && *end != '\0')) {
        return std::nullopt;
    }
    if (value > max_value) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

/**
 * @brief 向日志文件追加一行（未指定日志文件时静默忽略）。
 * @param path 日志文件路径
 * @param line 日志内容
 */
void AppendLog(const std::string& path, const std::string& line) {
    if (path.empty()) {
        return;
    }
    std::ofstream out(path, std::ios::app);
    if (out.is_open()) {
        out << line << '\n';
    }
}

/**
 * @brief 把就绪信息原子地写入就绪文件。
 * @param path 目标文件路径
 * @param content 文件内容
 * @return 写入成功返回 true
 * @details 先写临时文件再改名，避免父进程读到写了一半的内容。
 */
bool WriteReadyFile(const std::string& path, const std::string& content) {
    const std::filesystem::path target(path);
    const std::filesystem::path temporary = target.string() + ".part";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << content;
        out.flush();
        if (!out.good()) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(temporary, target, error);
    return !error;
}

/**
 * @brief 解析命令行参数。
 * @param argc 参数个数
 * @param argv 参数数组
 * @param options 输出参数
 * @return 解析成功返回 true
 */
bool ParseArguments(int argc, char** argv, Options* options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool has_value = (index + 1) < argc;
        if (argument == "--alignment") {
            if (!has_value) {
                return false;
            }
            const auto value = ParseUnsigned(argv[++index], 32);
            if (!value.has_value() ||
                (*value != 8 && *value != 16 && *value != 32)) {
                return false;
            }
            options->alignment = static_cast<std::uint8_t>(*value);
        } else if (argument == "--port") {
            if (!has_value) {
                return false;
            }
            const auto value = ParseUnsigned(argv[++index], 65535);
            if (!value.has_value()) {
                return false;
            }
            options->port = static_cast<std::uint16_t>(*value);
        } else if (argument == "--ready-file") {
            if (!has_value) {
                return false;
            }
            options->ready_file = argv[++index];
        } else if (argument == "--max-runtime-ms") {
            if (!has_value) {
                return false;
            }
            const auto value = ParseUnsigned(argv[++index], 600000);
            if (!value.has_value()) {
                return false;
            }
            options->max_runtime_ms = *value;
        } else if (argument == "--content-byte") {
            if (!has_value) {
                return false;
            }
            const auto value = ParseUnsigned(argv[++index], 255);
            if (!value.has_value()) {
                return false;
            }
            options->content_byte = static_cast<std::uint8_t>(*value);
        } else if (argument == "--corrupt-len-at") {
            if (!has_value) {
                return false;
            }
            const auto value = ParseUnsigned(argv[++index], 1000);
            if (!value.has_value() || *value == 0) {
                return false;
            }
            options->corrupt_len_at = *value;
        } else if (argument == "--log-file") {
            if (!has_value) {
                return false;
            }
            options->log_file = argv[++index];
        } else {
            return false;
        }
    }
    return !options->ready_file.empty();
}

/**
 * @brief 把 8/16/32 位宽转换为库内枚举。
 * @param bits 位宽
 * @return 对应的对齐枚举；非法值返回 Unknown
 */
calmcar::xcp::PacketAlignment ToAlignment(std::uint8_t bits) {
    switch (bits) {
        case 8:
            return calmcar::xcp::PacketAlignment::Bit8;
        case 16:
            return calmcar::xcp::PacketAlignment::Bit16;
        case 32:
            return calmcar::xcp::PacketAlignment::Bit32;
        default:
            return calmcar::xcp::PacketAlignment::Unknown;
    }
}

}  // namespace

/**
 * @brief 程序入口：启动最小 V1.5 测试从站并等待父进程回收。
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 0 正常退出；2 参数错误；3 启动失败
 */
int main(int argc, char** argv) {
    Options options;
    if (!ParseArguments(argc, argv, &options)) {
        PrintUsage();
        return 2;
    }

    const auto alignment = ToAlignment(options.alignment);
    if (alignment == calmcar::xcp::PacketAlignment::Unknown) {
        std::cerr << "非法的 --alignment：" << static_cast<int>(options.alignment)
                  << '\n';
        return 2;
    }

    try {
        calmcar::xcp::test::TcpTestSlave slave(calmcar::xcp::TcpWireProfile::AsamV15,
                                             options.port);
        // 对齐必须在 Start() 之前设定：它决定 LEN 之外 Tail 的填充字节数，
        // 进入收发循环后再改会让已发出的帧与后续帧语义不一致。
        slave.SetPacketAlignment(alignment);

        std::vector<std::uint8_t> memory(kMemorySize, options.content_byte);
        slave.SetMemory(static_cast<calmcar::xcp::Address>(kMemoryBase), memory);

        // 负向用例用：把第 N 个响应的 LEN 故意写大，验证 Master 是否真的按
        // LEN 校验帧边界（而不是"读到什么算什么"）。
        if (options.corrupt_len_at != 0) {
            calmcar::xcp::test::TcpFaultInjection fault;
            fault.corrupt_len_n = options.corrupt_len_at;
            slave.SetFaultInjection(fault);
        }

        slave.Start();
        AppendLog(options.log_file,
                  "started port=" + std::to_string(slave.Port()) + " alignment=" +
                      std::to_string(static_cast<int>(options.alignment)));

        const std::string ready = "PORT=" + std::to_string(slave.Port()) + "\n" +
                                  "ALIGNMENT=" +
                                  std::to_string(static_cast<int>(options.alignment)) +
                                  "\n";
        if (!WriteReadyFile(options.ready_file, ready)) {
            std::cerr << "写入就绪文件失败：" << options.ready_file << '\n';
            slave.Stop();
            return 3;
        }

        // 到点自动退出：父进程被异常终止时也不会留下常驻进程。
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(options.max_runtime_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        AppendLog(options.log_file, "commands=" +
                                        std::to_string(slave.CommandCount()) +
                                        " connected=" +
                                        (slave.IsConnected() ? "1" : "0"));
        slave.Stop();
        AppendLog(options.log_file, "stopped");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "从站启动失败：" << error.what() << '\n';
        AppendLog(options.log_file, std::string("failed: ") + error.what());
        return 3;
    }
}