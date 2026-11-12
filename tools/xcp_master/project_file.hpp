#pragma once

/**
 * @file project_file.hpp
 * @brief xcp_master 工程配置文件（JSON）的数据模型、序列化与原子写入。
 *
 * @details 本文件只包含**纯函数与 POD 结构**，不依赖 FTXUI、不依赖 Socket、
 *          不依赖 `Options`（避免与 `main.cpp` 循环包含），因此可以在
 *          `XcpMasterSymbolPolicyTest` 中不经终端直接单测。
 *
 *          配置文件的定位是"复现一次做好的连接与观测/标定上下文"：
 *          - 连接字段保存本次**生效**的传输选择，而非用户原始输入；
 *          - `observe` / `calibrate` 一律保存 `module::name` 规范名；
 *          - 加载本文件**绝不**触发任何 XCP 写操作。
 *
 * @note 使用 vendor 入库的 nlohmann/json（`include/vendor/nlohmann_json`），
 *       不使用 FetchContent，也不包含任何网络获取动作。
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
// 只取 GetModuleFileNameW：必须屏蔽 windows.h 的 min/max 宏，
// 否则包含本头文件的 TU（例如 main.cpp）里所有 std::min/std::max 都会被
// 宏展开成语法错误；同时收敛掉本模块用不到的 Win32 声明面。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "cli_selection.hpp"
#include "libxcp/protocol_version.hpp"
#include "libxcp/tcp_wire_profile.hpp"
#include "libxcp/transport_selection.hpp"

namespace calmcar::xcp::tools {

/**
 * @brief 本模块使用的 JSON 类型别名
 * @details 使用 `ordered_json` 而非默认的 `json`：默认类型按字母序排列对象键，
 *          生成的配置文件中 `a2l_path` 会排到 `calibrate` 之后，既与文档样例
 *          不符，也不利于人工阅读与 diff。`ordered_json` 保持插入顺序。
 */
using ProjectJson = nlohmann::ordered_json;

/// @brief 工程配置文件格式版本；不匹配时整体拒绝加载（fail-closed）。
inline constexpr int kProjectFileSchemaVersion = 1;

/// @brief 工程配置文件最大字节数；超过即拒绝，防止恶意的超大输入。
inline constexpr std::uintmax_t kProjectFileMaxBytes = 1024U * 1024U;

/// @brief 工程配置文件缺省后缀
inline constexpr std::string_view kProjectFileExtension = ".json";

/**
 * @brief 工程文件中的连接配置
 *
 * @details 对应首次命令行的 `--transport` / `--host` / `--port` /
 *          `--tcp-profile` 等参数，但记录的是**解析后的生效值**。
 */
struct ProjectConnection {
    /// @brief 传输层类别：`"udp"` 或 `"tcp"`
    std::string transport{"udp"};

    /// @brief 目标主机 IPv4 地址；TCP 端点来自 A2L 时可为空
    std::string host;

    /// @brief 目标端口（1..65535）；TCP 端点来自 A2L 时可为 0
    std::uint16_t port{0};

    /// @brief TCP 端点（host/port）是否由 A2L 的 `XCP_ON_TCP_IP` 提供
    bool endpoint_from_a2l{false};

    /// @brief `"asam-v1.5"` / `"asam-v1.4"` / `"auto"` / `"auto-legacy"`
    /// @details UDP 传输下为空字符串，表示不适用。
    std::string tcp_profile;

    /// @brief `--packet-alignment` 的位宽（8/16/32）；未声明为空
    std::optional<std::uint8_t> packet_alignment_bits;

    /// @brief 协议层版本（形如 `"1.2"`）；未声明为空
    std::optional<std::string> protocol_version;

    /// @brief 传输层版本（形如 `"1.4"`）；未声明为空
    std::optional<std::string> transport_version;

    /// @brief 是否由 `--preset xcplite` 展开而来；未使用预设为空
    std::optional<bool> preset_xcplite;

    /// @brief 受控 A2L 快照根目录；为空表示使用缺省 `<exe 目录>/tmp`
    std::optional<std::string> a2l_snapshot_root;
};

/**
 * @brief 工程文件中的会话配置
 *
 * @details 对应 `--mode` / `--interval-ms` / `--module`。
 */
struct ProjectSession {
    /// @brief `"poll"` 或 `"daq"`
    std::string mode{"poll"};

    /// @brief poll 模式的目标周期（毫秒）；daq 模式下无意义，保存为空
    std::optional<std::uint64_t> interval_ms;

    /// @brief A2L 模块过滤；不过滤时为空
    std::optional<std::string> module;
};

/**
 * @brief 工程配置文件根模型
 */
struct ProjectFile {
    /// @brief 格式版本号，必须等于 `kProjectFileSchemaVersion`
    int schema_version{kProjectFileSchemaVersion};

    /// @brief A2L 文件绝对路径
    std::string a2l_path;

