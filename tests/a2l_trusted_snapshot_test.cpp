// 受控 A2L 快照校验的自动化测试：覆盖成功路径与失败关闭路径。
// 注意：本机 Windows 只能验证本机 ACL/重解析语义；POSIX 分支的用例在
// Windows 上编译但不运行，跨平台结论须在对应平台实跑后才成立。

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "libxcp/trusted_a2l_snapshot.hpp"

namespace fs = std::filesystem;
using calmcar::xcp::StageTrustedA2lSnapshot;

namespace {

/// @brief 最小可解析 A2L 文本：仅用于快照字节一致性校验，不参与语义解析。
constexpr char kSampleA2l[] =
    "ASAP2_VERSION 1 71\n"
    "/begin PROJECT demo \"\"\n"
    "/begin MODULE demo \"\"\n"
    "/end MODULE\n"
    "/end PROJECT\n";

/**
 * @brief 选择一个"仅本人可写"的基目录，用于验证成功路径。
 * @return 通过的基目录；本机没有任何满足条件的目录时返回空路径。
 * @details 不能直接使用 `temp_directory_path()`：Windows 的 `%TEMP%` 常常把
 *          写权限授予 Users/Authenticated Users，此时快照必须失败关闭，
 *          成功路径在这台机器上就不可验证。这里按"用户配置目录 → 临时目录"
 *          顺序探测，用被测函数自身判定（探测件本身也会被读回校验）。
 */
fs::path FindPrivateBase(std::string* last_reason = nullptr) {
    std::vector<fs::path> candidates;
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA");
        local != nullptr && *local != '\0') {
        candidates.emplace_back(fs::path(local) / "libxcp_snapshot_base");
    }
#endif
    candidates.push_back(fs::temp_directory_path() / "libxcp_snapshot_base");
    std::string reason;
    for (const auto& candidate : candidates) {
        std::error_code ec;
        fs::create_directories(candidate, ec);
        if (ec) {
            reason = "无法创建探测目录 " + candidate.string() + "：" + ec.message();
            continue;
        }
        // 先放一份真实样例，再用被测函数探测该目录是否"仅本人可写"。
        std::ofstream sample(candidate / "probe.a2l",
                             std::ios::binary | std::ios::trunc);
        if (!sample.is_open()) {
            reason = "无法在探测目录中写入样例：" + candidate.string();
            continue;
        }
        sample.write(kSampleA2l,
                     static_cast<std::streamsize>(sizeof(kSampleA2l) - 1U));
        sample.close();
        fs::remove(candidate / "probe.a2l", ec);
        std::ofstream real(candidate / "probe.a2l", std::ios::binary | std::ios::trunc);
        real.write(kSampleA2l,
                   static_cast<std::streamsize>(sizeof(kSampleA2l) - 1U));
        real.close();
        const auto probe =
            StageTrustedA2lSnapshot(candidate / "probe.a2l", candidate / "probe");
        fs::remove(candidate / "probe.a2l", ec);
        if (probe.trusted) return candidate;
        // 记录被测函数给出的拒绝点：跳过时把它打印出来，避免读者误判为代码缺陷。
        reason = candidate.string() + " → " + probe.reason;
    }
    if (last_reason != nullptr) *last_reason = reason;
    return {};
}

/**
 * @brief 每个用例独占的临时目录，析构时尽量清理。
 */
class SnapshotTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto base = fs::temp_directory_path() /
                          ("libxcp_snapshot_" +
                           std::to_string(::testing::UnitTest::GetInstance()
                                              ->random_seed()) +
                           "_" + std::to_string(counter_++));
        std::error_code ec;
        fs::create_directories(base, ec);
        ASSERT_FALSE(ec) << "无法创建测试目录：" << ec.message();
        dir_ = base;
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    /// @brief 写入一份示例 A2L 并返回其路径。
    fs::path WriteSample(const std::string& name,
                         const std::string& text = kSampleA2l) {
        const auto path = dir_ / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        return path;
    }

    fs::path dir_;              ///< 用例目录。
    inline static int counter_ = 0;  ///< 避免同秒内目录重名。
};

/// @brief 成功路径：快照可读回、内容一致，且守卫存在时其他读者仍可打开。
TEST_F(SnapshotTest, StagesReadableCopyOfSource) {
    std::string reject_reason;
    const auto base = FindPrivateBase(&reject_reason);
    if (base.empty()) {
        GTEST_SKIP() << "本机没有任何仅本人可写的目录，快照成功路径不可验证；"
                        "被测函数在探测目录上的拒绝点："
                     << reject_reason;
    }
    const auto source = WriteSample("source.a2l");
    const auto result = StageTrustedA2lSnapshot(source, base / "snapshot");
    ASSERT_TRUE(result.trusted) << result.reason;
    ASSERT_FALSE(result.path.empty());
    EXPECT_EQ(result.bytes, std::string(kSampleA2l));
    EXPECT_NE(result.guard, nullptr);

    std::ifstream reread(result.path, std::ios::binary);
    ASSERT_TRUE(reread.is_open()) << "守卫存在时 SDK 仍需能按路径打开快照";
    const std::string content{std::istreambuf_iterator<char>(reread), {}};
    EXPECT_EQ(content, std::string(kSampleA2l));
}

