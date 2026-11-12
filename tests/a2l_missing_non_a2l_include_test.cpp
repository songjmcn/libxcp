// =============================================================================
// a2l_missing_non_a2l_include_test.cpp —— 缺失的非 A2L include 不再中断加载
//
// 背景：A2L 常见的 `include "XCP.aml"` 指向 AML 伴生文件。上游
// `A2lScanner::FixIncludeFile()` 原本**先检查文件存在性、后判断扩展名**，
// 于是伴生 AML 缺失时整个 A2L 加载失败——尽管 AML 定义从未被解释：
// 解析器只把 A2ML 块当作不透明字符串存进 `Module::A2ml()`。
//
// 现行语义（v12）：`/include` 的目标**存在就按原逻辑解析**（含 `.aml`），
// **不存在**时按扩展名分两路——非 A2L 跳过该 include 并继续解析、`.a2l`
// 仍然报错。跳过既不能中断加载，也不能悄悄消失：SDK 在 include 图预扫描
// 阶段记录被跳过的目标，桥接层转成 `LoadWarning`（code=NotFound、
// phase=Load），xcp_master 会把它打印成"A2L 告警"。
//
// 本文件守住五件事：
//   1. 非 A2L 伴生文件缺失时，加载成功且符号完整（核心行为）；
//   2. 缺失的非 A2L include 会被记为 LoadWarning（留痕，不静默）；
//   3. 非 A2L 伴生文件存在时，仍按标准 include 进来（行为不回退）；
//   4. `.a2l` include 缺失时**仍然报错**（不得静默残缺）；
//   5. 绝对路径与相对路径、大小写不同的扩展名都按同一规则处理。
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "libxcp/a2l/a2l_bridge.hpp"

namespace {

using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::LoadOptions;

/// @brief 含 AML include 与一个测量量的最小 A2L（include 路径由调用方代入）
constexpr std::string_view kA2lTemplate = R"ASAP2(ASAP2_VERSION 1 71
/begin PROJECT misinc ""
/begin MODULE misinc_m ""
/begin A2ML
struct Sig { char sig[4]; };
/end A2ML
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

/// @brief 一个**语法合法**的 AML 伴生文件内容
/// @details 「存在则继续解析」意味着这段字节会真的进入词法器，因此它必须是
///          合法的 `/begin A2ML ... /end A2ML` 块；写成注释会让 MODULE 层
///          出现裸文本，解析器报语法错误——那不是 include 语义的问题。
constexpr std::string_view kCompanionAml = R"AML(/begin A2ML
struct Companion { char tag[4]; };
/end A2ML
/begin MEASUREMENT AmlInjected "injected by the companion file" UBYTE CM 0 0 0 255
  ECU_ADDRESS 0x1004
/end MEASUREMENT
)AML";

/// @brief 检查目录能否真正建子目录（`temp_directory_path()` 只报路径，不证明可写）
/// @param dir 待检查的目录
/// @return true = 可写；false = 不存在、无权限或无法创建
/// @details 受限环境（沙箱、低完整性级别进程）里 `temp_directory_path()`
///          会正常返回路径，但随后的 `create_directories` 抛
///          "Access is denied"。只判断路径不够，必须实探一次。
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
/// @details `temp_directory_path()` 在受限环境里会抛 `filesystem_error`，
///          或者在沙箱里返回一个**不可写**的路径。两者都会让整批用例以
///          "unknown file: error: C++ exception" 的形式失败，看不出是环境问题。
///          因此这里必须实探写入能力，而不能只看路径是否存在。
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
    const auto root = base / "libxcp_missing_include" / tag;
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
    // 用 `std::filesystem::path::string()` 显式转换，原因同 `MakeTestDir()`：
    // MSVC 的 `std::ofstream(path)` 重载会在宽字符转换失败时静默不打开文件，
    // 表现为"写入返回 false"而看不出根因。
    std::ofstream output(file.string(), std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    return !output.fail();
}

/// @brief 用给定的 include 路径生成 A2L 并写到磁盘
/// @param dir 测试目录
/// @param include_path 代入 `%INCLUDE%` 的路径文本
/// @return A2L 文件路径；失败返回空路径
std::filesystem::path WriteA2l(const std::filesystem::path& dir,
                               const std::string& include_path) {
    std::string text(kA2lTemplate);
    const std::string marker = "%INCLUDE%";
    const auto pos = text.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    text.replace(pos, marker.size(), include_path);
    const auto file = dir / "main.a2l";
    if (!WriteText(file, text)) {
        return {};
    }
    return file;
}

/// @brief 加载 A2L 并返回桥接对象
/// @param file A2L 路径
/// @param loaded 输出：加载结果（用于失败诊断）
/// @return 成功返回桥接对象，失败返回 nullptr
std::unique_ptr<A2lBridge> Load(const std::filesystem::path& file,
                                std::string& error) {
    LoadOptions options;
    options.require_if_data_xcp = false;  // 本文件只关心 include 处理
    options.allow_include_outside_root = true;
    auto loaded = A2lBridge::Load(file.string(), options);
    if (!loaded.HasValue()) {
        error = loaded.ErrorInfo().message;
        return nullptr;
    }
    return std::move(loaded.Value());
}