    /// @brief 连接配置
    ProjectConnection connection;

    /// @brief 会话配置
    ProjectSession session;

    /// @brief 待观测变量规范名（`module::name`）
    std::vector<std::string> observe;

    /// @brief 待标定变量规范名（`module::name`）；只是名字白名单，不含目标值
    std::vector<std::string> calibrate;
};

/**
 * @brief 运行时**实际生效**的连接参数
 *
 * @details 与 `TransportSelectionRequest`（用户输入）和
 *          `ResolvedTransportSelection`（解析结果）都不同：本结构记录的是
 *          连上 ECU 之后**真正在用**的那一组值——
 *          - Wire Profile：`--tcp-profile auto` 时是嗅探胜出的真实布局，
 *            而不是 "auto" 这个"还没定下来"的选择；
 *          - 对齐：嗅探胜出候选实际携带的位宽；
 *          - 协议层/传输层版本：`XcpMaster::GetProtocolVersionSnapshot()` 与
 *            `GetTransportVersionSnapshot()` 的 `effective` 值。
 *
 *          工程配置文件的 `connection` 段保存的是本结构，这样 `save` 之后
 *          用 `-c` 重新加载能直接复现同一次运行，不需要再嗅探一遍。
 *
 * @note `protocol_version` / `transport_version` 为空的含义是"运行时没有
 *       取得权威证据"。此时**必须**写 null 而不是编造一个值：加载时会回到
 *       CONNECT 协商路径重新取证，编造值反而会让版本裁决基于错误前提。
 */
struct EffectiveConnection {
    /// @brief 传输层类别：`"udp"` 或 `"tcp"`
    std::string transport{"udp"};

    /// @brief 生效的目标主机；TCP 端点来自 A2L 时可为空
    std::string host;

    /// @brief 生效的目标端口；由 A2L 提供时可为 0
    std::uint16_t port{0};

    /// @brief 端点（host/port）是否由 A2L 的 `XCP_ON_*_IP` 提供
    bool endpoint_from_a2l{false};

    /// @brief 生效的 Wire Profile：`"asam-v1.5"` / `"asam-v1.4"` / 空（UDP）
    std::string tcp_profile;

    /// @brief 生效的 PACKET_ALIGNMENT 位宽（8/16/32）；未知为空
    /// @details Wire Profile 具有固定含义时也照实写出（asam-v1.4 恒 8），
    ///          因为加载端据此构造的请求与运行期一致。
    std::optional<std::uint8_t> packet_alignment_bits;

    /// @brief 生效的协议层版本（形如 `"1.3"`）；无权威证据为空
    std::optional<std::string> protocol_version;

    /// @brief 生效的传输层版本（形如 `"1.4"`）；无权威证据为空
    std::optional<std::string> transport_version;

    /// @brief 是否由 `--preset xcplite` 展开而来；未使用预设为空
    std::optional<bool> preset_xcplite;

    /// @brief 受控 A2L 快照根目录；为空表示使用缺省 `<exe 目录>/tmp`
    std::optional<std::string> a2l_snapshot_root;

    /// @brief 版本与对齐的取值说明，供人工核对（不写入 JSON）
    /// @details 例如 `"Profile=auto→asam-v1.5"`、`"PL 来自 GET_VERSION"`。
    std::vector<std::string> notes;
};

namespace detail {

/**
 * @brief 把 `std::uint16_t` 转成 JSON 整数
 * @param value 待转换的数值
 * @return JSON 整数节点
 */
[[nodiscard]] inline ProjectJson ToJsonUnsigned(std::uint16_t value) {
    return ProjectJson(static_cast<std::uint64_t>(value));
}

/**
 * @brief 把可选的字符串转为 JSON：空值写 `null`
 * @param value 可选的字符串
 * @return JSON 字符串或 `null`
 */
[[nodiscard]] inline ProjectJson ToJsonOptional(
    const std::optional<std::string>& value) {
    if (!value.has_value()) {
        return ProjectJson(nullptr);
    }
    return ProjectJson(*value);
}

/**
 * @brief 把可选的无符号数转为 JSON：空值写 `null`
 * @tparam T 无符号整型
 * @param value 可选数值
 * @return JSON 整数或 `null`
 */
template <typename T>
[[nodiscard]] inline ProjectJson ToJsonOptionalUnsigned(
    const std::optional<T>& value) {
    if (!value.has_value()) {
        return ProjectJson(nullptr);
    }
    return ProjectJson(static_cast<std::uint64_t>(*value));
}

/**
 * @brief 把可选的布尔转为 JSON：空值写 `null`
 * @param value 可选布尔
 * @return JSON 布尔或 `null`
 */
[[nodiscard]] inline ProjectJson ToJsonOptionalBool(
    const std::optional<bool>& value) {
    if (!value.has_value()) {
        return ProjectJson(nullptr);
    }
    return ProjectJson(*value);
}

/**
 * @brief 检查 JSON 对象是否只含有白名单内的一批键
 * @param node 待检查的 JSON 对象
 * @param allowed 允许出现的键集合
 * @param path 出错时用于定位的字段路径，如 `"connection"`
 * @param error 失败时写入中文错误文本
 * @return true 表示没有未知键
 */
[[nodiscard]] inline bool RequireOnlyKeys(
    const ProjectJson& node,
    std::initializer_list<std::string_view> allowed,
    std::string_view path,
    std::string& error) {
    for (auto it = node.begin(); it != node.end(); ++it) {
        bool known = false;
        for (const auto key : allowed) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            error = "未知字段 ";
            if (!path.empty()) {
                error += std::string(path);
                error += '.';
            }
            error += it.key();
            return false;
        }
    }
    return true;
}

