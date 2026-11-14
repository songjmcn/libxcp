#pragma once

/**
 * @file include_resolver.hpp
 * @brief A2L `/include` 目标文件的读取回调（供原文核对阶段使用）
 *
 * @details `ScanA2lTcpCandidate()` 需要在"端点声明唯一"的核对中看一眼被
 *          include 进来的伴生文件（典型是 `.aml`），以证明它没有另外引入
 *          `XCP_ON_TCP_IP` / `XCP_ON_UDP_IP`。扫描器自己不碰文件系统，读取
 *          动作由本文件提供的回调完成，保证核心库不依赖具体部署形态。
 */

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "libxcp/a2l_tcp_alignment.hpp"

namespace calmcar::xcp::tools {

/// @brief 默认的单个 include 文件大小上限（1 MiB）
inline constexpr std::size_t kIncludeMaxBytes = 1024U * 1024U;

/**
 * @brief 单次 include 读取的选项
 */
struct IncludeReadOptions {
    /// @brief 相对路径的基准目录；通常是用户提供的 A2L 所在目录
    std::filesystem::path base_dir;

    /// @brief 单个文件允许的最大字节数
    std::size_t max_bytes{kIncludeMaxBytes};
};

/**
 * @brief 读取一个 `/include` 目标
 *
 * @param options 基准目录与大小上限
 * @param include_path include token 里的原文；相对路径按 `base_dir` 解析
 * @return 文件内容；无法读取或超限时返回空
 *
 * @details 只做三件事：拼路径、查普通文件、限长读全。
 *          - 不做 `canonical()`：符号链接/短名都交给操作系统解析，避免在
 *            Windows 上把长路径规范化成带 `\\?\` 前缀的形式而影响日志可读性；
 *          - 不递归：扫描器对 include 内容是"看一眼"，不会二次下降，因此
 *            不需要深度限制与循环检测（递归由 `ScanA2lTcpCandidate` 之外的
 *            上游解析器负责）；
 *          - 超限直接判为不可读（返回空），而不是截断——被截断的文件可能
 *            丢掉端点块，从而把"看不见"误判成"没有"。
 */
[[nodiscard]] inline std::optional<std::string> ReadIncludeFile(
    const IncludeReadOptions& options, std::string_view include_path) {
    if (include_path.empty()) return std::nullopt;

    std::error_code ec;
    std::filesystem::path target{std::string(include_path)};
    if (target.is_relative()) target = options.base_dir / target;

    if (!std::filesystem::is_regular_file(target, ec) || ec) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(target, ec);
    if (ec || size > options.max_bytes) {
        return std::nullopt;
    }

    std::ifstream in(target, std::ios::binary);
    if (!in) return std::nullopt;
    std::string content(static_cast<std::size_t>(size), '\0');
    if (!content.empty()) {
        in.read(content.data(), static_cast<std::streamsize>(content.size()));
        if (!in) return std::nullopt;
    }
    return content;
}

/**
 * @brief 构造可直接交给 `ScanA2lTcpCandidate()` 的解析回调
 *
 * @param options 基准目录与大小上限
 * @return `/include` 解析回调；内部捕获 `options` 的拷贝，调用方无需保活
 */
[[nodiscard]] inline IncludeResolver MakeIncludeResolver(
    IncludeReadOptions options) {
    return [options = std::move(options)](const std::string& path)
               -> std::optional<std::string> {
        return ReadIncludeFile(options, path);
    };
}

}  // namespace calmcar::xcp::tools
