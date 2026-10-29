/**
 * @file trusted_snapshot_posix.cpp
 * @brief 受控 A2L 快照的 POSIX 原生实现（stat/fstat 权限校验 + 独占创建）。
 *
 * @warning 本分支尚未校验挂载选项与服务端语义（FUSE/NFS 无法从本地证明），
 *          因此其可信度只在本机常见本地文件系统成立；本轮只做了 Windows 实测。
 */

#include "trusted_snapshot_platform.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

namespace calmcar::xcp::detail::fs {

/// @brief 取当前可执行文件所在目录。
std::filesystem::path ExecutableDirectory() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::path(buffer.c_str()).parent_path();
#else
    std::string buffer(1024, '\0');
    const auto len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0) return {};
    buffer.resize(static_cast<std::size_t>(len));
    return std::filesystem::path(buffer).parent_path();
#endif
}

namespace {

/**
 * @brief 检查目录链：拒绝符号链接、拒绝组/其他身份可写、拒绝非本人且非 root 属主。
 * @param dir 目录路径。
 * @param reason 失败原因。
 * @return 是否通过。
 */
inline bool CheckDirectoryPosix(const std::filesystem::path& dir,
                                std::string* reason) {
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0) {
        *reason = "无法读取目录状态：" + dir.string();
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        *reason = "目录是符号链接，快照路径无法证明：" + dir.string();
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        *reason = "路径不是目录：" + dir.string();
        return false;
    }
    const auto euid = ::geteuid();
    if (st.st_uid != euid && st.st_uid != 0) {
        *reason = "目录属主既非本人也非 root：" + dir.string();
        return false;
    }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        *reason = "目录对组或其他身份可写：" + dir.string();
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
    const int writer = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (writer < 0) {
        *reason = "无法独占创建快照文件（errno " + std::to_string(errno) + "）";
        return false;
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto done = ::write(writer, bytes.data() + written,
                                  bytes.size() - written);
        if (done <= 0) {
            ::close(writer);
            ::unlink(path.c_str());
            *reason = "写入快照失败（errno " + std::to_string(errno) + "）";
            return false;
        }
        written += static_cast<std::size_t>(done);
    }
    ::fsync(writer);
    ::close(writer);

    const int reader = ::open(path.c_str(), O_RDONLY);
    if (reader < 0) {
        ::unlink(path.c_str());
        *reason = "无法以只读方式打开快照（errno " + std::to_string(errno) + "）";
        return false;
    }
    struct stat st{};
    if (::fstat(reader, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != ::geteuid()) {
        ::close(reader);
        ::unlink(path.c_str());
        *reason = "快照文件身份不可证明";
        return false;
    }
    std::string verify(bytes.size(), '\0');
    std::size_t read_total = 0;
    while (read_total < verify.size()) {
        const auto done = ::read(reader, verify.data() + read_total,
                                 verify.size() - read_total);
        if (done <= 0) {
            ::close(reader);
            ::unlink(path.c_str());
            *reason = "读回快照失败（errno " + std::to_string(errno) + "）";
            return false;
        }
        read_total += static_cast<std::size_t>(done);
    }
    if (verify != bytes) {
        ::close(reader);
        ::unlink(path.c_str());
        *reason = "快照读回内容与原始 A2L 不一致";
        return false;
    }
    ::lseek(reader, 0, SEEK_SET);
    // 守卫释放时关闭句柄并删除快照，避免解析结束后留下副本。
    const std::string owned_path = path.string();
    *guard = std::shared_ptr<void>(
        reinterpret_cast<void*>(static_cast<intptr_t>(reader)),
        [owned_path](void* handle) {
            ::close(static_cast<int>(reinterpret_cast<intptr_t>(handle)));
            ::unlink(owned_path.c_str());
        });
    return true;
}

}  // namespace

/// @brief 校验根目录及其全部已完成解析的祖先（POSIX：逐级 stat 权限位）。
bool CheckRootChain(const std::filesystem::path& root, std::string* reason) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(root, ec);
    if (ec) {
        *reason = "无法取得快照目录的绝对路径：" + root.string();
        return false;
    }
    std::filesystem::path current = std::filesystem::weakly_canonical(absolute, ec);
    if (ec) current = absolute;
    // 逐级向上：POSIX 下同一套检查对快照目录与祖先都适用（拒绝符号链接、
    // 拒绝组/其他身份可写、拒绝非本人且非 root 属主）。
    while (!current.empty()) {
        if (!CheckDirectoryPosix(current, reason)) return false;
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