/**
 * @brief 读取一个必填字符串字段
 * @param node 父对象
 * @param key 字段名
 * @param path 字段路径（用于错误信息）
 * @param out 成功时写入取值
 * @param error 失败时写入中文错误文本
 * @return true 表示成功
 */
[[nodiscard]] inline bool ReadRequiredString(
    const ProjectJson& node,
    std::string_view key,
    std::string_view path,
    std::string& out,
    std::string& error) {
    const auto it = node.find(std::string(key));
    if (it == node.end()) {
        error = "缺少字段 " + std::string(path);
        return false;
    }
    if (!it->is_string()) {
        error = "字段 " + std::string(path) + " 期望字符串";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

/**
 * @brief 读取一个可选字符串字段（`null` 或缺省都表示无值）
 * @param node 父对象
 * @param key 字段名
 * @param path 字段路径（用于错误信息）
 * @param out 成功时写入取值
 * @param error 失败时写入中文错误文本
 * @return true 表示成功
 */
[[nodiscard]] inline bool ReadOptionalString(
    const ProjectJson& node,
    std::string_view key,
    std::string_view path,
    std::optional<std::string>& out,
    std::string& error) {
    const auto it = node.find(std::string(key));
    if (it == node.end() || it->is_null()) {
        out.reset();
        return true;
    }
    if (!it->is_string()) {
        error = "字段 " + std::string(path) + " 期望字符串或 null";
        return false;
    }
    out = it->get<std::string>();
    return true;
}

/**
 * @brief 读取一个可选布尔字段（`null` 或缺省都表示无值）
 * @param node 父对象
 * @param key 字段名
 * @param path 字段路径（用于错误信息）
 * @param out 成功时写入取值
 * @param error 失败时写入中文错误文本
 * @return true 表示成功
 */
[[nodiscard]] inline bool ReadOptionalBool(
    const ProjectJson& node,
    std::string_view key,
    std::string_view path,
    std::optional<bool>& out,
    std::string& error) {
    const auto it = node.find(std::string(key));
    if (it == node.end() || it->is_null()) {
        out.reset();
        return true;
    }
    if (!it->is_boolean()) {
        error = "字段 " + std::string(path) + " 期望布尔或 null";
        return false;
    }
    out = it->get<bool>();
    return true;
}

/**
 * @brief 把 `classify()` 返回的字节偏移换算为"第 N 行第 M 列"
 * @param text 原始文本
 * @param byte_index 字节偏移（0 起）
 * @return 形如 `"第 3 行第 5 列"` 的定位文本
 */
[[nodiscard]] inline std::string DescribePosition(std::string_view text,
                                                 std::size_t byte_index) {
    const std::size_t limit = byte_index > text.size() ? text.size() : byte_index;
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t i = 0; i < limit; ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return "第 " + std::to_string(line) + " 行第 " + std::to_string(column) +
           " 列";
}

}  // namespace detail

/**
 * @brief 把工程配置模型序列化为 JSON 文本
 * @param file 配置模型
 * @return 带两空格缩进的 JSON 文本（末尾含换行）
 *
 * @details 所有字段**全部显式写出**（无值写 `null`），便于人工 diff 与评审；
 *          Windows 路径中的反斜杠由 nlohmann 负责转义。
 */
[[nodiscard]] inline std::string ProjectFileToJson(const ProjectFile& file) {
    ProjectJson root;
    root["schema_version"] = file.schema_version;
    root["a2l_path"] = file.a2l_path;

    ProjectJson connection;
    connection["transport"] = file.connection.transport;
    connection["host"] = file.connection.host;
    connection["port"] = detail::ToJsonUnsigned(file.connection.port);
    connection["endpoint_from_a2l"] = file.connection.endpoint_from_a2l;
    connection["tcp_profile"] = file.connection.tcp_profile;
    connection["packet_alignment_bits"] =
        detail::ToJsonOptionalUnsigned(file.connection.packet_alignment_bits);
    connection["protocol_version"] =
        detail::ToJsonOptional(file.connection.protocol_version);
    connection["transport_version"] =
        detail::ToJsonOptional(file.connection.transport_version);
    connection["preset_xcplite"] =
        detail::ToJsonOptionalBool(file.connection.preset_xcplite);
    connection["a2l_snapshot_root"] =
        detail::ToJsonOptional(file.connection.a2l_snapshot_root);
    root["connection"] = std::move(connection);

    ProjectJson session;
    session["mode"] = file.session.mode;
    session["interval_ms"] =
        detail::ToJsonOptionalUnsigned(file.session.interval_ms);
    session["module"] = detail::ToJsonOptional(file.session.module);
    root["session"] = std::move(session);

    root["observe"] = file.observe;
    root["calibrate"] = file.calibrate;

    return root.dump(2) + "\n";
}

/**
 * @brief 校验并解析工程配置 JSON 文本
 * @param text JSON 原文
 * @param out 成功时写入解析后的模型
 * @param error 失败时写入中文错误文本（含行列号）
 * @return true 表示解析成功
 *
 * @details 严格模式：
 *          - 语法错误由 nlohmann 拒绝（注释、尾随逗号、尾部垃圾均不合法）；
 *          - 版本号不符、缺字段、类型不符、未知字段一律整体拒绝，
 *            **不做"尽力而为"的部分加载**——半加载的配置比失败更难排查。
 *
 * @note 重复键采用 nlohmann 的"后者覆盖"语义（该库不提供重复键检测开关）。
 *       这是已知且被测试固定的行为，文档中已注明。
 */
[[nodiscard]] inline bool ParseProjectFileJson(std::string_view text,
                                               ProjectFile& out,
                                               std::string& error) {
    if (text.size() > kProjectFileMaxBytes) {
        error = "配置文件超过大小上限（1 MiB）";
        return false;
    }

    ProjectJson root;
    try {
        root = ProjectJson::parse(text);
    } catch (const nlohmann::ordered_json::parse_error& exception) {
        error = "JSON 语法错误：" + detail::DescribePosition(text, exception.byte) +
                "：" + exception.what();
        return false;
    } catch (const nlohmann::ordered_json::exception& exception) {
        error = std::string("JSON 解析失败：") + exception.what();
        return false;
    }

    if (!root.is_object()) {
        error = "配置文件顶层必须是 JSON 对象";
        return false;
    }

    if (!detail::RequireOnlyKeys(
            root,
            {"schema_version", "a2l_path", "connection", "session", "observe",
             "calibrate"},
            "", error)) {
        return false;
    }

    // 版本号：必须存在且严格等于当前格式版本。
    const auto version_it = root.find("schema_version");
    if (version_it == root.end()) {
        error = "缺少字段 schema_version";
        return false;
    }
    if (!version_it->is_number_integer()) {
        error = "字段 schema_version 期望整数";
        return false;
    }
    const auto version = version_it->get<std::int64_t>();
    if (version != kProjectFileSchemaVersion) {
        error = "不支持的 schema_version=" + std::to_string(version) +
                "（期望 " + std::to_string(kProjectFileSchemaVersion) + "）";
        return false;
    }

    ProjectFile parsed;
    parsed.schema_version = kProjectFileSchemaVersion;

    if (!detail::ReadRequiredString(root, "a2l_path", "a2l_path",
                                    parsed.a2l_path, error)) {
        return false;
    }
    if (parsed.a2l_path.empty()) {
        error = "字段 a2l_path 不能为空";
        return false;
    }

    // ---- connection ----
    const auto connection_it = root.find("connection");
    if (connection_it == root.end()) {
        error = "缺少字段 connection";
        return false;
    }
    if (!connection_it->is_object()) {
        error = "字段 connection 期望对象";
        return false;
    }
    const auto& connection = *connection_it;
    if (!detail::RequireOnlyKeys(
            connection,
            {"transport", "host", "port", "endpoint_from_a2l", "tcp_profile",
             "packet_alignment_bits", "protocol_version", "transport_version",
             "preset_xcplite", "a2l_snapshot_root"},
            "connection", error)) {
        return false;
    }
    if (!detail::ReadRequiredString(connection, "transport", "connection.transport",
                                    parsed.connection.transport, error)) {
        return false;
    }
    if (parsed.connection.transport != "udp" && parsed.connection.transport != "tcp") {
        error = "字段 connection.transport 只能是 udp 或 tcp，实际为 \"" +
                parsed.connection.transport + "\"";
        return false;
    }

    if (!detail::ReadRequiredString(connection, "host", "connection.host",
                                    parsed.connection.host, error)) {
        return false;
    }

    const auto port_it = connection.find("port");
    if (port_it == connection.end()) {
        error = "缺少字段 connection.port";
        return false;
    }
    if (!port_it->is_number_unsigned() && !port_it->is_number_integer()) {
        error = "字段 connection.port 期望整数";
        return false;
    }
    const auto port = port_it->get<std::int64_t>();
    if (port < 0 || port > 65535) {
        error = "字段 connection.port 超出范围 0..65535，实际为 " +
                std::to_string(port);
        return false;
    }
    parsed.connection.port = static_cast<std::uint16_t>(port);

    const auto from_a2l_it = connection.find("endpoint_from_a2l");
    if (from_a2l_it == connection.end()) {
        error = "缺少字段 connection.endpoint_from_a2l";
        return false;
    }
    if (!from_a2l_it->is_boolean()) {
        error = "字段 connection.endpoint_from_a2l 期望布尔";
        return false;
    }
    parsed.connection.endpoint_from_a2l = from_a2l_it->get<bool>();

    if (!detail::ReadRequiredString(connection, "tcp_profile",
                                    "connection.tcp_profile",
                                    parsed.connection.tcp_profile, error)) {
        return false;
    }
    // tcp_profile 的取值集合随传输层而变：UDP 下必须为空，TCP 下必须可识别。
    if (parsed.connection.transport == "udp") {
        if (!parsed.connection.tcp_profile.empty()) {
            error = "UDP 传输下 connection.tcp_profile 必须为空";
            return false;
        }
    } else {
        const bool known =
            parsed.connection.tcp_profile == "auto" ||
            parsed.connection.tcp_profile == "auto-legacy" ||
            ParseTcpWireProfileText(parsed.connection.tcp_profile).has_value();
        if (!known) {
            error = "字段 connection.tcp_profile 取值非法：\"" +
                    parsed.connection.tcp_profile + "\"";
            return false;
        }
    }

    const auto alignment_it = connection.find("packet_alignment_bits");
    if (alignment_it != connection.end() && !alignment_it->is_null()) {
        if (!alignment_it->is_number_unsigned() &&
            !alignment_it->is_number_integer()) {
            error = "字段 connection.packet_alignment_bits 期望整数或 null";
            return false;
        }
        const auto bits = alignment_it->get<std::int64_t>();
        if (bits != 8 && bits != 16 && bits != 32) {
            error = "字段 connection.packet_alignment_bits 只能是 8/16/32";
            return false;
        }
        parsed.connection.packet_alignment_bits =
            static_cast<std::uint8_t>(bits);
    }

    if (!detail::ReadOptionalString(connection, "protocol_version",
                                    "connection.protocol_version",
                                    parsed.connection.protocol_version, error)) {
        return false;
    }
    if (parsed.connection.protocol_version.has_value() &&
        !ParseVersionText(*parsed.connection.protocol_version).has_value()) {
        error = "字段 connection.protocol_version 格式非法，期望形如 \"1.2\"";
        return false;
    }
    if (!detail::ReadOptionalString(connection, "transport_version",
                                    "connection.transport_version",
                                    parsed.connection.transport_version, error)) {
        return false;
    }
    if (parsed.connection.transport_version.has_value() &&
        !ParseVersionText(*parsed.connection.transport_version).has_value()) {
        error = "字段 connection.transport_version 格式非法，期望形如 \"1.4\"";
        return false;
    }
    if (!detail::ReadOptionalBool(connection, "preset_xcplite",
                                  "connection.preset_xcplite",
                                  parsed.connection.preset_xcplite, error)) {
        return false;
    }
    if (!detail::ReadOptionalString(connection, "a2l_snapshot_root",
                                    "connection.a2l_snapshot_root",
                                    parsed.connection.a2l_snapshot_root, error)) {
        return false;
    }

    // ---- session ----
    const auto session_it = root.find("session");
    if (session_it == root.end()) {
        error = "缺少字段 session";
        return false;
    }
    if (!session_it->is_object()) {
        error = "字段 session 期望对象";
        return false;
    }
    const auto& session = *session_it;
    if (!detail::RequireOnlyKeys(session, {"mode", "interval_ms", "module"},
                                 "session", error)) {
        return false;
    }
    if (!detail::ReadRequiredString(session, "mode", "session.mode",
                                    parsed.session.mode, error)) {
        return false;
    }
    if (parsed.session.mode != "poll" && parsed.session.mode != "daq") {
        error = "字段 session.mode 只能是 poll 或 daq，实际为 \"" +
                parsed.session.mode + "\"";
        return false;
    }
    const auto interval_it = session.find("interval_ms");
    if (interval_it != session.end() && !interval_it->is_null()) {
        if (!interval_it->is_number_unsigned() && !interval_it->is_number_integer()) {
            error = "字段 session.interval_ms 期望整数或 null";
            return false;
        }
        const auto interval = interval_it->get<std::int64_t>();
        if (interval < 1) {
            error = "字段 session.interval_ms 必须大于 0";
            return false;
        }
        parsed.session.interval_ms = static_cast<std::uint64_t>(interval);
    }
    if (!detail::ReadOptionalString(session, "module", "session.module",
                                    parsed.session.module, error)) {
        return false;
    }

    // ---- 变量名单 ----
    const auto read_names = [&](std::string_view key, std::string_view path,
                                std::vector<std::string>& out_names) {
        const auto it = root.find(std::string(key));
        if (it == root.end()) {
            error = "缺少字段 " + std::string(path);
            return false;
        }
        if (!it->is_array()) {
            error = "字段 " + std::string(path) + " 期望数组";
            return false;
        }
        for (const auto& element : *it) {
            if (!element.is_string()) {
                error = "字段 " + std::string(path) + " 的元素必须是字符串";
                return false;
            }
            const auto name = element.get<std::string>();
            if (name.empty()) {
                error = "字段 " + std::string(path) + " 含空字符串";
                return false;
            }
            out_names.push_back(name);
        }
        return true;
    };
    if (!read_names("observe", "observe", parsed.observe)) {
        return false;
    }
    if (!read_names("calibrate", "calibrate", parsed.calibrate)) {
        return false;
    }

    out = std::move(parsed);
    return true;
}

/**
 * @brief 读取并解析工程配置文件
 * @param path 配置文件路径
 * @param out 成功时写入解析后的模型
 * @param error 失败时写入中文错误文本
 * @return true 表示成功
 *
 * @details 读取前先按 `kProjectFileMaxBytes` 限制文件大小，避免把巨型文件读入内存。
 */
[[nodiscard]] inline bool LoadProjectFile(const std::filesystem::path& path,
                                          ProjectFile& out,
                                          std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "无法读取工程配置文件 \"" + path.string() + "\"：" + ec.message();
        return false;
    }
    if (size > kProjectFileMaxBytes) {
        error = "工程配置文件过大（" + std::to_string(size) +
                " 字节），上限 1 MiB";
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "无法打开工程配置文件 \"" + path.string() + "\"";
        return false;
    }
    std::string text;
    text.resize(static_cast<std::size_t>(size));
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (stream.gcount() != static_cast<std::streamsize>(text.size())) {
        error = "读取工程配置文件 \"" + path.string() + "\" 时长度不足";
        return false;
    }

    std::string parse_error;
    if (!ParseProjectFileJson(text, out, parse_error)) {
        error = "工程配置文件 \"" + path.string() + "\" 无效：" + parse_error;
        return false;
    }
    return true;
}

