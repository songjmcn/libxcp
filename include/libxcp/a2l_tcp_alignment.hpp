#pragma once

/**
 * @file a2l_tcp_alignment.hpp
 * @brief 保守的 A2L 原文扫描器，以及与 SDK 端点的双源核对（公共库组件）。
 *
 * @details 本组件原为 `tools/xcp_master/a2l_tcp_alignment_candidate.hpp`
 *          （header-only），第二阶段把它提取为库组件，使库外调用方无需依赖
 *          `tools/` 的包含路径即可复用：
 *          1. 在**受限且失败即拒绝**的词法/结构子集内，从 A2L 原文定位唯一的
 *             `XCP_ON_TCP_IP` 块并读出其字段；
 *          2. 把该结果与 SDK 解析出的 TCP 端点逐字段核对，两者一致才允许把
 *             `PACKET_ALIGNMENT` 当作对齐证据。
 *
 * @par 为什么必须双源核对
 *      a2llib 的外层词法器会把未匹配字符静默交给默认规则（`thirdparty/a2llib/src/a2lflexer.l`
 *      末尾的 `.` 分支），而 `IF_DATA` 原始文本还会被另一套词法器重新切分、
 *      非法 `PACKET_ALIGNMENT` 枚举会静默保留默认值。因此**任何单一来源**
 *      （原文扫描或 SDK 字段）都不足以成为对齐证据。两源不一致时本组件一律
 *      返回不信任，方向是安全的。
 *
 * @par 一致性边界
 *      首次实现仍分两次读取文件（SDK 加载一次、扫描器读一次），没有可证明的
 *      同一快照。因此本组件把"两源必须一致"当作安全网：并发修改若造成两源
 *      分歧会被检出并拒绝；若有人能同时改写两源使其一致，等价于该文件确实
 *      声明了那个值，这一点与命令行被改写同级，超出本组件威胁模型。
 *
 * @warning 本组件只覆盖下文列出的受限子集，超集输入一律拒绝；被拒绝的输入
 *          不产生任何证据，调用方必须要求用户显式指定对齐。
 */
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace calmcar::xcp {

/// @brief 单个候选 TCP 端点；字段仅为扫描结果，尚需与 SDK 结果核对。
struct A2lTcpCandidate {
    std::uint64_t version{};                ///< 该块首个数值（传输层版本原值）。
    std::uint16_t port{};                   ///< 端口。
    std::optional<std::uint8_t> alignment;  ///< 显式位宽；缺失不代表默认值。
    std::optional<std::string> host;        ///< `HOST_NAME` 原值。
    std::optional<std::string> address;     ///< `ADDRESS` 原值。
};

/// @brief 读取 `/include` 目标内容的回调；返回空表示无法读取。
using IncludeResolver =
    std::function<std::optional<std::string>(const std::string& path)>;


/**
 * @brief 在受限子集内定位唯一 `XCP_ON_TCP_IP` 块。
 * @param bytes 调用方读取的完整原始字节；函数自身不读文件。
 * @param resolver `/include` 解析回调；为空时任何 `/include` 都导致拒绝。
 * @return 无候选表示输入不在子集内或结构上有歧义。
 *
 * @note 允许同一 `MODULE` 下存在多个 `IF_DATA XCP`（真实 A2L 常把
 *       `SEGMENT`/`CHECKSUM` 与 `PROTOCOL_LAYER`/`DAQ`/`TCP` 分成两块），
 *       也允许 `INSTANCE` 等对象上的 `IF_DATA XCP`，只要它们不含 TCP 端点块；
 *       这正是"端点声明唯一"这一更强不变量的正确表述。
 */
[[nodiscard]] std::optional<A2lTcpCandidate> ScanA2lTcpCandidate(
    std::string_view bytes, const IncludeResolver& resolver = {});

/// @brief 双源核对结论。
enum class A2lAlignmentTrust : std::uint8_t {
    kTrusted = 0,  ///< 原文与 SDK 一致，可作为 alignment 证据。
    kNotDeclared,  ///< 原文确实没有声明，不得用默认值冒充。
    kUntrusted,    ///< 两源不一致或 SDK 结果不足，禁止作为证据。
};

/// @brief SDK 侧读到的单个 TCP 端点事实。
struct SdkTcpEndpointFacts {
    std::string host;                       ///< 端点主机名/地址。
    std::uint16_t port{};                   ///< 端点端口。
    std::uint64_t version{};                ///< 该端点声明的 TL 版本原值。
    std::optional<std::uint8_t> alignment;  ///< SDK 认为声明的位宽。
    bool alignment_declared{false};         ///< SDK 是否认为关键字存在。
};

/// @brief 核对结论及其原因。
struct A2lAlignmentVerdict {
    A2lAlignmentTrust trust{A2lAlignmentTrust::kUntrusted};  ///< 结论。
    std::optional<std::uint8_t> bits;                        ///< 信任时的位宽。
    std::string reason;                                      ///< 中文原因。
};

/**
 * @brief 用 SDK 解析结果核对原文扫描结果。
 * @param scan 原文扫描结果。
 * @param endpoints SDK 读到的全部 TCP 端点。
 * @param plus_flags 每个端点是否来自 `XCPplus` 块（当前一律拒绝）。
 * @return 核对结论；只有端口、版本、位宽、地址全部一致才可能返回 `kTrusted`。
 */
[[nodiscard]] A2lAlignmentVerdict CrossCheckA2lTcpAlignment(
    const A2lTcpCandidate& scan,
    const std::vector<SdkTcpEndpointFacts>& endpoints,
    const std::vector<bool>& plus_flags);

}  // namespace calmcar::xcp
