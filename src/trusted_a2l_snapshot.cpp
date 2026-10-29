/**
 * @file trusted_a2l_snapshot.cpp
 * @brief 受控 A2L 快照：平台无关的编排（见 `libxcp/trusted_a2l_snapshot.hpp`）。
 *
 * @details 原生部分（可执行文件目录、目录链校验、独占创建与只读守卫）位于
 *          `src/detail/fs/trusted_snapshot_win.cpp` 与
 *          `src/detail/fs/trusted_snapshot_posix.cpp`；本文件只负责
 *          "读源文件 → 确定目录 → 逐轮尝试唯一文件名 → 汇总结论"。
 */

#include "libxcp/trusted_a2l_snapshot.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>

#include "detail/fs/trusted_snapshot_platform.hpp"

namespace calmcar::xcp {
namespace {

/**
 * @brief 生成快照文件名（时间戳 + 随机数），避免与并发实例冲突。
 * @return 仅含 ASCII 的候选文件名。
 */
inline std::string SnapshotFileName() {
    std::random_device device;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::uint64_t mix = static_cast<std::uint64_t>(now) ^
                        (static_cast<std::uint64_t>(device()) << 32U);
    std::string hex;
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        hex.push_back(kDigits[mix & 0xFU]);
        mix >>= 4U;
    }
    return "a2l-snapshot-" + hex + ".a2l";
}

}  // namespace

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
    const std::filesystem::path& source, const std::filesystem::path& root) {
    TrustedA2lSnapshot result;
    std::ifstream input(source, std::ios::binary);
    if (!input.is_open()) {
        result.reason = "无法打开原始 A2L：" + source.string();
        return result;
    }
    result.bytes.assign(std::istreambuf_iterator<char>(input), {});
    input.close();
    if (result.bytes.empty()) {
        result.reason = "原始 A2L 为空：" + source.string();
        return result;
    }

    std::filesystem::path directory = root;
    if (directory.empty()) {
        const auto exe_dir = detail::fs::ExecutableDirectory();
        if (exe_dir.empty()) {
            result.reason = "无法确定可执行文件目录，无法建立可信快照";
            return result;
        }
        directory = exe_dir / "tmp";
    }
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        result.reason = "无法创建快照目录：" + directory.string() + "（" +
                        ec.message() + "）";
        return result;
    }
    if (!detail::fs::CheckRootChain(directory, &result.reason)) return result;

    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto candidate = directory / SnapshotFileName();
        if (std::filesystem::exists(candidate, ec)) continue;
        std::string stage_reason;
        if (!detail::fs::StageFile(candidate, result.bytes, &result.guard,
                                   &stage_reason)) {
            result.reason = stage_reason;
            return result;
        }
        result.trusted = true;
        result.path = candidate;
        result.reason = "快照已写入并读回校验：" + candidate.string();
        return result;
    }
    result.reason = "无法在快照目录中创建唯一文件：" + directory.string();
    return result;
}

}  // namespace calmcar::xcp