/**
 * @brief 原子写出工程配置文件
 * @param path 目标路径
 * @param text 文件内容
 * @param error 失败时写入中文错误文本
 * @return true 表示成功
 *
 * @details 先写同目录临时文件，再原子替换目标，保证失败时不会留下半截文件。
 *          目标已存在时**拒绝覆盖**（`replace_existing=false`），
 *          避免用户误把已有工程配置冲掉。
 */
[[nodiscard]] inline bool WriteProjectFileAtomically(
    const std::filesystem::path& path, std::string_view text,
    std::string& error) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        error = "目标文件已存在，拒绝覆盖：\"" + path.string() + "\"";
        return false;
    }

    // 临时文件名带固定后缀，便于失败时人工识别与清理。
    std::filesystem::path temp = path;
    temp += ".tmp";

    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "无法创建临时文件 \"" + temp.string() + "\"";
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temp, ec);
            error = "写入临时文件 \"" + temp.string() + "\" 失败";
            return false;
        }
    }

    // rename 在目标已存在时不负责覆盖语义，故此处必须保证目标不存在。
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(temp, remove_ec);
        error = "替换目标文件 \"" + path.string() + "\" 失败：" + ec.message();
        return false;
    }
    return true;
}

/**
 * @brief 把工程配置的 `connection` 转换为 `TransportSelectionRequest`
 * @param connection 连接配置
 * @param out 成功时写入请求
 * @param error 失败时写入中文错误文本
 * @return true 表示成功
 *
 * @details 只做"文本 → 枚举"的还原，**不**做任何补全或推断：
 *          是否与 A2L 冲突、是否需要额外对齐，仍由
 *          `ResolveTransportSelection()` 统一裁决，避免工具层另立判词。
 *
 *          唯一例外是 `auto`/`auto-legacy` 与显式对齐同时出现：这两者在
 *          `ResolveTransportSelection()` 规则 8 里互斥，而早期版本的本工具
 *          在 auto 模式下写盘时恰好会留下这一组合（auto 尚未定下来、对齐却
 *          已落盘），导致 save 出的文件自己加载不回来。auto 的语义本就是
 *          "对齐由胜出候选决定"，因此这里丢弃对齐值、保留 auto 语义，属于
 *          按语义舍弃冗余字段，不构成推断。
 */
