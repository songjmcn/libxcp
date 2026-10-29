/**
 * @file a2l_tcp_alignment.cpp
 * @brief 受限 A2L 扫描器与双源核对的实现（见 `libxcp/a2l_tcp_alignment.hpp`）。
 *
 * @details 词法/结构子集全部放在本文件的匿名命名空间内：它是实现细节，
 *          任何外部调用方都只能通过 `ScanA2lTcpCandidate()` 与
 *          `CrossCheckA2lTcpAlignment()` 使用本组件。
 */

#include "libxcp/a2l_tcp_alignment.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace calmcar::xcp {
namespace {

namespace candidate_detail {

/// @brief 词法单元种类。
enum class TokenKind : std::uint8_t {
    kWord,     ///< 标识符或关键字。
    kNumber,   ///< 数值字面量。
    kString,   ///< 双引号字符串。
    kBegin,    ///< `/begin`。
    kEnd,      ///< `/end`。
    kInclude,  ///< `/include`（目标路径是紧随其后的字符串单元）。
};

/// @brief 词法单元。
struct Token {
    std::string text;       ///< 内容（字符串为去引号后的内容）。
    TokenKind kind{TokenKind::kWord};  ///< 种类。
};

/// @brief 仅识别 ASCII 空白。
inline bool Space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

/// @brief 判断 ASCII 字母或下划线。
inline bool Letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

/// @brief 判断 ASCII 数字。
inline bool Digit(char c) { return c >= '0' && c <= '9'; }

/**
 * @brief 拒绝会被 SDK 原始 `IF_DATA` 读取器误认的块终止序列。
 * @param text 字符串或注释内容。
 * @return 是否包含忽略空白后的危险序列。
 * @details SDK 的一部分读取路径按原始空白搜索 `/endIF_DATA`；字符串或注释里
 *          出现该序列会让"块边界"在两套解析器之间不一致，因此直接拒绝。
 */
inline bool PseudoEnd(std::string_view text) {
    std::string compact;
    for (char c : text)
        if (!Space(c)) compact.push_back(c);
    return compact.find("/endIF_DATA") != std::string::npos ||
           compact.find("/endA2ML") != std::string::npos;
}

/**
 * @brief 判断数值字面量是否属于 SDK 接受的形态。
 * @param s 待判定文本。
 * @return 是否形如 `[-+]?digits(.digits)?([eE][-+]?digits)?` 或 `[-+]?0xHEX`。
 */
inline bool IsNumberLiteral(std::string_view s) {
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        if (i + 2 >= s.size()) return false;
        for (std::size_t k = i + 2; k < s.size(); ++k) {
            const char c = s[k];
            if (!Digit(c) && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F'))
                return false;
        }
        return true;
    }
    bool digits = false;
    while (i < s.size() && Digit(s[i])) {
        ++i;
        digits = true;
    }
    if (!digits) return false;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && Digit(s[i])) ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        bool exp_digits = false;
        while (i < s.size() && Digit(s[i])) {
            ++i;
            exp_digits = true;
        }
        if (!exp_digits) return false;
    }
    return i == s.size();
}

/**
 * @brief 把 A2L 原文切分进受限安全子集；无法确定即返回 false。
 * @param input 完整原始字节。
 * @param out 成功时写入词法单元序列。
 * @return 是否整份输入都在子集内。
 * @details 拒绝的情况：字符串/注释**之外**出现非 ASCII 或控制字符、NUL、
 *          反斜杠转义、未闭合字符串或块注释、嵌套块注释、以及 `/` 开头的
 *          非 `/begin` `/end` `/include` 指令（含各类厂商扩展指令）。
 *
 *          字符串与注释内部允许任意 UTF-8 字节（真实 A2L 常见中文注释与
 *          中文 `"描述"`），因为这两处内容不参与结构判定；但其中出现
 *          块终止伪序列仍会拒绝（见 `PseudoEnd`）。
 */
