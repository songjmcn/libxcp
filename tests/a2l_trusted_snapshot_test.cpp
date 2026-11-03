// 受控 A2L 快照校验的自动化测试：覆盖成功路径与失败关闭路径。
// 注意：本机 Windows 只能验证本机 ACL/重解析语义；POSIX 分支的用例在
// Windows 上编译但不运行，跨平台结论须在对应平台实跑后才成立。

#include <gtest/gtest.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "libxcp/trusted_a2l_snapshot.hpp"
#include "src/detail/fs/trusted_snapshot_platform.hpp"

namespace fs = std::filesystem;
using calmcar::xcp::StageTrustedA2lSnapshot;
// 快照用例的基目录不放在系统临时目录下：CI runner 的 %TEMP% 往往把写权限授予
// Users/Authenticated Users，那样"他人可写"的拒绝点会落在用例自己构造的快照根
// 之上，负向用例的锚定断言无法成立。改用可执行文件所在目录（构建产物目录，父链
// 可控且仅本人可写）后，负向用例在 CI 上也能真跑而不是退化成跳过。
namespace fs_platform = calmcar::xcp::detail::fs;

namespace {

#if defined(_WIN32)
/**
 * @brief 把目录的 DACL 换成 NULL DACL（等价于所有人完全控制）。
 * @param dir 目标目录。
 * @return 是否设置成功。
 * @details 用 NULL DACL 而不是依赖本机 %TEMP% 的既有权限，是为了让负向用例
 *          "自己造出他人可写"的场景：环境的权限恰好是私有时也照常验证，
 *          不允许退化成跳过。
 */
bool SetNullDacl(const fs::path& dir) {
    auto* descriptor = static_cast<PSECURITY_DESCRIPTOR>(
        ::LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
    if (descriptor == nullptr) return false;
    bool ok = ::InitializeSecurityDescriptor(descriptor,
                                             SECURITY_DESCRIPTOR_REVISION) != FALSE;
    if (ok) {
        ok = ::SetSecurityDescriptorDacl(descriptor, TRUE, nullptr, FALSE) != FALSE;
    }
    if (ok) {
        ok = ::SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION,
                                descriptor) != FALSE;
    }
    ::LocalFree(descriptor);
    return ok;
}

/**
 * @brief 用 SDDL 覆盖目录的 DACL。
 * @param dir 目标目录。
 * @param sddl 安全描述符定义语言文本。
 * @return 是否设置成功。
 */
bool SetDaclFromSddl(const fs::path& dir, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
    }
    const bool ok = ::SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION,
                                       descriptor) != FALSE;
    ::LocalFree(descriptor);
    return ok;
}
#endif

/**
 * @brief 判断拒绝理由是否指向给定路径。
 * @param reason 被测函数给出的拒绝理由。
 * @param target 期望的拒绝点（用例自己构造并污染的快照根）。
 * @return 理由中是否出现了该路径的任一等价写法。
 * @details 被测函数内部会对路径做 `weakly_canonical`，在符号链接、短文件名
 *          （8.3）或 `\\?\` 前缀目录下，规范形与用例传入的原始字符串可能不同。
 *          只比较原始字符串会让"拒绝点正确"的用例在个别机器上误报失败，因此
 *          这里同时接受相对原始形、绝对形与规范形三种写法。
 */
bool ReasonMentionsPath(const std::string& reason, const fs::path& target) {
    std::error_code ec;
    const auto absolute = fs::absolute(target, ec);
    const auto canonical = fs::weakly_canonical(target, ec);
    std::vector<std::string> forms{target.string()};
    if (!absolute.empty()) forms.push_back(absolute.string());
    if (!canonical.empty()) forms.push_back(canonical.string());
    for (const auto& form : forms) {
        if (!form.empty() && reason.find(form) != std::string::npos) return true;
    }
    return false;
}

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
 * @details 探测顺序的第一候选是**可执行文件所在目录**：CI runner 的系统临时
 *          目录常常把写权限授予 Users/Authenticated Users，此时快照必须失败
 *          关闭，成功路径在那台机器上就不可验证。构建产物目录由构建过程创建，
 *          祖先链可控，因此能稳定满足"仅本人可写"。`%LOCALAPPDATA%` 与系统
 *          临时目录保留为兜底，覆盖"可执行文件目录被镜像成只读"的极端环境。
 */