[[nodiscard]] inline bool ProjectConnectionToRequest(
    const ProjectConnection& connection, TransportSelectionRequest& out,
    std::string& error) {
    TransportSelectionRequest request;
    if (connection.transport == "tcp") {
        request.transport = TransportKind::Tcp;
    } else if (connection.transport == "udp") {
        request.transport = TransportKind::Udp;
    } else {
        error = "未知传输层：" + connection.transport;
        return false;
    }

    if (!connection.tcp_profile.empty()) {
        if (connection.tcp_profile == "auto") {
            request.tcp_profile_auto = true;
        } else if (connection.tcp_profile == "auto-legacy") {
            request.tcp_profile_auto_legacy = true;
        } else {
            const auto profile = ParseTcpWireProfileText(connection.tcp_profile);
            if (!profile.has_value()) {
                error = "无法识别的 tcp_profile：" + connection.tcp_profile;
                return false;
            }
            request.tcp_profile = profile;
        }
    }

    // auto/auto-legacy 下对齐由胜出候选决定；旧文件里若残留显式对齐，
    // 直接丢弃，否则规则 8 会以"自动模式不接受显式对齐"拒绝整个配置。
    const bool profile_is_auto =
        request.tcp_profile_auto || request.tcp_profile_auto_legacy;
    if (!profile_is_auto) {
        request.packet_alignment_bits = connection.packet_alignment_bits;
    }
    if (connection.protocol_version.has_value()) {
        request.protocol_version =
            ParseVersionText(*connection.protocol_version);
    }
    if (connection.transport_version.has_value()) {
        request.transport_version =
            ParseVersionText(*connection.transport_version);
    }
    if (!connection.host.empty()) {
        request.host = connection.host;
    }
    if (connection.port != 0) {
        request.port = connection.port;
    }
    request.preset_xcplite =
        connection.preset_xcplite.value_or(false);

    out = std::move(request);
    return true;
}


