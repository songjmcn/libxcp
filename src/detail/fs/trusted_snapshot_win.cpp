/**
 * @file trusted_snapshot_win.cpp
 * @brief 受控 A2L 快照的 Windows 原生实现（ACL 校验 + 独占创建 + 只读守卫）。
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>

#include "trusted_snapshot_platform.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace calmcar::xcp::detail::fs {

/// @brief 取当前可执行文件所在目录。
std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(1024, L'\0');
    for (;;) {
        const DWORD len = ::GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
        if (len == 0) return {};
        if (len < buffer.size()) {
            buffer.resize(len);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

namespace {

/// @brief 允许对快照目录/文件拥有写权限的主体（本人、SYSTEM、管理员、属主相关）。
struct AllowedSids {
    std::vector<std::vector<unsigned char>> storage;  ///< SID 字节缓存。

    /// @brief 复制一个 SID 到缓存中。
    void Add(PSID sid) {
        if (sid == nullptr) return;
        const DWORD len = ::GetLengthSid(sid);
        std::vector<unsigned char> copy(len);
        if (!::CopySid(len, copy.data(), sid)) return;
        storage.push_back(std::move(copy));
    }

    /// @brief 由 SDDL 字符串添加 SID。
    bool AddSddl(const wchar_t* text) {
        PSID sid = nullptr;
        if (!::ConvertStringSidToSidW(text, &sid)) return false;
        Add(sid);
        ::LocalFree(sid);
        return true;
    }

    /// @brief 判断某 SID 是否在允许集合内。
    [[nodiscard]] bool Contains(PSID sid) const {
        if (sid == nullptr) return false;
        for (const auto& item : storage) {
            if (::EqualSid(const_cast<unsigned char*>(item.data()), sid) != FALSE)
                return true;
        }
        return false;
    }
};

/**
 * @brief 取当前进程用户 SID 并补齐允许集合。
 * @param allowed 输出集合。
 * @return 是否成功取得用户 SID。
 */
inline bool BuildAllowedSids(AllowedSids& allowed) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    const bool ok = ::GetTokenInformation(token, TokenUser, buffer.data(), size,
                                          &size) != FALSE;
    ::CloseHandle(token);
    if (!ok) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    allowed.Add(user->User.Sid);
    // SYSTEM 与内置管理员组：它们本来就拥有绕过 ACL 的能力，视为可信边界之内。
    if (!allowed.AddSddl(L"S-1-5-18") || !allowed.AddSddl(L"S-1-5-32-544"))
        return false;
    // CREATOR OWNER / OWNER RIGHTS 是继承型 ACE 的常见主体，最终仍落回属主。
    return allowed.AddSddl(L"S-1-3-0") && allowed.AddSddl(L"S-1-3-4");
}

/// @brief 相对安全对象的写权限位（含删除与改 ACL/属主）。
constexpr ACCESS_MASK kWriteLikeMask =
    FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
    FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE |
    GENERIC_ALL;

/**
 * @brief 祖先目录上"足以让其他身份替换快照路径"的权限位。
 * @details 祖先目录只需要阻止他人删除/改名/重建快照目录本身或改其安全属性；
 *          "能在该目录里新建文件"（如 C:\ 根上的 Authenticated Users:(AD)）
 *          并不能改写快照内容，因此不计入。否则任何 Windows 卷根都无法通过，
 *          该能力将永远无法启用。
 */
constexpr ACCESS_MASK kAncestorForbiddenMask =
    FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL;

/**
 * @brief 检查句柄对应安全对象的 DACL：拒绝非可信主体可写。
 * @param handle 已打开的对象句柄（必须含 READ_CONTROL 权限）。
 * @param allowed 允许主体集合。
 * @param mask 需要拒绝的权限位。
 * @param include_inherit_only 是否把"仅继承给子项"的 ACE 也算作风险。
 * @param violation 失败时写入违规描述（不含路径）。
 * @return 是否通过。
 */