inline bool Lex(std::string_view input, std::vector<Token>& out) {
    std::size_t i = 0;
    while (i < input.size()) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == 0 || (c < 32 && !Space(static_cast<char>(c)))) return false;
        if (Space(input[i])) {
            ++i;
            continue;
        }
        if (i + 1 < input.size() &&
            (input.substr(i, 2) == "//" || input.substr(i, 2) == "/*")) {
            const bool block = input[i + 1] == '*';
            const auto start = i;
            i += 2;
            if (block) {
                const auto end = input.find("*/", i);
                if (end == std::string_view::npos) return false;
                if (input.substr(i, end - i).find("/*") != std::string_view::npos)
                    return false;  // 嵌套注释语义在两套解析器间不一致
                i = end + 2;
            } else {
                while (i < input.size() && input[i] != '\r' && input[i] != '\n')
                    ++i;
            }
            const auto comment = input.substr(start, i - start);
            for (unsigned char b : comment)
                if (b == 0) return false;
            if (PseudoEnd(comment)) return false;
            continue;
        }
        if (input[i] == '"') {
            const auto start = ++i;
            while (i < input.size() && input[i] != '"') {
                if (static_cast<unsigned char>(input[i]) == 0 || input[i] == '\\')
                    return false;
                ++i;
            }
            if (i == input.size()) return false;
            const auto text = input.substr(start, i - start);
            if (PseudoEnd(text)) return false;
            out.push_back({std::string(text), TokenKind::kString});
            ++i;
            continue;
        }
        if (input[i] == '/') {
            const auto start = i++;
            while (i < input.size() && Letter(input[i])) ++i;
            const auto word = input.substr(start, i - start);
            if (word != "/begin" && word != "/end" && word != "/include")
                return false;
            if (i == input.size() || !Space(input[i])) return false;
            auto next = i;
            while (next < input.size() && Space(input[next])) ++next;
            if (next == input.size()) return false;
            if (word == "/include") {
                if (input[next] != '"') return false;
            } else if (!Letter(input[next])) {
                return false;
            }
            out.push_back({std::string(word),
                           word == "/begin"
                               ? TokenKind::kBegin
                               : (word == "/end" ? TokenKind::kEnd
                                                 : TokenKind::kInclude)});
            continue;
        }
        if (input[i] == '+' || input[i] == '-' || Digit(input[i])) {
            const auto start = i++;
            while (i < input.size() &&
                   (Digit(input[i]) || input[i] == '.' || input[i] == 'x' ||
                    input[i] == 'X' || input[i] == 'e' || input[i] == 'E' ||
                    input[i] == '+' || input[i] == '-' ||
                    (input[i] >= 'a' && input[i] <= 'f') ||
                    (input[i] >= 'A' && input[i] <= 'F')))
                ++i;
            const auto text = std::string(input.substr(start, i - start));
            if (!IsNumberLiteral(text)) return false;
            out.push_back({text, TokenKind::kNumber});
            continue;
        }
        if (!Letter(input[i])) return false;
        const auto start = i++;
        // A2L 标识符允许点号（如 `conv.bool`、`cpp_demo.table`）；末尾点号视为
        // 非法，避免把 `X.` 这类排版残片当成标识符而掩盖真正的结构错误。
        while (i < input.size() &&
               (Letter(input[i]) || Digit(input[i]) || input[i] == '.'))
            ++i;
        const auto word = input.substr(start, i - start);
        if (word.back() == '.') return false;
        if (word == "IF_DATA" && !out.empty() &&
            out.back().kind == TokenKind::kBegin) {
            // SDK 按原始空白提取协议名：注释不能充当分隔符或协议名前缀。
            if (i == input.size() || !Space(input[i])) return false;
            auto protocol = i;
            while (protocol < input.size() && Space(input[protocol])) ++protocol;
            if (input.substr(protocol, 3) != "XCP") return false;
            if (protocol + 3 < input.size() && !Space(input[protocol + 3]))
                return false;
        }
        out.push_back({std::string(word), TokenKind::kWord});
    }
    return true;
}

}  // namespace candidate_detail