/**
 * @brief 取当前可执行文件所在目录
 * @details 不使用 `argv[0]`：它可能是相对路径、可能经过 PATH 查找，也可能
 *          被调用方改写。三个平台各自用系统提供的权威入口：
 *          Windows `GetModuleFileNameW`，macOS `_NSGetExecutablePath`，
 *          其余 POSIX `readlink("/proc/self/exe")`。
 * @return 可执行文件目录；无法取得时返回空路径（调用方需回退到当前目录）
 */
[[nodiscard]] inline std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::path(buffer.c_str()).parent_path();
#else
    std::string buffer(1024, '\0');
    const auto length =
        ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    buffer.resize(static_cast<std::size_t>(length));
    return std::filesystem::path(buffer).parent_path();
#endif
}

/**
 * @brief 推导 `save` 未给路径时的默认工程配置文件路径
 * @details 规则（计划 §5）：`<可执行文件目录>/<A2L 文件名去扩展名>.xcp.json`。
 *          取 A2L 文件名而不是固定名，是为了让同一台机器上为多个工程保存的
 *          配置天然不互相覆盖。
 * @param a2l_path A2L 文件路径
 * @return 默认保存路径；A2L 路径为空时返回空路径
 */
[[nodiscard]] inline std::filesystem::path DefaultProjectFilePath(
    const std::string& a2l_path) {
    if (a2l_path.empty()) return {};
    const auto stem = std::filesystem::path(a2l_path).stem().string();
    const auto directory = ExecutableDirectory();
    const auto base =
        directory.empty() ? std::filesystem::path(".") : directory;
    return base / (stem + ".xcp" + std::string(kProjectFileExtension));
}