inline bool CheckAclByHandle(HANDLE handle, const AllowedSids& allowed,
                             ACCESS_MASK mask, bool include_inherit_only,
                             std::string* violation) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = ::GetSecurityInfo(
        handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
        nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        *violation = "无法读取 DACL（错误码 " + std::to_string(status) + "）";
        return false;
    }
    bool safe = true;
    if (dacl == nullptr) {
        // NULL DACL 表示"没有访问控制"：Windows 把它解释为所有人都拥有完全
        // 控制权，这样的目录/文件任何人都能替换内容，必须判为不可信。
        *violation = "没有 DACL（Null DACL 等价于所有人完全控制）";
        safe = false;
    } else {
        for (DWORD index = 0; index < dacl->AceCount && safe; ++index) {
            LPVOID raw = nullptr;
            if (!::GetAce(dacl, index, &raw)) continue;
            const auto* header = static_cast<const ACE_HEADER*>(raw);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
            // "仅继承"的 ACE 不作用于本对象，但它的子对象会继承这份权限：
            // 检查快照目录与快照文件时必须一并计入，否则目录本身看似只读，
            // 里面的快照文件却继承到 Everyone:(F)。
            if (!include_inherit_only &&
                (header->AceFlags & INHERIT_ONLY_ACE) != 0) {
                continue;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            if ((ace->Mask & mask) == 0) continue;
            auto* sid = const_cast<DWORD*>(&ace->SidStart);
            if (allowed.Contains(sid)) continue;
            wchar_t* text = nullptr;
            std::string name = "未知主体";
            if (::ConvertSidToStringSidW(sid, &text) != FALSE) {
                const int need = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr,
                                                       0, nullptr, nullptr);
                if (need > 0) {
                    name.assign(static_cast<std::size_t>(need - 1), '\0');
                    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, name.data(), need,
                                          nullptr, nullptr);
                }
                ::LocalFree(text);
            }
            *violation = "写权限授予了非可信主体 " + name;
            safe = false;
        }
    }
    ::LocalFree(descriptor);
    return safe;
}

/**
 * @brief 检查单个目录：拒绝重解析点，拒绝非可信主体可写。
 * @param dir 目录路径。
 * @param allowed 允许主体集合。
 * @param mask 需要拒绝的权限位（快照目录用 kWriteLikeMask，祖先用
 *             kAncestorForbiddenMask）。
 * @param include_inherit_only 是否把"仅继承给子项"的 ACE 也算作风险：
 *             快照目录传 true（子文件会继承），祖先目录传 false。
 * @param reason 失败原因。
 * @return 是否通过。
 */
inline bool CheckDirectoryByHandle(const std::filesystem::path& dir,
                                   const AllowedSids& allowed, ACCESS_MASK mask,
                                   bool include_inherit_only,
                                   std::string* reason) {
    HANDLE handle = ::CreateFileW(
        dir.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *reason = "无法打开目录以校验权限（错误码 " +
                  std::to_string(::GetLastError()) + "）：" + dir.string();
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info)) {
        ::CloseHandle(handle);
        *reason = "无法读取目录信息：" + dir.string();
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        ::CloseHandle(handle);
        *reason = "目录是重解析点/链接，快照路径无法证明：" + dir.string();
        return false;
    }
    std::string violation;
    const bool safe =
        CheckAclByHandle(handle, allowed, mask, include_inherit_only, &violation);
    ::CloseHandle(handle);
    if (!safe) {
        *reason = "目录" + violation + "：" + dir.string();
        return false;
    }
    return true;
}

/**
 * @brief 创建并写入快照文件，读取校验后返回只读守卫。
 * @return 是否成功；失败时 reason 说明原因。
 */
