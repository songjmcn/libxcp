#include "a2l_log_timestamp.hpp"

#include <a2l/a2llogstream.h>
#include <boost/locale/time_zone.hpp>
#include <gtest/gtest.h>

#include <ctime>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>

namespace {

/**
 * @brief 在测试期间设置 Boost.Locale 时区，并在析构时恢复原设置。
 */
class ScopedTimeZone final {
public:
    /**
     * @brief 设置临时时区并保存原有设置。
     * @param time_zone 新时区标识。
     */
    explicit ScopedTimeZone(const std::string& time_zone)
        : previous_time_zone_(boost::locale::time_zone::global(time_zone)) {}

    /** @brief 恢复测试前的 Boost.Locale 时区设置。 */
    ~ScopedTimeZone() { boost::locale::time_zone::global(previous_time_zone_); }

    ScopedTimeZone(const ScopedTimeZone&) = delete;
    ScopedTimeZone& operator=(const ScopedTimeZone&) = delete;

private:
    /** @brief 测试开始前的时区标识。 */
    const std::string previous_time_zone_;
};

}  // namespace

/**
 * @brief 验证 A2L 日志时间格式化兼容层。
 */
TEST(A2lLogTimestampTest, FormatsTimestampUsingConfiguredLocalTimeZone) {
    const ScopedTimeZone time_zone("UTC");

    EXPECT_EQ(a2l_sdk::detail::FormatLocalLogTimestamp(std::time_t{0}),
              "00:00:00");
}

/**
 * @brief 验证控制台日志保留时间戳、严重级别和消息内容。
 */
TEST(A2lLogTimestampTest, ConsoleLogKeepsTimestampAndMessageFields) {
    const ScopedTimeZone time_zone("UTC");
    std::ostringstream captured_output;
    std::streambuf* const original_output =
        std::cout.rdbuf(captured_output.rdbuf());
    a2l::A2lLogStream::SetLogLevel(a2l::A2lLogSeverity::kTrace);
    a2l::A2lLogStream::ShowLocation(false);
    a2l::A2lLogStream::LogToConsole(std::source_location::current(),
                                    a2l::A2lLogSeverity::kInfo,
                                    "compatibility");
    std::cout.rdbuf(original_output);
    a2l::A2lLogStream::ShowLocation(true);

    const std::string output = captured_output.str();
    ASSERT_GE(output.size(), 8U);
    EXPECT_TRUE(std::regex_match(output.substr(0, 8),
                                 std::regex(R"(\d{2}:\d{2}:\d{2})")));
    EXPECT_NE(output.find("[Info] compatibility"), std::string::npos);
}
