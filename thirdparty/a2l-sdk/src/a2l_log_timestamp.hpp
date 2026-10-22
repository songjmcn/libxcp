#pragma once

#include <boost/locale/formatting.hpp>
#include <boost/locale/generator.hpp>

#include <ctime>
#include <locale>
#include <sstream>
#include <string>

namespace a2l_sdk::detail {

/**
 * @brief 将 POSIX 时间戳按当前系统时区格式化为时分秒。
 * @param timestamp POSIX 时间戳。
 * @return 本地时区的 24 小时时间字符串，格式为 HH:MM:SS。
 */
[[nodiscard]] inline std::string FormatLocalLogTimestamp(
    std::time_t timestamp) {
    static const std::locale log_locale = [] {
        boost::locale::generator locale_generator;
        return locale_generator("");
    }();

    std::ostringstream time_stream;
    time_stream.imbue(log_locale);
    time_stream << boost::locale::as::local_time
                << boost::locale::as::ftime("%H:%M:%S") << timestamp;
    return time_stream.str();
}

}  // namespace a2l_sdk::detail
