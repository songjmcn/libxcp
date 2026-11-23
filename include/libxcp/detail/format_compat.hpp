#ifndef CALMCAR_XCP_DETAIL_FORMAT_COMPAT_HPP_
#define CALMCAR_XCP_DETAIL_FORMAT_COMPAT_HPP_

/**
 * @file format_compat.hpp
 * @brief 统一标准库 std::format 与 fmt 两个后端，供日志宏与基准测试共用。
 *
 * 后端选择优先级（在编译期确定，调用点语法完全一致）：
 * 1. 标准库提供 C++20 std::format（__cpp_lib_format >= 201907L）时使用 std::format；
 * 2. 否则若构建系统定义了 LIBXCP_HAVE_FMT，使用 fmt；
 * 3. 两者都不可用时直接触发 #error，避免静默退化成不可读的输出。
 *
 * 说明：本头文件只探测标准库是否提供 std::format；是否可用 fmt 由构建系统
 * 定义 LIBXCP_HAVE_FMT 告知（该定义由 libxcp::format 目标传播）。
 * 这里刻意不使用 __has_include(<fmt/format.h>) 之类的自探测：那会把
 * "某目标漏链 libxcp::format"从编译期硬错退化成运行时静默换后端。
 */

#include <string>
#include <utility>
#include <version>

#if defined(__cpp_lib_format) && (__cpp_lib_format >= 201907L)
#include <format>
/** @brief 标记当前使用标准库后端。 */
#define LIBXCP_FORMAT_BACKEND_STD 1
#elif defined(LIBXCP_HAVE_FMT)
#include <fmt/format.h>
/** @brief 标记当前使用 fmt 后端。 */
#define LIBXCP_FORMAT_BACKEND_FMT 1
#else
#error "libxcp 格式化后端未就绪：当前编译器没有 std::format，且未定义 LIBXCP_HAVE_FMT。请把目标链接到 libxcp::format（libxcp 核心已 PUBLIC 传播），或在 CMake 中确认配置期已启用 fmt 回退（配置日志应出现“回退到 fmt”）。"
#endif

namespace calmcar::xcp::detail {

/**
 * @brief 按 C++20 格式串生成字符串。
 * @tparam Args 格式参数类型，由推导得出。
 * @param pattern 编译期格式串，支持 std::format 与 fmt 的公共语法。
 * @param args 与占位符对应的参数。
 * @return 格式化后的字符串。
 * @note 两个后端都使用带编译期检查的 format_string，占位符与参数不匹配时编译失败。
 */
#if defined(LIBXCP_FORMAT_BACKEND_STD)
template <class... Args>
[[nodiscard]] inline std::string FormatText(std::format_string<Args...> pattern,
                                            Args&&... args) {
    return std::format(pattern, std::forward<Args>(args)...);
}
#else
template <class... Args>
[[nodiscard]] inline std::string FormatText(fmt::format_string<Args...> pattern,
                                            Args&&... args) {
    return fmt::format(pattern, std::forward<Args>(args)...);
}
#endif

}  // namespace calmcar::xcp::detail

#endif  // CALMCAR_XCP_DETAIL_FORMAT_COMPAT_HPP_