/// @brief 已发布的加载告警里是否存在指向该路径的"跳过"告警
/// @param bridge 已加载的桥接对象
/// @param path 期望出现在告警中的路径片段（比较时按子串匹配，避免分隔符差异）
/// @return true = 找到一条 message 含该路径的告警
bool HasSkippedWarning(const A2lBridge& bridge, std::string_view path) {
    for (const auto& warning : bridge.ListLoadWarnings()) {
        if (warning.message.find(std::string(path)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// @brief 已发布快照中是否存在指定全限定名
bool HasSymbol(const A2lBridge& bridge, std::string_view name) {
    const auto* database = bridge.Database();
    if (database == nullptr) {
        return false;
    }
    return database->Find(name).HasValue();
}

}  // namespace

/// @brief 核心修复：伴生 AML 缺失时加载仍然成功，符号完整
TEST(A2lMissingNonA2lIncludeTest, SkipsMissingAmlCompanionFile) {
    const auto dir = MakeTestDir("missing_aml");
    ASSERT_FALSE(dir.empty());
    // 故意不创建 XCP.aml
    const auto file = WriteA2l(dir, "XCP.aml");
    ASSERT_FALSE(file.empty());

    std::string error;
    const auto bridge = Load(file, error);
    ASSERT_NE(bridge, nullptr) << "缺失 AML 仍应加载成功，实际失败：" << error;
    EXPECT_TRUE(HasSymbol(*bridge, "misinc_m::SigVolt"));
    // 跳过必须留痕：否则调用方会以为解析出的是完整模型
    EXPECT_TRUE(HasSkippedWarning(*bridge, "XCP.aml"))
        << "缺失 AML 被跳过时应产生一条 LoadWarning";
}

/// @brief 绝对路径写法的缺失 AML 同样被跳过
TEST(A2lMissingNonA2lIncludeTest, SkipsMissingAmlWithAbsolutePath) {
    const auto dir = MakeTestDir("missing_aml_abs");
    ASSERT_FALSE(dir.empty());
    const auto absolute = (dir / "NotThere.aml").generic_string();
    const auto file = WriteA2l(dir, absolute);
    ASSERT_FALSE(file.empty());

    std::string error;
    const auto bridge = Load(file, error);
    ASSERT_NE(bridge, nullptr) << "绝对路径缺失 AML 仍应加载成功，实际失败：" << error;
    EXPECT_TRUE(HasSymbol(*bridge, "misinc_m::SigVolt"));
}

/// @brief 扩展名大小写不敏感：`.AML` 缺失也被跳过
TEST(A2lMissingNonA2lIncludeTest, ExtensionMatchIsCaseInsensitive) {
    const auto dir = MakeTestDir("missing_aml_upper");
    ASSERT_FALSE(dir.empty());
    const auto file = WriteA2l(dir, "XCP.AML");
    ASSERT_FALSE(file.empty());

    std::string error;
    const auto bridge = Load(file, error);
    ASSERT_NE(bridge, nullptr) << "大写 .AML 缺失仍应加载成功，实际失败：" << error;
    EXPECT_TRUE(HasSymbol(*bridge, "misinc_m::SigVolt"));
}

/// @brief 伴生 AML 存在时行为不回退：仍按标准 include，且内容真的进了模型
TEST(A2lMissingNonA2lIncludeTest, IncludesNonA2lFileWhenPresent) {
    const auto dir = MakeTestDir("present_aml");
    ASSERT_FALSE(dir.empty());
    ASSERT_TRUE(WriteText(dir / "XCP.aml", kCompanionAml));
    const auto file = WriteA2l(dir, "XCP.aml");
    ASSERT_FALSE(file.empty());

    std::string error;
    const auto bridge = Load(file, error);
    ASSERT_NE(bridge, nullptr) << "存在的 AML 应正常 include，实际失败：" << error;
    EXPECT_TRUE(HasSymbol(*bridge, "misinc_m::SigVolt"));
    // 决定性证据：伴生文件是**文本 include**，它里面的 MEASUREMENT 会被注入
    // 当前模块。若这条断言失败，说明"存在则解析"没有真正生效（文件被静默
    // 跳过了），而不是仅仅"没报错"。
    EXPECT_TRUE(HasSymbol(*bridge, "misinc_m::AmlInjected"))
        << "伴生文件里的 MEASUREMENT 未进入模型，说明该 include 未被解析";
    // 存在即解析：没有跳过，自然不该有跳过告警
    EXPECT_FALSE(HasSkippedWarning(*bridge, "XCP.aml"))
        << "AML 存在时不应产生跳过告警";
}

/// @brief 回归保护：`.a2l` include 缺失必须仍然报错，不得静默残缺
TEST(A2lMissingNonA2lIncludeTest, StillRejectsMissingA2lInclude) {
    const auto dir = MakeTestDir("missing_a2l");
    ASSERT_FALSE(dir.empty());
    const auto file = WriteA2l(dir, "NotThere.a2l");
    ASSERT_FALSE(file.empty());

    std::string error;
    const auto bridge = Load(file, error);
    EXPECT_EQ(bridge, nullptr) << "缺失的 .a2l include 必须报错，不得静默跳过";
    EXPECT_NE(error.find("File doesn't exist"), std::string::npos)
        << "错误信息应说明文件缺失，实际：" << error;
}

/// @brief 无扩展名的 include 视为非 A2L：缺失时被跳过
TEST(A2lMissingNonA2lIncludeTest, SkipsMissingIncludeWithoutExtension) {
    const auto dir = MakeTestDir("missing_noext");
    ASSERT_FALSE(dir.empty());
    const auto file = WriteA2l(dir, "plainfile");
    ASSERT_FALSE(file.empty());

    std::string error;
    const auto bridge = Load(file, error);
    ASSERT_NE(bridge, nullptr) << "无扩展名缺失 include 应被跳过，实际失败：" << error;
    EXPECT_TRUE(HasSymbol(*bridge, "misinc_m::SigVolt"));
}
