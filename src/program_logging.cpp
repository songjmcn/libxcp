/**
 * @file program_logging.cpp
 * @brief 项目自有可执行程序的 glog 文件输出初始化与关闭。
 */

#include "program_logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <glog/logging.h>

namespace calmcar::xcp::internal {
namespace {

/// @brief 将目录分量限制为不含路径分隔符的安全名称。
std::string SafeComponent(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') ||
                           character == '_' || character == '-' ||
                           character == '.';
        result.push_back(valid ? static_cast<char>(character) : '_');
    }
    if (result.empty() || result == "." || result == "..") {
        result = "application";
    }
    return result;
}

/// @brief 验证日志目录可创建并写入文件，然后删除检查项。
bool IsWritableDirectory(const std::filesystem::path& directory,
                         std::string& error_message) {
    static std::atomic<std::uint64_t> next_probe{0};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();

    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto suffix = std::to_string(timestamp) + "-" +
                            std::to_string(next_probe.fetch_add(1));
        const auto probe_directory =
            directory / (".libxcp-write-probe-" + suffix);
        std::error_code error;
        const bool created =
            std::filesystem::create_directory(probe_directory, error);
        if (error) {
            error_message = "无法验证日志目录可写性 '" + directory.string() +
                            "': " + error.message();
            return false;
        }
        if (!created) {
            continue;
        }

        bool file_writable = false;
        {
            std::ofstream probe_file(probe_directory / "write-test.tmp",
                                     std::ios::binary | std::ios::out);
            if (probe_file) {
                probe_file.put('x');
                probe_file.flush();
                file_writable = static_cast<bool>(probe_file);
            }
        }

        std::error_code cleanup_error;
        std::filesystem::remove_all(probe_directory, cleanup_error);
        if (!file_writable) {
            error_message =
                "无法在日志目录创建并写入测试文件 '" + directory.string() + "'";
            if (cleanup_error) {
                error_message += "; 清理检查项失败: " + cleanup_error.message();
            }
            return false;
        }
        if (cleanup_error) {
            error_message = "无法清理日志目录写入检查项 '" +
                            probe_directory.string() +
                            "': " + cleanup_error.message();
            return false;
        }
        return true;
    }

    error_message =
        "无法在日志目录创建唯一的写入检查项 '" + directory.string() + "'";
    return false;
}

}  // namespace

ProgramLogSession::~ProgramLogSession() { Shutdown(); }

bool ProgramLogSession::Initialize(std::string_view executable_path,
                                   std::string_view module,
                                   std::string& error_message) {
    error_message.clear();
    if (owns_glog_) {
        return true;
    }
    if (google::IsGoogleLoggingInitialized()) {
        error_message =
            "glog 已由其他代码初始化，无法保证本程序日志仅写入指定文件目录";
        return false;
    }

    try {
        std::error_code error;
        const auto working_directory = std::filesystem::current_path(error);
        if (error) {
            error_message = "无法确定当前工作目录: " + error.message();
            return false;
        }

        const auto executable_name =
            std::filesystem::path(std::string(executable_path))
                .filename()
                .stem()
                .string();
        const auto program = SafeComponent(executable_name);
        const auto module_name = SafeComponent(module);
        const auto log_directory =
            working_directory / "logs" / module_name / program;

        std::filesystem::create_directories(log_directory, error);
        if (error) {
            error_message = "无法创建日志目录 '" + log_directory.string() +
                            "': " + error.message();
            return false;
        }
        if (!IsWritableDirectory(log_directory, error_message)) {
            return false;
        }

        executable_name_ = executable_path.empty()
                               ? "application"
                               : std::string(executable_path);
        FLAGS_log_dir = log_directory.string();
        FLAGS_logtostderr = false;
        FLAGS_alsologtostderr = false;
        // glog 在 severity >= stderrthreshold 时也会写 stderr；取高于 FATAL
        // 的值，使本项目的 INFO/WARNING/ERROR 日志只进入文件。
        FLAGS_stderrthreshold = google::GLOG_FATAL + 1;
        // glog 保存 argv0 中 basename 的指针，因此必须传入生命周期覆盖
        // 整个 glog 实例的成员字符串，不能传局部 std::string。
        google::InitGoogleLogging(executable_name_.c_str());
        owns_glog_ = google::IsGoogleLoggingInitialized();
        if (!owns_glog_) {
            error_message = "glog 初始化后仍未进入已初始化状态";
            return false;
        }
        // 立即冲刷每条日志：文件是本项目唯一的日志通道（stderrthreshold 已
        // 高于 FATAL），若沿用 glog 默认的 30 秒缓冲，进程被强杀（Windows
        // 上集成测试用 TerminateProcess 停止常驻从站）或崩溃时，缓冲里的
        // 诊断信息会整段丢失，心跳等周期性状态日志也无法实时观察。
        FLAGS_logbufsecs = 0;

        // 显式设置稳定的程序名/级别前缀与 .glog 后缀；同时关闭默认软链接，
        // 避免目录中出现不符合文件名约定的无后缀别名。
        google::SetLogFilenameExtension(".glog");
        const auto configure_destination = [&log_directory, &program](
                                               google::LogSeverity severity,
                                               std::string_view severity_name) {
            const auto base_filename =
                (log_directory /
                 (program + "." + std::string(severity_name) + "."))
                    .string();
            google::SetLogDestination(severity, base_filename.c_str());
            google::SetLogSymlink(severity, "");
        };
        configure_destination(google::GLOG_INFO, "INFO");
        configure_destination(google::GLOG_WARNING, "WARNING");
        configure_destination(google::GLOG_ERROR, "ERROR");
        configure_destination(google::GLOG_FATAL, "FATAL");
        return true;
    } catch (const std::exception& exception) {
        error_message = std::string("初始化文件日志失败: ") + exception.what();
        return false;
    } catch (...) {
        error_message = "初始化文件日志失败: 未知异常";
        return false;
    }
}

void ProgramLogSession::Shutdown() noexcept {
    if (!owns_glog_) {
        return;
    }
    try {
        google::FlushLogFiles(google::GLOG_INFO);
    } catch (...) {
        // 刷新失败不能阻止随后尝试关闭日志。
    }
    try {
        google::ShutdownGoogleLogging();
    } catch (...) {
        // 关闭日志不能覆盖业务退出码或正在传播的异常，也不能回退到终端输出。
    }
    owns_glog_ = false;
}

}  // namespace calmcar::xcp::internal
