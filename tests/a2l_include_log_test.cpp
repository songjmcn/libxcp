// =============================================================================
// a2l_include_log_test.cpp —— include 解析失败必须经 A2lLogStream 留下错误日志
//
// 背景：`A2lScanner::FixIncludeFile()` 过去用 `std::ostringstream err` 自行
// 拼接错误文本再抛出，错误只出现在异常消息里，a2llib 自己的日志通道
// （`A2lLogStream` / `A2L_ERROR()`）看不到任何东西。
//
// 现行实现：`.a2l` include 缺失时，报错文本经 `A2lLogStream`（kError）派发，
// 异常消息与日志文本一致——调用方既能在 `A2lFile::LastError()` 里读到原因，
// 也能通过注册的日志回调收集到同一条记录。
//
// 为什么不在桥接层用例里覆盖：`A2lLogStream` 没有从 `liba2l.dll` 导出，桥接层
// 用例注册的回调落在测试进程里的**另一份静态副本**上，永远等不到 DLL 内扫描器
// 的日志。因此本用例直接链接 a2llib 的静态目标 `a2l`（与 DLL 内是同一份源码），
// 在 a2llib 层验证日志行为。
// =============================================================================

#include <gtest/gtest.h>

#include <a2l/a2lfile.h>
#include <a2l/a2llogstream.h>

#include <filesystem>
#include <fstream>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// @brief 含一个缺失 `.a2l` include 的最小 A2L（include 路径由调用方代入）
constexpr std::string_view kA2lTemplate = R"ASAP2(ASAP2_VERSION 1 71
/begin PROJECT inclog ""
/begin MODULE inclog_m ""
/include "%INCLUDE%"
/begin MOD_COMMON ""
  BYTE_ORDER MSB_LAST
  DATA_SIZE 8
  ALIGNMENT_BYTE 1
  ALIGNMENT_WORD 2
  ALIGNMENT_LONG 4
/end MOD_COMMON
/begin COMPU_METHOD CM "identity" IDENTICAL "%6.2" ""
/end COMPU_METHOD
/begin MEASUREMENT SigVolt "signal voltage" UBYTE CM 0 0 0 255
  ECU_ADDRESS 0x1000
/end MEASUREMENT
/end MODULE
/end PROJECT
)ASAP2";

/// @brief 检查目录能否真正建子目录（`temp_directory_path()` 只报路径，不证明可写）
/// @param dir 待检查的目录
/// @return true = 可写；false = 不存在、无权限或无法创建
/// @details 受限环境（沙箱、低完整性级别进程）里 `temp_directory_path()` 会正常
///          返回路径，但随后的 `create_directories` 抛 "Access is denied"。
bool IsWritableRoot(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec) {
        return false;
    }
    const auto probe = dir / "libxcp_write_probe";
    std::filesystem::remove_all(probe, ec);
    ec.clear();
    return std::filesystem::create_directories(probe, ec) && !ec;
}

/// @brief 选择一个真正可写的临时根目录
/// @return 首选系统临时目录，不可写时回退当前工作目录；都不可用返回空路径
std::filesystem::path PickTempRoot() {
    std::error_code ec;
    const auto system_temp = std::filesystem::temp_directory_path(ec);
    if (!ec && IsWritableRoot(system_temp)) {
        return system_temp;
    }
    const auto current = std::filesystem::current_path(ec);
    if (!ec && IsWritableRoot(current)) {
        return current;
    }
    return {};
}

/// @brief 构造测试目录（每个用例独占，避免相互干扰）
/// @param tag 用例标签，用作子目录名
/// @return 已创建的目录；失败返回空路径
std::filesystem::path MakeTestDir(const std::string& tag) {
    const auto base = PickTempRoot();
    if (base.empty()) {
        return {};
    }
    const auto root = base / "libxcp_include_log" / tag;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) {
        return {};
    }
    return root;
}

