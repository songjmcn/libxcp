/*
 * Copyright 2026 Ingemar Hedvall
 * SPDX-License-Identifier: MIT
 */

#include "a2l/a2llogstream.h"

#include "a2l_log_timestamp.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

/** @brief 收集由 A2L 库注册的日志回调。 */
std::vector<a2l::A2lLogFunction> kLogFunctionList;

/** @brief A2L 日志严重级别名称。 */
constexpr std::array<std::string_view, 9> kSeverityList = {
    "Trace", "Debug",    "Info",  "Notice",   "Warning",
    "Error", "Critical", "Alert", "Emergency"};

/** @brief 当前控制台日志过滤级别。 */
a2l::A2lLogSeverity kLogLevel = a2l::A2lLogSeverity::kTrace;

/** @brief 是否在控制台日志中显示源代码位置。 */
bool kShowLocation = true;

}  // namespace

namespace a2l {

A2lLogStream::A2lLogStream(std::source_location location,
                           A2lLogSeverity severity)
    : location_(location), severity_(severity) {
    imbue(std::locale::classic());
}

A2lLogStream::~A2lLogStream() {
    A2lLogStream::LogString(location_, severity_, str());
}

void A2lLogStream::LogString(const std::source_location& location,
                             A2lLogSeverity severity, const std::string& text) {
    for (const auto& func : kLogFunctionList) {
        if (func) {
            func(location, severity, text);
        }
    }
}

void A2lLogStream::SetLogFunction(const A2lLogFunction& func) {
    kLogFunctionList.emplace_back(func);
}

void A2lLogStream::ResetLogFunction() { kLogFunctionList.clear(); }

void A2lLogStream::LogToConsole(const std::source_location& location,
                                A2lLogSeverity severity,
                                const std::string& text) {
    if (severity < kLogLevel) {
        return;
    }

    try {
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::cout << a2l_sdk::detail::FormatLocalLogTimestamp(now) << " ";
        const auto severity_index = static_cast<std::size_t>(severity);
        if (severity_index < kSeverityList.size()) {
            std::cout << "[" << kSeverityList[severity_index] << "] ";
        }

        std::cout << text;
        if (kShowLocation) {
            std::cout << " (" << location.file_name() << ":"
                      << location.function_name() << " C:" << location.column()
                      << " L:" << location.line() << ")";
        }
        std::cout << std::endl;
    } catch (const std::exception&) {
    }
}

void A2lLogStream::SetLogLevel(A2lLogSeverity severity) {
    kLogLevel = severity;
}

void A2lLogStream::ShowLocation(bool show_location) {
    kShowLocation = show_location;
}

}  // namespace a2l