/// @brief 原始文件不存在：必须失败关闭，而不是悄悄创建空快照。
TEST_F(SnapshotTest, RejectsMissingSource) {
    const auto result =
        StageTrustedA2lSnapshot(dir_ / "missing.a2l", dir_ / "snapshot");
    EXPECT_FALSE(result.trusted);
    EXPECT_NE(result.reason.find("无法打开原始 A2L"), std::string::npos)
        << result.reason;
    EXPECT_TRUE(result.path.empty());
}

/// @brief 原始文件为空：同样失败关闭。
TEST_F(SnapshotTest, RejectsEmptySource) {
    const auto source = WriteSample("empty.a2l", "");
    const auto result = StageTrustedA2lSnapshot(source, dir_ / "snapshot");
    EXPECT_FALSE(result.trusted);
    EXPECT_NE(result.reason.find("为空"), std::string::npos) << result.reason;
}

/// @brief 根目录路径被普通文件占用：无法建立目录，必须失败关闭。
TEST_F(SnapshotTest, RejectsRootThatIsAFile) {
    const auto source = WriteSample("source.a2l");
    const auto blocker = WriteSample("blocker", "x");
    const auto result = StageTrustedA2lSnapshot(source, blocker);
    EXPECT_FALSE(result.trusted);
    EXPECT_FALSE(result.reason.empty());
}

/// @brief 每次快照都必须是新文件，不能覆盖已有快照。
TEST_F(SnapshotTest, CreatesDistinctFilesPerCall) {
    std::string reject_reason;
    const auto base = FindPrivateBase(&reject_reason);
    if (base.empty()) {
        GTEST_SKIP() << "本机没有任何仅本人可写的目录，快照成功路径不可验证；"
                        "被测函数在探测目录上的拒绝点："
                     << reject_reason;
    }
    const auto source = WriteSample("source.a2l");
    const auto root = base / "snapshot";
    const auto first = StageTrustedA2lSnapshot(source, root);
    const auto second = StageTrustedA2lSnapshot(source, root);
    ASSERT_TRUE(first.trusted) << first.reason;
    ASSERT_TRUE(second.trusted) << second.reason;
    EXPECT_NE(first.path, second.path);
}

/// @brief 守卫释放后必须删除快照文件，避免泄露标定数据。
TEST_F(SnapshotTest, DeletesSnapshotWhenGuardReleased) {
    std::string reject_reason;
    const auto base = FindPrivateBase(&reject_reason);
    if (base.empty()) {
        GTEST_SKIP() << "本机没有任何仅本人可写的目录，快照成功路径不可验证；"
                        "被测函数在探测目录上的拒绝点："
                     << reject_reason;
    }
    const auto source = WriteSample("source.a2l");
    fs::path stamped;
    {
        const auto result = StageTrustedA2lSnapshot(source, base / "snapshot");
        ASSERT_TRUE(result.trusted) << result.reason;
        stamped = result.path;
        EXPECT_TRUE(fs::exists(stamped));
    }
    EXPECT_FALSE(fs::exists(stamped)) << "快照应由守卫释放时删除：" << stamped;
}

/// @brief 他人可写的目录必须失败关闭，并给出明确拒绝点（本机 %TEMP% 即此情形）。
TEST_F(SnapshotTest, RejectsRootWritableByOthers) {
    const auto source = WriteSample("source.a2l");
    const auto result = StageTrustedA2lSnapshot(source, dir_ / "snapshot");
    if (result.trusted) {
        GTEST_SKIP() << "本机临时目录已仅本人可写，该拒绝路径需在他人可写的目录上验证";
    }
    EXPECT_TRUE(result.path.empty());
    EXPECT_FALSE(result.reason.empty());
    // 拒绝点措辞按平台不同：Windows 走 ACL/重解析点判定，POSIX 走权限位
    // 判定（"目录对组或其他身份可写"）。这里只断言"给出了可读的拒绝理由"，
    // 不把某一平台的措辞当作契约，否则同一语义会在另一平台误报失败。
    EXPECT_TRUE(result.reason.find("写权限") != std::string::npos ||
                result.reason.find("可写") != std::string::npos ||
                result.reason.find("ACL") != std::string::npos ||
                result.reason.find("重解析") != std::string::npos)
        << result.reason;
}

#if !defined(_WIN32)
/// @brief POSIX：其他人可写的目录不得作为可信快照根。
TEST_F(SnapshotTest, RejectsWorldWritableRootOnPosix) {
    const auto source = WriteSample("source.a2l");
    const auto root = dir_ / "open";
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(ec);
    fs::permissions(root, fs::perms::all, fs::perm_options::replace, ec);
    ASSERT_FALSE(ec);
    const auto result = StageTrustedA2lSnapshot(source, root);
    EXPECT_FALSE(result.trusted);
    EXPECT_NE(result.reason.find("可写"), std::string::npos) << result.reason;
}
#endif

}  // namespace