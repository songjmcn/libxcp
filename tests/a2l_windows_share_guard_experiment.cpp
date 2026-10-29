/**
 * @file a2l_windows_share_guard_experiment.cpp
 * @brief 独立验证 Windows 读守护句柄与 SDK 多次路径加载的共享兼容性。
 * @details 仅为共享语义实验，不证明临时路径、ACL、重解析点、输入内容或
 * 快照可信性。无需注册 CTest；执行时从 DLL 所在目录加载 SDK。
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "libxcp/a2l/a2l_bridge.hpp"

namespace {
/** @brief 关闭 Windows 内核句柄。 */
class Handle {
public:
    /** @brief 接管句柄。 */
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    /** @brief 析构时关闭有效句柄。 */
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    /** @brief 返回原生句柄。 */
    HANDLE Get() const { return value_; }
    /** @brief 关闭句柄。 */
    void Reset() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE value_;  ///< 所持句柄。
};

/** @brief 删除本实验创建的单个文件。 */
class Cleanup {
public:
    /** @brief 记住新建文件路径。 */
    explicit Cleanup(std::filesystem::path path) : path_(std::move(path)) {}
    /** @brief 尝试删除文件。 */
    ~Cleanup() { if (!DeleteFileW(path_.c_str())) std::cerr << "cleanup failed: " << GetLastError() << '\n'; }
private:
    std::filesystem::path path_;  ///< 仅在成功独占创建后设置。
};

/** @brief 报告失败，保留 Win32 错误码。 */
bool Check(bool ok, const char* what, DWORD error = 0) {
    std::cout << (ok ? "PASS " : "FAIL ") << what;
    if (!ok) std::cout << " (error " << error << ')';
    std::cout << '\n';
    return ok;
}
}  // namespace

/** @brief 先后测试已有写者与读守护者，并多次调用真实 SDK。 */
int main() {
    constexpr char content[] = "ASAP2_VERSION 1 61\n/begin PROJECT p \"\"\n"
                               "/begin MODULE m \"\"\n/end MODULE\n/end PROJECT\n";
    wchar_t temp[MAX_PATH + 1]{};
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    if (!Check(n > 0 && n < MAX_PATH, "GetTempPathW", GetLastError())) return 1;
    const auto path = std::filesystem::path(temp) /
        (L"a2l-share-experiment-" + std::to_wstring(GetCurrentProcessId()) + L".a2l");
    Handle created(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!Check(created.Get() != INVALID_HANDLE_VALUE, "CREATE_NEW", GetLastError())) return 1;
    Cleanup cleanup(path);
    DWORD written = 0;
    const bool write_ok = WriteFile(created.Get(), content, sizeof(content) - 1, &written, nullptr) && written == sizeof(content) - 1;
    if (!Check(write_ok, "write sample", GetLastError())) return 1;
    created.Reset();

    Handle prior_writer(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!Check(prior_writer.Get() != INVALID_HANDLE_VALUE, "open prior writer", GetLastError())) return 1;
    Handle incompatible_guard(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
    const DWORD prior_error = GetLastError();
    if (!Check(incompatible_guard.Get() == INVALID_HANDLE_VALUE && prior_error == ERROR_SHARING_VIOLATION,
               "guard rejects existing writer", prior_error)) return 1;
    prior_writer.Reset();

    Handle guard(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!Check(guard.Get() != INVALID_HANDLE_VALUE, "open read guard", GetLastError())) return 1;
    Handle later_writer(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    const DWORD later_error = GetLastError();
    if (!Check(later_writer.Get() == INVALID_HANDLE_VALUE && later_error == ERROR_SHARING_VIOLATION,
               "guard rejects later writer", later_error)) return 1;
    Handle another_reader(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!Check(another_reader.Get() != INVALID_HANDLE_VALUE, "guard permits compatible reader", GetLastError())) return 1;

    calmcar::xcp::a2l::LoadOptions options;
    options.require_if_data_xcp = false;
    for (int i = 0; i < 3; ++i) {
        const auto loaded = calmcar::xcp::a2l::A2lBridge::Load(path.string(), options);
        if (!loaded.HasValue()) {
            std::cerr << "SDK load failed: " << loaded.ErrorInfo().message << '\n';
            return 1;
        }
        if (!Check(loaded.Value()->Database() != nullptr, "SDK load and database ready")) return 1;
    }
    return 0;
}
#else
#include <iostream>
/** @brief 非 Windows 明确不运行该实验。 */
int main() { std::cerr << "Windows only\n"; return 77; }
#endif