/**
 * @brief 解析十进制或 `0x` 前缀十六进制的无符号整数。
 * @param text 数值字面量原文。
 * @return 解析结果；非法字符或溢出时为空。
 * @details `std::stoull` 按十进制解析 `0x104` 只会得到 `0`，因此这里显式识别
 *          十六进制前缀，避免版本号被静默读成 0 后与 SDK 结果"看似不一致"。
 */
[[nodiscard]] inline std::optional<std::uint64_t> ParseUnsignedLiteral(
    std::string_view text) {
    std::uint64_t value = 0;
    std::size_t i = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        for (i = 2; i < text.size(); ++i) {
            const char c = text[i];
            std::uint64_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint64_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint64_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint64_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
            if (value > (0xFFFFFFFFFFFFFFFFULL - digit) / 16U) return std::nullopt;
            value = value * 16U + digit;
        }
        return value;
    }
    if (text.empty()) return std::nullopt;
    for (const char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (0xFFFFFFFFFFFFFFFFULL - digit) / 10U) return std::nullopt;
        value = value * 10U + digit;
    }
    return value;
}

}  // namespace

/**
 * @brief 在受限子集内定位唯一 `XCP_ON_TCP_IP` 块。
 * @param bytes 调用方读取的完整原始字节；函数自身不读文件。
 * @param resolver `/include` 解析回调；为空时任何 `/include` 都导致拒绝。
 * @return 无候选表示输入不在子集内或结构上有歧义。
 *
 * @details 接受条件（全部满足才返回候选）：
 *          - 整份文件词法可判定，`/begin` `/end` 名称严格配平、无跨层闭合；
 *          - 恰有一个 `MODULE`，且文件内没有任何 `XCPplus` 块；
 *          - 全文件恰有一个 `XCP_ON_TCP_IP`，且它是某个协议为 `XCP` 的
 *            `IF_DATA` 的直接子块，而该 `IF_DATA` 又是 `MODULE` 的直接子块；
 *          - 该 TCP 块内字段无重复、无未知键（只认 `PACKET_ALIGNMENT_x`、
 *            `HOST_NAME`、`ADDRESS`；出现 `IPV6` 等一律拒绝）；
 *          - `/include` 目标必须是 A2ML 文件（首个结构 token 为 `/begin A2ML`），
 *            且其原文不含 `XCP_ON_TCP_IP` / `XCP_ON_UDP_IP` 字样。A2ML 只
 *            描述块schema，不可能新增端点，因此"不展开 AML"是安全的；反过来，
 *            任何可能引入端点的包含文件都会被拒绝。
 *
 * @note 允许同一 `MODULE` 下存在多个 `IF_DATA XCP`（真实 A2L 常把
 *       `SEGMENT`/`CHECKSUM` 与 `PROTOCOL_LAYER`/`DAQ`/`TCP` 分成两块），
 *       也允许 `INSTANCE` 等对象上的 `IF_DATA XCP`，只要它们不含 TCP 端点块；
 *       这正是"端点声明唯一"这一更强不变量的正确表述。
 */