bool StageFileImpl(const std::filesystem::path& path, const std::string& bytes,
                   std::shared_ptr<void>* guard, std::string* reason) {
    HANDLE writer = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writer == INVALID_HANDLE_VALUE) {
        *reason = "无法独占创建快照文件（错误码 " +
                  std::to_string(::GetLastError()) + "）";
        return false;
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - written, 1U << 20U));
        DWORD done = 0;
        if (!::WriteFile(writer, bytes.data() + written, chunk, &done, nullptr) ||
            done == 0) {
            ::CloseHandle(writer);
            ::DeleteFileW(path.c_str());
            *reason = "写入快照失败（错误码 " + std::to_string(::GetLastError()) +
                      "）";
            return false;
        }
        written += done;
    }
    ::FlushFileBuffers(writer);
    ::CloseHandle(writer);

    // 以"允许其他读者、禁止写入者"的方式重新打开：SDK 会多次按路径打开文件，
    // 因此这里只能除去写共享，不能独占。
    HANDLE reader = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
    if (reader == INVALID_HANDLE_VALUE) {
        ::DeleteFileW(path.c_str());
        *reason = "无法以只读方式打开快照（错误码 " +
                  std::to_string(::GetLastError()) + "）";
        return false;
    }
    // 快照文件本身也必须只被可信主体可写：仅校验目录不够——目录上
    // Everyone:(OI)(CI)(IO)(F) 这类"只继承"的 ACE 不会作用于目录，却被
    // 快照文件原样继承，攻击者就能直接改写快照内容。
    {
        AllowedSids allowed;
        std::string violation;
        if (!BuildAllowedSids(allowed)) {
            ::CloseHandle(reader);
            ::DeleteFileW(path.c_str());
            *reason = "无法取得当前用户 SID，无法校验快照文件权限";
            return false;
        }
        if (!CheckAclByHandle(reader, allowed, kWriteLikeMask, true, &violation)) {
            ::CloseHandle(reader);
            ::DeleteFileW(path.c_str());
            *reason = "快照文件" + violation + "：" + path.string();
            return false;
        }
    }
    std::string verify(bytes.size(), '\0');
    std::size_t read_total = 0;
    while (read_total < verify.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(verify.size() - read_total, 1U << 20U));
        DWORD done = 0;
        if (!::ReadFile(reader, verify.data() + read_total, chunk, &done, nullptr) ||
            done == 0) {
            ::CloseHandle(reader);
            ::DeleteFileW(path.c_str());
            *reason = "读回快照失败（错误码 " + std::to_string(::GetLastError()) + "）";
            return false;
        }
        read_total += done;
    }
    if (verify != bytes) {
        ::CloseHandle(reader);
        ::DeleteFileW(path.c_str());
        *reason = "快照读回内容与原始 A2L 不一致";
        return false;
    }
    ::SetFilePointer(reader, 0, nullptr, FILE_BEGIN);
    // 守卫释放时先关闭只读句柄，再删除快照：SDK 解析期间路径始终有效，
    // 解析结束后不留残余副本（快照可能包含保密标定数据）。
    const std::wstring owned_path = path.native();
    *guard = std::shared_ptr<void>(reader, [owned_path](void* handle) {
        ::CloseHandle(static_cast<HANDLE>(handle));
        ::DeleteFileW(owned_path.c_str());
    });
    return true;
}

}  // namespace

/// @brief 校验根目录及其全部已完成解析的祖先（Windows：按句柄读 ACL）。
bool CheckRootChain(const std::filesystem::path& root, std::string* reason) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(root, ec);
    if (ec) {
        *reason = "无法取得快照目录的绝对路径：" + root.string();
        return false;
    }
    std::filesystem::path current = std::filesystem::weakly_canonical(absolute, ec);
    if (ec) current = absolute;
    // 第一轮是快照目录本身：任何非可信写入都不可接受；
    // 之后是祖先：只关心"能否删除/替换本路径"。
    bool is_snapshot_dir = true;
    while (!current.empty()) {
        AllowedSids allowed;
        if (!BuildAllowedSids(allowed)) {
            *reason = "无法取得当前用户 SID，无法校验目录权限";
            return false;
        }
        if (!CheckDirectoryByHandle(
                current, allowed,
                is_snapshot_dir ? kWriteLikeMask : kAncestorForbiddenMask,
                // 快照目录里的文件会继承它的 ACE，必须把"仅继承"项也算进来；
                // 祖先目录只需阻止他人删除/替换本路径，继承项不影响该判定。
                is_snapshot_dir, reason)) {
            return false;
        }
        is_snapshot_dir = false;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return true;
}

bool StageFile(const std::filesystem::path& path, const std::string& bytes,
               std::shared_ptr<void>* out_guard, std::string* reason) {
    return StageFileImpl(path, bytes, out_guard, reason);
}

}  // namespace calmcar::xcp::detail::fs
