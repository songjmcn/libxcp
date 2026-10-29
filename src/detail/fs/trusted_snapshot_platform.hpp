#pragma once

/**
 * @file trusted_snapshot_platform.hpp
 * @brief 受控 A2L 快照的平台原生入口（内部头，不安装、不对外暴露）。
 *
 * @details 原生实现只有两份：`trusted_snapshot_win.cpp`（ACL 句柄校验）与
 *          `trusted_snapshot_posix.cpp`（stat/fstat 权限校验），由 CMake 按平台
 *          二选一编译；平台无关的编排逻辑在 `src/trusted_a2l_snapshot.cpp`。
 *
 * @note 之所以拆文件而不是用 `#if`：仓库的 `tests/check_socket_platform_boundary.py`
 *       只豁免显式列出的原生实现文件，`src/` 其余文件禁止出现 `_WIN32` 与
 *       windows.h/fcntl.h/sys/types.h。拆开后公共层仍然可被审计。
 */

#include <filesystem>
#include <memory>
#include <string>

namespace calmcar::xcp::detail::fs {

/// @brief 取当前可执行文件所在目录；无法取得时返回空路径。
[[nodiscard]] std::filesystem::path ExecutableDirectory();

/**
 * @brief 校验快照根目录及其全部已完成解析的祖先。
 * @param root 根目录（须已存在）。
 * @param reason 失败原因（中文）。
 * @return 是否通过。
 * @details 快照目录本身拒绝任何非可信写入；祖先只关心"能否删除/替换本路径"。
 */
[[nodiscard]] bool CheckRootChain(const std::filesystem::path& root,
                                  std::string* reason);

/**
 * @brief 独占创建快照文件、写入并刷盘，再以只读句柄读回校验。
 * @param path 目标路径（调用方保证此前不存在）。
 * @param bytes 待写入的原始字节。
 * @param out_guard 成功时接管只读守卫句柄（析构时关闭并删除）。
 * @param reason 失败原因（中文）。
 * @return 是否成功。
 */
[[nodiscard]] bool StageFile(const std::filesystem::path& path,
                             const std::string& bytes,
                             std::shared_ptr<void>* out_guard,
                             std::string* reason);

}  // namespace calmcar::xcp::detail::fs