[[nodiscard]] std::optional<A2lTcpCandidate> ScanA2lTcpCandidate(
    std::string_view bytes, const IncludeResolver& resolver) {
    using namespace candidate_detail;
    std::vector<Token> tokens;
    if (!Lex(bytes, tokens)) return std::nullopt;

    /// @brief 栈上的块：名字、是否为 XCP 的 IF_DATA、其父块名。
    struct OpenBlock {
        std::string name;
        bool xcp_if_data{false};
        std::string parent;
    };
    std::vector<OpenBlock> stack;
    std::size_t module_count = 0;
    std::size_t tcp_block_count = 0;
    A2lTcpCandidate result;

    std::size_t i = 0;
    while (i < tokens.size()) {
        const Token& token = tokens[i];
        if (token.kind == TokenKind::kInclude) {
            if (i + 1 >= tokens.size() || tokens[i + 1].kind != TokenKind::kString)
                return std::nullopt;
            const std::string path = tokens[i + 1].text;
            i += 2;
            if (!resolver) return std::nullopt;
            const auto content = resolver(path);
            if (!content.has_value()) return std::nullopt;
            // AML 必须是 A2ML：跳过空白后以 `/begin A2ML` 开头。
            std::size_t probe = 0;
            while (probe < content->size() && Space((*content)[probe])) ++probe;
            if (content->substr(probe, 11) != "/begin A2ML") return std::nullopt;
            if (content->find("XCP_ON_TCP_IP") != std::string::npos ||
                content->find("XCP_ON_UDP_IP") != std::string::npos)
                return std::nullopt;
            continue;
        }
        if (token.kind == TokenKind::kBegin) {
            if (i + 1 >= tokens.size() || tokens[i + 1].kind != TokenKind::kWord)
                return std::nullopt;
            std::string name = tokens[i + 1].text;
            i += 2;
            OpenBlock block;
            block.name = name;
            block.parent = stack.empty() ? std::string() : stack.back().name;
            if (name == "MODULE") {
                if (block.parent != "PROJECT") return std::nullopt;
                if (++module_count > 1) return std::nullopt;
            }
            if (name == "IF_DATA") {
                if (i >= tokens.size() || tokens[i].kind != TokenKind::kWord)
                    return std::nullopt;
                const std::string protocol = tokens[i].text;
                ++i;
                if (protocol == "XCPplus") return std::nullopt;
                block.xcp_if_data = protocol == "XCP";
            }
            if (name == "XCP_ON_TCP_IP") {
                if (stack.empty() || !stack.back().xcp_if_data ||
                    stack.back().parent != "MODULE" || tcp_block_count != 0)
                    return std::nullopt;
                ++tcp_block_count;
                if (i + 1 >= tokens.size() || tokens[i].kind != TokenKind::kNumber ||
                    tokens[i + 1].kind != TokenKind::kNumber)
                    return std::nullopt;
                A2lTcpCandidate parsed;
                // 版本号可能写成 `0x104`，必须按字面量进制解析而不是十进制截断。
                const auto version = ParseUnsignedLiteral(tokens[i].text);
                const auto port = ParseUnsignedLiteral(tokens[i + 1].text);
                if (!version.has_value() || !port.has_value()) return std::nullopt;
                parsed.version = *version;
                i += 2;
                if (*port == 0 || *port > 65535) return std::nullopt;
                parsed.port = static_cast<std::uint16_t>(*port);
                while (true) {
                    if (i >= tokens.size()) return std::nullopt;
                    if (tokens[i].kind == TokenKind::kEnd) {
                        if (i + 1 >= tokens.size() ||
                            tokens[i + 1].kind != TokenKind::kWord ||
                            tokens[i + 1].text != "XCP_ON_TCP_IP")
                            return std::nullopt;
                        i += 2;
                        break;
                    }
                    if (tokens[i].kind != TokenKind::kWord) return std::nullopt;
                    const std::string key = tokens[i].text;
                    ++i;
                    if (key == "PACKET_ALIGNMENT") {
                        if (parsed.alignment) return std::nullopt;
                        if (i >= tokens.size() || tokens[i].kind != TokenKind::kWord)
                            return std::nullopt;
                        const std::string value = tokens[i].text;
                        ++i;
                        if (value == "PACKET_ALIGNMENT_8")
                            parsed.alignment = std::uint8_t{8};
                        else if (value == "PACKET_ALIGNMENT_16")
                            parsed.alignment = std::uint8_t{16};
                        else if (value == "PACKET_ALIGNMENT_32")
                            parsed.alignment = std::uint8_t{32};
                        else
                            return std::nullopt;
                    } else if (key == "HOST_NAME" || key == "ADDRESS") {
                        if (i >= tokens.size() || tokens[i].kind != TokenKind::kString)
                            return std::nullopt;
                        std::string value = tokens[i].text;
                        ++i;
                        auto& slot = key == "HOST_NAME" ? parsed.host : parsed.address;
                        if (slot) return std::nullopt;
                        slot = std::move(value);
                    } else {
                        return std::nullopt;  // 未纳入核对的键一律不支持
                    }
                }
                result = std::move(parsed);
                continue;  // 已手工消费到 `/end XCP_ON_TCP_IP`
            }
            stack.push_back(std::move(block));
            continue;
        }
        if (token.kind == TokenKind::kEnd) {
            if (i + 1 >= tokens.size() || tokens[i + 1].kind != TokenKind::kWord)
                return std::nullopt;
            const std::string name = tokens[i + 1].text;
            i += 2;
            if (stack.empty() || stack.back().name != name) return std::nullopt;
            stack.pop_back();
            continue;
        }
        ++i;  // 普通内容单元不参与结构判定
    }

    if (!stack.empty()) return std::nullopt;
    if (module_count != 1) return std::nullopt;
    if (tcp_block_count != 1) return std::nullopt;
    return result;
}

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
    const std::vector<bool>& plus_flags) {
    A2lAlignmentVerdict verdict;
    if (endpoints.size() != 1) {
        verdict.reason = "SDK 报告的 TCP 端点数不是 1，无法确定核对对象";
        return verdict;
    }
    if (!plus_flags.empty() && plus_flags.front()) {
        verdict.reason = "SDK 端点来自 XCPplus 块，本工具拒绝使用";
        return verdict;
    }
    const SdkTcpEndpointFacts& sdk = endpoints.front();
    if (sdk.port != scan.port) {
        verdict.reason = "端口不一致：原文 " + std::to_string(scan.port) +
                         " vs SDK " + std::to_string(sdk.port);
        return verdict;
    }
    if (sdk.version != scan.version) {
        verdict.reason = "传输层版本不一致：原文 " +
                         std::to_string(scan.version) + " vs SDK " +
                         std::to_string(sdk.version);
        return verdict;
    }
    if (!scan.alignment.has_value()) {
        verdict.trust = A2lAlignmentTrust::kNotDeclared;
        verdict.reason = "原文未声明 PACKET_ALIGNMENT，不得假定为 8 bit";
        return verdict;
    }
    if (!sdk.alignment_declared || !sdk.alignment.has_value()) {
        verdict.reason = "SDK 未把 PACKET_ALIGNMENT 视为声明，两源不一致";
        return verdict;
    }
    if (*sdk.alignment != *scan.alignment) {
        verdict.reason = "位宽不一致：原文 " + std::to_string(*scan.alignment) +
                         "bit vs SDK " + std::to_string(*sdk.alignment) + "bit";
        return verdict;
    }
    // 端点地址也必须一致，否则可能核对到了另一台设备的声明。
    const bool sdk_host_known = !sdk.host.empty();
    const bool scan_host_known =
        scan.host.has_value() && !scan.host->empty();
    const bool scan_addr_known =
        scan.address.has_value() && !scan.address->empty();
    if (sdk_host_known && (scan_host_known || scan_addr_known)) {
        const bool host_match = scan_host_known && *scan.host == sdk.host;
        const bool addr_match = scan_addr_known && *scan.address == sdk.host;
        if (!host_match && !addr_match) {
            verdict.reason = "端点地址不一致：原文 " +
                             (scan_host_known ? *scan.host : *scan.address) +
                             " vs SDK " + sdk.host;
            return verdict;
        }
    }
    verdict.trust = A2lAlignmentTrust::kTrusted;
    verdict.bits = scan.alignment;
    verdict.reason = "原文与 SDK 均声明 PACKET_ALIGNMENT_" +
                     std::to_string(*scan.alignment) + "bit，且端点一致";
    return verdict;
}

}  // namespace calmcar::xcp
