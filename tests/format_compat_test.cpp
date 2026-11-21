/**
 * @file format_compat_test.cpp
 * @brief 校验 std::format / fmt 兼容层的输出在两个后端下逐字节一致。
 *
 * 覆盖本项目实际用到的全部占位符形态：`{}`、`{:06d}`、`{:.1f}`、`{:.2f}`、`{:.0f}`，
 * 以及 `{{` / `}}` 转义与"参数多于占位符"的宽容行为。
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "libxcp/detail/format_compat.hpp"

namespace calmcar::xcp {
namespace {

using detail::FormatText;

/** @brief 常用形态：无格式说明符的占位符。 */
TEST(FormatCompat, FormatsPlainPlaceholders) {
    EXPECT_EQ(FormatText("value={}", 42), "value=42");
    EXPECT_EQ(FormatText("name={}", "abc"), "name=abc");
    EXPECT_EQ(FormatText("name={}", std::string("abc")), "name=abc");
    EXPECT_EQ(FormatText("name={}", std::string_view("abc")), "name=abc");
    EXPECT_EQ(FormatText("{}-{}", 1, 2), "1-2");
}

/** @brief 整数零填充：基准测试用它生成 signal_NNNNNN 形式的候选标签。 */
TEST(FormatCompat, FormatsZeroPaddedInteger) {
    EXPECT_EQ(FormatText("{:06d}", 42), "000042");
    EXPECT_EQ(FormatText("{:06d}", 1234567), "1234567");
}

/** @brief 定点小数：百分比与耗时日志都依赖它。 */
TEST(FormatCompat, FormatsFixedPrecision) {
    EXPECT_EQ(FormatText("{:.1f}", 0.5), "0.5");
    EXPECT_EQ(FormatText("{:.1f}", 3.14159), "3.1");
    EXPECT_EQ(FormatText("{:.2f}", 3.14159), "3.14");
    EXPECT_EQ(FormatText("{:.0f}", 170.0), "170");
}

/** @brief 混合形态：与日志宏的真实调用样式保持一致。 */
TEST(FormatCompat, FormatsMixedPlaceholders) {
    EXPECT_EQ(
        FormatText("耗时 {:.1f} ms（{} 个候选 / {:06d}）", 12.5, 7, 42),
        "耗时 12.5 ms（7 个候选 / 000042）");
}

/** @brief 花括号转义。 */
TEST(FormatCompat, EscapesBraces) {
    EXPECT_EQ(FormatText("{{}}"), "{}");
    EXPECT_EQ(FormatText("a{{b}}c"), "a{b}c");
}

/** @brief 参数多于占位符时多余参数被忽略，不抛异常。 */
TEST(FormatCompat, IgnoresExtraArguments) {
    EXPECT_EQ(FormatText("only {}", 1, 2), "only 1");
}

/** @brief 基准测试的候选标签生成逻辑（MakePreviewLabel 的真实形态）。 */
TEST(FormatCompat, BuildsBenchmarkPreviewLabel) {
    const std::string label = FormatText(
        "apa_complex::algo_status_info.apa_control_status.signal_{:06d}"
        " = 12345.6 mV | POLL | 2026-10-23 20:08:00",
        std::size_t{5});
    EXPECT_EQ(label,
              "apa_complex::algo_status_info.apa_control_status.signal_000005"
              " = 12345.6 mV | POLL | 2026-10-23 20:08:00");
}

/** @brief 基准测试的跳过原因文案（能力系数与耗时同时出现）。 */
TEST(FormatCompat, BuildsBenchmarkSkipReason) {
    const std::string reason = FormatText(
        "本机算力与参考机不匹配：实测单帧 {:.1f} ms，参考预算 {:.0f} ms，"
        "能力系数 {:.2f}",
        3.5, 510.0, 2.5);
    EXPECT_EQ(reason, "本机算力与参考机不匹配：实测单帧 3.5 ms，参考预算 510 ms，"
                      "能力系数 2.50");
}

}  // namespace
}  // namespace calmcar::xcp
