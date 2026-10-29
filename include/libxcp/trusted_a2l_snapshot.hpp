#pragma once

// 受控 A2L 快照：把原始 A2L 复制到"只有本人（及 SYSTEM/管理员）可写"的目录，
// 并在整个 SDK 解析期间持有只读句柄，使其他身份无法在解析前后替换内容。
//
// 设计边界（已与用户确认）：
// - 快照根目录默认取"可执行文件所在目录下的 tmp"，也可由
//   `--a2l-trusted-snapshot-root` 显式指定；默认目录并不天然可信，仍需校验。
// - 校验范围包括根目录及其全部祖先：拒绝重解析点（Windows）与符号链接（POSIX）、
//   拒绝其他身份可写、文件系统与权限无法证明时一律失败关闭。
// - 不防护：管理员、拥有备份/还原等 ACL 绕过特权的账户、以及与本人同身份运行的
//   恶意进程；这些属于设计已声明的范围之外。
// - POSIX 侧尚未校验挂载选项（如 noexec/nodev 与之无关，但 FUSE/NFS 等服务端
//   语义无法从本地证明），因此该分支的可信度只在本机常见本地文件系统成立。

/**
 * @file trusted_a2l_snapshot.hpp
 * @brief 受控 A2L 快照：把原始 A2L 复制到"只有本人可写"的目录并在解析期持有只读句柄。
 *
 * @details 本组件原为 `tools/xcp_master/a2l_trusted_snapshot.hpp`（header-only），
 *          第二阶段提取为库组件。实现细节（ACL/重解析点/挂载检查）全部在
 *          `src/trusted_a2l_snapshot.cpp`，公共头只暴露结果结构与入口。
 */

#include <filesystem>
#include <memory>
#include <string>

namespace calmcar::xcp {

/// @brief 受控快照的结果。
struct TrustedA2lSnapshot {
    bool trusted{false};                ///< 是否可作为可信原文。
    std::filesystem::path path;         ///< 快照文件路径（仅在可信时有效）。
    std::string bytes;                  ///< 读回并核对过的原始字节。
    std::string reason;                 ///< 结论原因（中文，失败时说明拒绝点）。
    std::shared_ptr<void> guard;        ///< 只读守卫句柄，须存活到 SDK 解析结束。
};

/**
 * @brief 把原始 A2L 复制到受控目录并验证读回内容。
 * @param source 原始 A2L 路径。
 * @param root 受控根目录；为空时取"可执行文件目录/tmp"。
 * @return 结果；`trusted` 为假时 `reason` 给出拒绝点，调用方必须回落到显式配置。
 *
 * @details 步骤：校验/创建根目录及其祖先 → 独占创建 → 全量写入并刷盘 →
 *          以只读句柄读回逐字节比对 → 返回守卫句柄。任何一步无法证明都失败关闭。
 */
[[nodiscard]] TrustedA2lSnapshot StageTrustedA2lSnapshot(
    const std::filesystem::path& source, const std::filesystem::path& root);

}  // namespace calmcar::xcp