/**
 * @brief 把用户侧传输请求写入工程配置文件的连接段
 * @details 与 `ProjectConnectionToRequest()` 构成往返：这里写的是
 *          `TransportSelectionRequest` 中的原始选择（--tcp-profile auto 等
 *          写作 `"auto"`），而不是解析后的 `ResolvedTransportSelection`。
 *          这样做的好处是配置文件能忠实还原"用户当初怎么选的"，包括
 *          auto 这种"还没定下来"的选择；解析结果由加载时重新计算。
 * @param request 用户侧原始请求
 * @param out 输出连接段
 */
inline void ConnectionFromRequest(const TransportSelectionRequest& request,
                                  ProjectConnection& out) {
    out = ProjectConnection{};
    out.transport =
        std::string(TransportKindName(request.transport));  // "tcp" / "udp"
    out.host = request.host.value_or("");
    out.port = request.port.value_or(0);
    if (request.tcp_profile_auto) {
        out.tcp_profile = "auto";
    } else if (request.tcp_profile_auto_legacy) {
        out.tcp_profile = "auto-legacy";
    } else if (request.tcp_profile.has_value()) {
        // TcpWireProfile 的正式名是 "AsamV15"/"AsamV14"，
        // 而配置文件与命令行使用 "asam-v1.5"/"asam-v1.4"，
        // 因此这里按小写连字符形式写出，保证与解析入口对称。
        out.tcp_profile =
            request.tcp_profile == TcpWireProfile::AsamV15 ? "asam-v1.5"
                                                           : "asam-v1.4";
    }
    out.packet_alignment_bits = request.packet_alignment_bits;
    if (request.protocol_version.has_value()) {
        out.protocol_version = request.protocol_version->ToString();
    }
    if (request.transport_version.has_value()) {
        out.transport_version = request.transport_version->ToString();
    }
    if (request.preset_xcplite) out.preset_xcplite = true;
}

