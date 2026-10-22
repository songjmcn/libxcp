/**
 * @file version_resolver.hpp
 * @brief 协议层与传输层版本的集中裁决器（批次 B1）。
 *
 * @details 本文件实现方案 §4.2「版本选择规则」的单一裁决点。设计约束来自
 *          方案 §1 第 1 条：**集中版本裁决**——新增单一 Version Resolver，
 *          由它生成不可变的版本快照；Codec、Parser、DAQ、MemoryAccess
 *          **不得各自猜版本**。
 *
 * @par 三条铁律
 *      1. **PL 与 TL 是两个独立维度**，禁止互相推导（方案 §4 第 3 条）。
 *      2. **不得伪造精确版本**：Minor 未确认时必须表达为 `MajorOnly` +
 *         `1.x-conservative`，而不是假定一个 Minor 值（方案 D1）。
 *      3. **超时不等于老版本**：GET_VERSION 无响应只表示"未确认"，不得据此
 *         推断对端为 V1.3（方案 §5 批次 B1：「不得以 timeout 冒充老版本」）。
 */

#ifndef LIBXCP_VERSION_RESOLVER_HPP
#define LIBXCP_VERSION_RESOLVER_HPP

#include <optional>

#include "libxcp/command_executor.hpp"
#include "libxcp/protocol_version.hpp"

namespace calmcar::xcp {

/// @brief 版本裁决器（无状态，纯函数式）
///
/// @details 刻意做成无状态：输入（CONNECT 主版本 + GET_VERSION 探测结果 +
///          配置 + A2L 声明）→ 输出（两份不可变快照）。这样裁决逻辑可被
///          独立单测穷举，不需要构造完整会话。
class VersionResolver {
public:
    /// @brief 版本裁决的输入集合
    struct Inputs {
        /// @brief CONNECT 响应给出的协议层主版本（原始 PDF §3.1.1：只有主版本）
        std::uint8_t connect_protocol_major{0};

        /// @brief CONNECT 响应给出的传输层主版本（同上，只有主版本）
        std::uint8_t connect_transport_major{0};

        /// @brief GET_VERSION 探测结果（未探测或不可用则 `outcome` 非 Available）
        CommandExecutor::GetVersionProbe probe{};

        /// @brief 调用方显式版本策略
        VersionPolicy policy{};

        /// @brief 可信 A2L 声明的协议层版本（未提供则 nullopt）
        /// @details 方案 §4.2：A2L 是静态证据，弱于运行时；与 GET_VERSION
        ///          冲突时以运行时为裁决，但须记录冲突。
        std::optional<XcpProtocolVersion> a2l_protocol_version;

        /// @brief 可信 A2L 声明的传输层版本
        std::optional<XcpProtocolVersion> a2l_transport_version;
    };

    /// @brief 裁决结果（两份独立快照）
    struct Result {
        /// @brief 协议层版本快照
        ProtocolVersionSnapshot protocol{};

        /// @brief 传输层版本快照
        TransportVersionSnapshot transport{};

        /// @brief 裁决过程中发现的冲突描述（空表示无冲突）
        /// @details 只记录不改写——冲突须让调用方可见（方案 §4.2：
        ///          「记录冲突；影响布局时阻断相应功能」）。
        std::vector<std::string> conflicts;

        /// @brief GET_VERSION 是否可用（决定是否进入保守状态的主要原因之一）
        bool get_version_available{false};
    };

    /// @brief 执行裁决
    /// @param inputs 输入集合
    /// @return 不可变的双快照结果
    [[nodiscard]] static Result Resolve(const Inputs& inputs);

private:
    /// @brief 裁决单个层（PL 或 TL）的证据与生效版本
    /// @param layer_name 层名称（"协议层"/"传输层"），用于冲突描述
    /// @param connect_major CONNECT 给出的该层主版本
    /// @param probe_version GET_VERSION 给出的该层版本（不可用则 nullopt）
    /// @param configured 调用方显式配置的该层版本
    /// @param a2l A2L 声明的该层版本
    /// @param support_cap 该层支持上限
    /// @param conflicts 冲突描述输出（追加）
    /// @return 该层的证据
    [[nodiscard]] static VersionEvidence ResolveLayer(
        const std::string& layer_name, std::uint8_t connect_major,
        std::optional<XcpProtocolVersion> probe_version,
        std::optional<XcpProtocolVersion> configured,
        std::optional<XcpProtocolVersion> a2l,
        XcpProtocolVersion support_cap, std::vector<std::string>& conflicts);
};

}  // namespace calmcar::xcp

#endif  // LIBXCP_VERSION_RESOLVER_HPP