fs::path FindPrivateBase(std::string* last_reason = nullptr) {
    std::vector<fs::path> candidates;
    // 首选可执行文件目录：CI 与本地构建产物目录都可写，且祖先链由构建过程创建。
    const auto exe_dir = fs_platform::ExecutableDirectory();
    if (!exe_dir.empty()) {
        candidates.emplace_back(exe_dir / "libxcp_snapshot_base");
    }
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
 * @details 目录建在可执行文件所在目录下，而不是系统临时目录：CI runner 的
 *          `%TEMP%` 带"非可信主体可写"的 ACE，会让负向用例的拒绝点落在用例
 *          自己构造的快照根之上，锚定断言随之失效（本用例集在 CI 上就是这样
 *          失败的）。构建产物目录由构建过程创建，祖先链可控、仅本人可写。
 */
class SnapshotTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 可执行文件目录是本用例集的唯一基目录来源：取不到就直接判失败，
        // 绝不复用系统临时目录（那正是让负向用例在 CI 上失去锚定断言的原因）。
        const auto exe_dir = fs_platform::ExecutableDirectory();
        ASSERT_FALSE(exe_dir.empty())
            << "无法确定可执行文件所在目录，快照用例无处建目录";
        const auto base = exe_dir /
                          ("libxcp_snapshot_" +
                           std::to_string(::testing::UnitTest::GetInstance()
                                              ->random_seed()) +
                           "_" + std::to_string(counter_++));
        std::error_code ec;
        fs::create_directories(base, ec);
        ASSERT_FALSE(ec) << "无法创建测试目录：" << ec.message() << "（" << base
                         << "）";
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

/**
 * @brief 他人可写的目录必须失败关闭，并给出明确拒绝点。
 * @details 目录权限由用例自己构造，不依赖本机 %TEMP% 的实际 ACL：否则在
 *          "临时目录恰好仅本人可写"的机器上会退化成一跳而过，安全回归形同
 *          虚设（把守卫改成永远通过也测不出来）。构造失败一律判失败。
 */
TEST_F(SnapshotTest, RejectsRootWritableByOthers) {
    const auto source = WriteSample("source.a2l");
    const auto root = dir_ / "snapshot";
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(ec) << "无法创建快照目录：" << ec.message();
#if defined(_WIN32)
    ASSERT_TRUE(SetNullDacl(root))
        << "无法把快照目录设为 NULL DACL，错误码 " << ::GetLastError();
#else
    fs::permissions(root, fs::perms::all, fs::perm_options::replace, ec);
    ASSERT_FALSE(ec) << "无法放宽快照目录权限：" << ec.message();
#endif
    const auto result = StageTrustedA2lSnapshot(source, root);
    EXPECT_FALSE(result.trusted) << "其他人可完全控制的目录不得作为可信快照根";
    EXPECT_TRUE(result.path.empty());
    EXPECT_FALSE(result.reason.empty());
    // 拒绝点必须落在用例自己构造的目录上。若只断言"整体不可信"，祖先目录
    // 的权限问题也会让用例通过，安全判定本身被关掉时照样测不出来（变异验证
    // 已证实这一点）。
    EXPECT_TRUE(ReasonMentionsPath(result.reason, root))
        << "拒绝点不在快照根上：" << result.reason;
    // 拒绝点措辞按平台不同：Windows 走 ACL/重解析点判定，POSIX 走权限位
    // 判定（"目录对组或其他身份可写"）。这里只断言"给出了可读的拒绝理由"，
    // 不把某一平台的措辞当作契约，否则同一语义会在另一平台误报失败。
    EXPECT_TRUE(result.reason.find("写权限") != std::string::npos ||
                result.reason.find("可写") != std::string::npos ||
                result.reason.find("ACL") != std::string::npos ||
                result.reason.find("DACL") != std::string::npos ||
                result.reason.find("重解析") != std::string::npos)
        << result.reason;
}

#if defined(_WIN32)
/**
 * @brief 目录上"仅继承"的 Everyone 写权限会落到快照文件上，必须拒绝。
 * @details DACL 只有一条 OI|CI|IO 的 Everyone 完全控制 ACE：目录自身看不见
 *          这份权限，但里面新建的快照文件会继承它。旧实现按"仅继承项跳过"
 *          处理目录，就会把这个目录判成可信。
 */
TEST_F(SnapshotTest, RejectsRootWithInheritOnlyEveryoneWrite) {
    const auto source = WriteSample("source.a2l");
    const auto root = dir_ / "snapshot";
    std::error_code ec;
    fs::create_directories(root, ec);
    ASSERT_FALSE(ec) << "无法创建快照目录：" << ec.message();
    // WD=Everyone（仅继承）、OW=属主、SY=SYSTEM、BA=Administrators；后三条
    // 保证用例自己仍能访问目录，避免把它变成"谁都进不去"而误判通过。
    ASSERT_TRUE(SetDaclFromSddl(
        root, L"D:(A;OICIIO;FA;;;WD)(A;;FA;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)"))
        << "无法设置继承型 DACL，错误码 " << ::GetLastError();
    const auto result = StageTrustedA2lSnapshot(source, root);
    EXPECT_FALSE(result.trusted)
        << "目录上仅继承的 Everyone 写权限会让快照文件被他人改写，必须拒绝";
    EXPECT_TRUE(result.path.empty());
    EXPECT_FALSE(result.reason.empty()) << result.reason;
    // 同 RejectsRootWritableByOthers：拒绝点必须是这个目录本身。
    EXPECT_TRUE(ReasonMentionsPath(result.reason, root))
        << "拒绝点不在快照根上：" << result.reason;
}
#endif

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