/// @brief 把文本写入文件
/// @param file 目标文件
/// @param text 文件内容
/// @return true = 写入成功
bool WriteText(const std::filesystem::path& file, std::string_view text) {
    // 显式走 `path::string()`：MSVC 的 `std::ofstream(path)` 重载在宽字符转换
    // 失败时会静默不打开文件，只表现为"写入返回 false"，看不出根因。
    std::ofstream output(file.string(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    return !output.fail();
}

/// @brief 写出含指定 include 路径的测试 A2L
/// @param dir 测试目录
/// @param include_path 要写进 `/include` 的路径
/// @return 写出的 A2L 文件路径；失败返回空路径
std::filesystem::path WriteA2l(const std::filesystem::path& dir,
                               std::string_view include_path) {
    std::string text(kA2lTemplate);
    const auto pos = text.find("%INCLUDE%");
    if (pos == std::string::npos) {
        return {};
    }
    text.replace(pos, std::string_view("%INCLUDE%").size(), include_path);
    const auto file = dir / "main.a2l";
    if (!WriteText(file, text)) {
        return {};
    }
    return file;
}

}  // namespace

/// @brief 核心行为：缺失的 `.a2l` include 失败时，除抛出异常外还要记一条错误日志
TEST(A2lIncludeLogTest, MissingA2lIncludeIsLoggedAsError) {
    const auto dir = MakeTestDir("missing_a2l_log");
    ASSERT_FALSE(dir.empty());
    const auto file = WriteA2l(dir, "NotThere.a2l");
    ASSERT_FALSE(file.empty());

    // 收集 a2llib 日志通道里的错误级记录
    std::vector<std::string> logged_errors;
    a2l::A2lLogStream::SetLogFunction(
        [&logged_errors](const std::source_location&,
                         a2l::A2lLogSeverity severity,
                         const std::string& text) {
            if (severity == a2l::A2lLogSeverity::kError) {
                logged_errors.push_back(text);
            }
        });

    a2l::A2lFile a2l_file;
    a2l_file.Filename(file.string());
    const bool parsed = a2l_file.ParseFile();
    a2l::A2lLogStream::ResetLogFunction();

    EXPECT_FALSE(parsed) << "缺失的 .a2l include 必须导致解析失败";
    EXPECT_NE(a2l_file.LastError().find("File doesn't exist"), std::string::npos)
        << "错误信息应说明文件缺失，实际：" << a2l_file.LastError();

    bool logged = false;
    for (const auto& text : logged_errors) {
        if (text.find("File doesn't exist") != std::string::npos) {
            logged = true;
            break;
        }
    }
    EXPECT_TRUE(logged) << "缺失的 .a2l include 应经 A2lLogStream 记录错误日志，"
                           "实际未捕获到任何错误级日志";
}

/// @brief 跳过缺失的非 A2L include 时，必须记一条说明"跳过了哪个文件"的日志
TEST(A2lIncludeLogTest, SkippedNonA2lIncludeIsLogged) {
    const auto dir = MakeTestDir("skipped_aml_log");
    ASSERT_FALSE(dir.empty());
    const auto file = WriteA2l(dir, "XCP.aml");
    ASSERT_FALSE(file.empty());

    std::vector<std::string> logged;
    a2l::A2lLogStream::SetLogFunction(
        [&logged](const std::source_location&, a2l::A2lLogSeverity,
                  const std::string& text) { logged.push_back(text); });

    a2l::A2lFile a2l_file;
    a2l_file.Filename(file.string());
    const bool parsed = a2l_file.ParseFile();
    a2l::A2lLogStream::ResetLogFunction();

    ASSERT_TRUE(parsed) << "缺失的非 A2L include 应被跳过并继续解析，实际失败："
                        << a2l_file.LastError();
    bool logged_skip = false;
    for (const auto& text : logged) {
        if (text.find("Skipped missing non-A2L include") != std::string::npos &&
            text.find("XCP.aml") != std::string::npos) {
            logged_skip = true;
            break;
        }
    }
    EXPECT_TRUE(logged_skip) << "跳过日志必须说明被跳过的是哪个文件，"
                                "实际未捕获到这样的日志";
}

/// @brief 日志文本与异常消息同源：两处都必须是同一句缺失说明
TEST(A2lIncludeLogTest, LogTextMatchesLastError) {
    const auto dir = MakeTestDir("log_text_matches");
    ASSERT_FALSE(dir.empty());
    const auto file = WriteA2l(dir, "Missing.a2l");
    ASSERT_FALSE(file.empty());

    std::vector<std::string> logged_errors;
    a2l::A2lLogStream::SetLogFunction(
        [&logged_errors](const std::source_location&,
                         a2l::A2lLogSeverity severity,
                         const std::string& text) {
            if (severity == a2l::A2lLogSeverity::kError) {
                logged_errors.push_back(text);
            }
        });

    a2l::A2lFile a2l_file;
    a2l_file.Filename(file.string());
    const bool parsed = a2l_file.ParseFile();
    a2l::A2lLogStream::ResetLogFunction();

    EXPECT_FALSE(parsed) << "缺失的 .a2l include 必须导致解析失败";

    ASSERT_EQ(logged_errors.size(), 1U)
        << "每次 include 失败应恰好记一条错误日志，实际：" << logged_errors.size();
    const auto& last_error = a2l_file.LastError();
    EXPECT_NE(last_error.find(logged_errors.front()), std::string::npos)
        << "异常消息应包含日志文本；日志：" << logged_errors.front()
        << "，异常：" << last_error;
}