/**
 * @brief 把运行时生效连接写入工程配置文件的连接段
 *
 * @details 与 `ConnectionFromRequest()` 的区别是取值来源：本函数写入的是
 *          「连上之后实际在用」的 `EffectiveConnection`，因此：
 *          - `tcp_profile` 恒为具体的 `"asam-v1.5"` / `"asam-v1.4"`，
 *            **绝不**写 `"auto"`——auto 表示"尚未确定"，而能走到保存说明
 *            已经嗅探完成；写 auto 会让下次加载重复嗅探，且与下面写出的
 *            具体对齐值在 `ResolveTransportSelection()` 规则 8 里直接冲突；
 *          - 版本号无权威证据时写 null，加载端回到 CONNECT 协商路径取证。
 *
 * @param effective 运行时生效参数
 * @param out 输出连接段
 */
inline void ConnectionFromEffective(const EffectiveConnection& effective,
                                    ProjectConnection& out) {
    out = ProjectConnection{};
    out.transport = effective.transport;
    out.host = effective.host;
    out.port = effective.port;
    out.endpoint_from_a2l = effective.endpoint_from_a2l;
    out.tcp_profile = effective.tcp_profile;
    out.packet_alignment_bits = effective.packet_alignment_bits;
    out.protocol_version = effective.protocol_version;
    out.transport_version = effective.transport_version;
    out.preset_xcplite = effective.preset_xcplite;
    out.a2l_snapshot_root = effective.a2l_snapshot_root;
}

/**
 * @brief 把 `PacketAlignment` 换算为配置文件中的位宽数值
 * @param alignment 对齐枚举
 * @return 8/16/32；`Unknown` 返回空
 */
[[nodiscard]] inline std::optional<std::uint8_t> AlignmentBitsOf(
    PacketAlignment alignment) {
    switch (alignment) {
        case PacketAlignment::Bit8:
            return static_cast<std::uint8_t>(8);
        case PacketAlignment::Bit16:
            return static_cast<std::uint8_t>(16);
        case PacketAlignment::Bit32:
            return static_cast<std::uint8_t>(32);
        default:
            return std::nullopt;
    }
}

/**
 * @brief 把 `TcpWireProfile` 换算为配置文件中的文本
 * @param profile Wire Profile
 * @return `"asam-v1.5"` / `"asam-v1.4"`；`Unknown` 返回空字符串
 */
[[nodiscard]] inline std::string WireProfileText(TcpWireProfile profile) {
    switch (profile) {
        case TcpWireProfile::AsamV15:
            return "asam-v1.5";
        case TcpWireProfile::AsamV14:
            return "asam-v1.4";
        default:
            return {};
    }
}

/**
 * @brief 由解析结果与运行期版本证据组装生效连接
 *
 * @param resolved `ResolveTransportSelection()` 的结果；嗅探模式下其
 *                 `wire_profile` / `alignment` 需由调用方回填胜出候选
 * @param protocol_version 协议层生效版本；无权威证据为空
 * @param transport_version 传输层生效版本；无权威证据为空
 * @return 组装好的生效连接
 *
 * @note `resolved.wire_profile` 为 `Unknown` 时 `tcp_profile` 写空字符串，
 *       由 `MergeProjectFileIntoOptions()` 在加载时判定为"TCP 但未指定
 *       Profile"并报出明确错误，而不是在保存端静默编造。
 */
[[nodiscard]] inline EffectiveConnection
MakeEffectiveConnection(const ResolvedTransportSelection& resolved,
                        const std::optional<XcpProtocolVersion>& protocol_version,
                        const std::optional<XcpProtocolVersion>&
                            transport_version) {
    EffectiveConnection effective;
    effective.transport = std::string(TransportKindName(resolved.transport));
    effective.host = resolved.host;
    effective.port = resolved.port;
    effective.endpoint_from_a2l =
        resolved.endpoint_source == EndpointSource::A2l;
    if (resolved.transport == TransportKind::Tcp) {
        effective.tcp_profile = WireProfileText(resolved.wire_profile);
    }
    effective.packet_alignment_bits = AlignmentBitsOf(resolved.alignment);
    if (protocol_version.has_value() && protocol_version->IsValid()) {
        effective.protocol_version = protocol_version->ToString();
    }
    if (transport_version.has_value() && transport_version->IsValid()) {
        effective.transport_version = transport_version->ToString();
    }
    return effective;
}

}  // namespace calmcar::xcp::tools
