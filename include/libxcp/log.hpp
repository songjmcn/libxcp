#pragma once

/**
 * @file log.hpp
 * @brief 为 libxcp 提供可替换后端的三级日志宏。
 */

#ifndef LIBXCP_ENABLE_LOG
#define LIBXCP_ENABLE_LOG 0
#endif

#if LIBXCP_ENABLE_LOG

#include <format>
#include <string>

#include <glog/logging.h>

namespace calmcar::xcp::detail {

/**
 * @brief 将已格式化消息写入当前日志后端。
 * @param file 调用日志宏的源文件。
 * @param line 调用日志宏的源代码行号。
 * @param severity 后端日志级别。
 * @param message 已完成格式化的消息。
 */
inline void WriteLogMessage(const char* file, int line,
                            google::LogSeverity severity,
                            const std::string& message) {
    google::LogMessage(file, line, severity).stream() << message;
}

}  // namespace calmcar::xcp::detail

/**
 * @brief 记录 INFO 级别日志；格式参数使用 C++20 std::format 语法。
 * @param ... 格式串及其对应的格式化参数。
 */
#define LIBXCP_LOG_INFO(...)                                                   \
    do {                                                                       \
        try {                                                                  \
            ::calmcar::xcp::detail::WriteLogMessage(__FILE__, __LINE__,        \
                                                    google::GLOG_INFO,         \
                                                    std::format(__VA_ARGS__)); \
        } catch (...) {                                                        \
        }                                                                      \
    } while (false)

/**
 * @brief 记录 WARNING 级别日志；格式参数使用 C++20 std::format 语法。
 * @param ... 格式串及其对应的格式化参数。
 */
#define LIBXCP_LOG_WARNING(...)                                                \
    do {                                                                       \
        try {                                                                  \
            ::calmcar::xcp::detail::WriteLogMessage(__FILE__, __LINE__,        \
                                                    google::GLOG_WARNING,      \
                                                    std::format(__VA_ARGS__)); \
        } catch (...) {                                                        \
        }                                                                      \
    } while (false)

/**
 * @brief 记录 ERROR 级别日志；该级别不会主动终止进程。
 * @param ... 格式串及其对应的格式化参数。
 */
#define LIBXCP_LOG_ERROR(...)                                                  \
    do {                                                                       \
        try {                                                                  \
            ::calmcar::xcp::detail::WriteLogMessage(__FILE__, __LINE__,        \
                                                    google::GLOG_ERROR,        \
                                                    std::format(__VA_ARGS__)); \
        } catch (...) {                                                        \
        }                                                                      \
    } while (false)

#else

/** @brief 日志关闭时不求值参数的 INFO 日志空操作宏。 */
#define LIBXCP_LOG_INFO(...) \
    do {                     \
    } while (false)
/** @brief 日志关闭时不求值参数的 WARNING 日志空操作宏。 */
#define LIBXCP_LOG_WARNING(...) \
    do {                        \
    } while (false)
/** @brief 日志关闭时不求值参数的 ERROR 日志空操作宏。 */
#define LIBXCP_LOG_ERROR(...) \
    do {                      \
    } while (false)

#endif
