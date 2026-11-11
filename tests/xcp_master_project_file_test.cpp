/**
 * @file xcp_master_project_file_test.cpp
 * @brief `tools/xcp_master/project_file.hpp` 的单元测试。
 *
 * 依据 `code-plan/xcp_master_工程配置文件_实施计划.md` §8.1 编写。
 *
 * @par 测试策略
 *   序列化层被刻意做成纯函数，就是让"写出去的东西"和"读回来的东西"可以
 *   逐个字段钉住。每个用例只改变一个输入维度：要么改写一份合法 JSON 的某个
 *   字段，要么构造一种畸形输入，断言拒绝原因可定位。涉及文件系统的小节使用
 *   临时目录，不污染仓库。
 */

#include "tools/xcp_master/project_file.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using calmcar::xcp::TcpWireProfile;
using calmcar::xcp::TransportKind;
using calmcar::xcp::TransportSelectionRequest;
using calmcar::xcp::tools::kProjectFileSchemaVersion;
using calmcar::xcp::tools::ParseProjectFileJson;
using calmcar::xcp::tools::ProjectConnection;
using calmcar::xcp::tools::ProjectConnectionToRequest;
using calmcar::xcp::tools::ProjectFile;
using calmcar::xcp::tools::ProjectFileToJson;
using calmcar::xcp::tools::WriteProjectFileAtomically;

/**
 * @brief 构造一份字段齐全的合法配置
 * @return 用于往返与畸形改写测试的基线模型
 */
ProjectFile MakeBaseline() {
    ProjectFile file;
    file.schema_version = kProjectFileSchemaVersion;
    file.a2l_path = "D:/project/libxcp/build/demo.a2l";
    file.connection.transport = "tcp";
    file.connection.host = "127.0.0.1";
    file.connection.port = 5555;
    file.connection.endpoint_from_a2l = false;
    file.connection.tcp_profile = "asam-v1.4";
    file.connection.packet_alignment_bits = 8;
    file.connection.protocol_version = "1.2";
    file.connection.transport_version = "1.4";
    file.connection.preset_xcplite = false;
    file.connection.a2l_snapshot_root = "D:/project/libxcp/build/tmp";
    file.session.mode = "poll";
    file.session.interval_ms = 500;
    file.session.module = "demo";
    file.observe = {"demo::meas_fast.engine_speed", "demo::meas_slow.coolant_temp"};
    file.calibrate = {"demo::cal.fuel_gain"};
    return file;
}

/**
 * @brief 解析一段 JSON 文本，断言失败并返回错误文本
 * @param text JSON 原文
 * @return 失败原因；若意外解析成功则返回空并让用例失败
 */
std::string ExpectRejected(const std::string& text) {
    ProjectFile parsed;
    std::string error;
    EXPECT_FALSE(ParseProjectFileJson(text, parsed, error))
        << "本应被拒绝的输入被接受了";
    return error;
}

/**
 * @brief 把文本中的一段字面量替换为另一段字面量
 * @details 相比 `std::string::replace(pos, count, ...)`，此函数按字面量长度
 *          自动计算 `count`，避免手数长度写错导致的残余字符或缺失逗号。
 * @param text 待改写的文本
 * @param from 要被替换的字面量（必须存在）
 * @param to 替换后的字面量
 */
void ReplaceLiteral(std::string& text, const std::string& from,
                    const std::string& to) {
    const auto position = text.find(from);
    ASSERT_NE(position, std::string::npos) << "未找到待替换片段：" << from;
    text.replace(position, from.size(), to);
}

/**
 * @brief 一个最小的合法 JSON（用于逐字段改写）
 * @return 合法 JSON 文本
 */
std::string MinimalJson() {
    return R"({
  "schema_version": 1,
  "a2l_path": "demo.a2l",
  "connection": {
    "transport": "udp",
    "host": "127.0.0.1",
    "port": 5555,
    "endpoint_from_a2l": false,
    "tcp_profile": "",
    "packet_alignment_bits": null,
    "protocol_version": null,
    "transport_version": null,
    "preset_xcplite": null,
    "a2l_snapshot_root": null
  },
  "session": {"mode": "poll", "interval_ms": 500, "module": null},
  "observe": [],
  "calibrate": []
})";
}

// ---------------------------------------------------------------------------
// 序列化往返
// ---------------------------------------------------------------------------

/// @brief 模型 → JSON → 模型后逐字段相等
TEST(ProjectFile, RoundTripPreservesEveryField) {
    const auto original = MakeBaseline();
    const auto text = ProjectFileToJson(original);

    ProjectFile parsed;
    std::string error;
    ASSERT_TRUE(ParseProjectFileJson(text, parsed, error)) << error;

    EXPECT_EQ(parsed.schema_version, original.schema_version);
    EXPECT_EQ(parsed.a2l_path, original.a2l_path);
    EXPECT_EQ(parsed.connection.transport, original.connection.transport);
    EXPECT_EQ(parsed.connection.host, original.connection.host);
    EXPECT_EQ(parsed.connection.port, original.connection.port);
    EXPECT_EQ(parsed.connection.endpoint_from_a2l,
              original.connection.endpoint_from_a2l);
    EXPECT_EQ(parsed.connection.tcp_profile, original.connection.tcp_profile);
    EXPECT_EQ(parsed.connection.packet_alignment_bits,
              original.connection.packet_alignment_bits);
    EXPECT_EQ(parsed.connection.protocol_version,
              original.connection.protocol_version);
    EXPECT_EQ(parsed.connection.transport_version,
              original.connection.transport_version);
    EXPECT_EQ(parsed.connection.preset_xcplite,
              original.connection.preset_xcplite);
    EXPECT_EQ(parsed.connection.a2l_snapshot_root,
              original.connection.a2l_snapshot_root);
    EXPECT_EQ(parsed.session.mode, original.session.mode);
    EXPECT_EQ(parsed.session.interval_ms, original.session.interval_ms);
    EXPECT_EQ(parsed.session.module, original.session.module);
    EXPECT_EQ(parsed.observe, original.observe);
    EXPECT_EQ(parsed.calibrate, original.calibrate);
}

/// @brief Windows 反斜杠路径与中文路径往返后逐字节一致
TEST(ProjectFile, RoundTripHandlesBackslashesAndChinese) {
    auto original = MakeBaseline();
    original.a2l_path = "D:\\项目\\资料\\demo.a2l";
    original.observe = {"模块::信号_一", "模块::signal\"quoted\""};
    original.calibrate = {"模块::标定值\t带制表符"};

    const auto text = ProjectFileToJson(original);

    ProjectFile parsed;
    std::string error;
    ASSERT_TRUE(ParseProjectFileJson(text, parsed, error)) << error;
    EXPECT_EQ(parsed.a2l_path, original.a2l_path);
    EXPECT_EQ(parsed.observe, original.observe);
    EXPECT_EQ(parsed.calibrate, original.calibrate);
}

/// @brief 无值字段写出 `null` 并能读回空值
TEST(ProjectFile, NullOptionalsSurviveRoundTrip) {
    auto original = MakeBaseline();
    original.connection.packet_alignment_bits.reset();
    original.connection.protocol_version.reset();
    original.connection.transport_version.reset();
    original.connection.preset_xcplite.reset();
    original.connection.a2l_snapshot_root.reset();
    original.session.interval_ms.reset();
    original.session.module.reset();

    const auto text = ProjectFileToJson(original);
    ProjectFile parsed;
    std::string error;
    ASSERT_TRUE(ParseProjectFileJson(text, parsed, error)) << error;

    EXPECT_FALSE(parsed.connection.packet_alignment_bits.has_value());
    EXPECT_FALSE(parsed.connection.protocol_version.has_value());
    EXPECT_FALSE(parsed.connection.transport_version.has_value());
    EXPECT_FALSE(parsed.connection.preset_xcplite.has_value());
    EXPECT_FALSE(parsed.connection.a2l_snapshot_root.has_value());
    EXPECT_FALSE(parsed.session.interval_ms.has_value());
    EXPECT_FALSE(parsed.session.module.has_value());
}

// ---------------------------------------------------------------------------
// 缺字段 / 版本 / 类型
// ---------------------------------------------------------------------------

/// @brief 缺 schema_version 被拒绝
TEST(ProjectFile, RejectsMissingSchemaVersion) {
    const auto error = ExpectRejected(R"({
  "a2l_path": "demo.a2l",
  "connection": {"transport": "udp", "host": "h", "port": 1,
    "endpoint_from_a2l": false, "tcp_profile": ""},
  "session": {"mode": "poll"}, "observe": [], "calibrate": []
})");
    EXPECT_NE(error.find("schema_version"), std::string::npos) << error;
}

/// @brief 缺 a2l_path 被拒绝
TEST(ProjectFile, RejectsMissingA2lPath) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"a2l_path\": \"demo.a2l\",", "");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("a2l_path"), std::string::npos) << error;
}

/// @brief 缺 connection.transport 被拒绝
TEST(ProjectFile, RejectsMissingTransport) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"transport\": \"udp\",", "");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("transport"), std::string::npos) << error;
}

/// @brief schema_version 不等于当前版本被拒绝
TEST(ProjectFile, RejectsForeignSchemaVersion) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"schema_version\": 1", "\"schema_version\": 2");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("schema_version=2"), std::string::npos) << error;
}

/// @brief port 写成字符串被拒绝
TEST(ProjectFile, RejectsPortAsString) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"port\": 5555", "\"port\": \"5555\"");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("connection.port"), std::string::npos) << error;
}

/// @brief observe 写成对象而不是数组被拒绝
TEST(ProjectFile, RejectsObserveAsObject) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"observe\": []", "\"observe\": {}");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("observe"), std::string::npos) << error;
}

/// @brief observe 数组元素不是字符串被拒绝
TEST(ProjectFile, RejectsNonStringObserveElement) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"observe\": []", "\"observe\": [1, 2]");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("observe"), std::string::npos) << error;
}

/// @brief 未知顶层字段被拒绝
TEST(ProjectFile, RejectsUnknownTopLevelKey) {
    const auto error = ExpectRejected(R"({
  "schema_version": 1, "a2l_path": "demo.a2l", "surprise": 1,
  "connection": {"transport": "udp", "host": "h", "port": 1,
    "endpoint_from_a2l": false, "tcp_profile": ""},
  "session": {"mode": "poll"}, "observe": [], "calibrate": []
})");
    EXPECT_NE(error.find("surprise"), std::string::npos) << error;
}

/// @brief connection 下的未知字段被拒绝
TEST(ProjectFile, RejectsUnknownConnectionKey) {
    const auto error = ExpectRejected(R"({
  "schema_version": 1, "a2l_path": "demo.a2l",
  "connection": {"transport": "udp", "host": "h", "port": 1,
    "endpoint_from_a2l": false, "tcp_profile": "", "extra_field": true},
  "session": {"mode": "poll"}, "observe": [], "calibrate": []
})");
    EXPECT_NE(error.find("connection.extra_field"), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// 语法错误
// ---------------------------------------------------------------------------

/// @brief 尾随逗号被拒绝，且错误文本含行列号
TEST(ProjectFile, RejectsTrailingCommaWithPosition) {
    const auto error = ExpectRejected(R"({"schema_version": 1,})");
    EXPECT_NE(error.find("JSON 语法错误"), std::string::npos) << error;
    EXPECT_NE(error.find("行"), std::string::npos) << error;
}

/// @brief 未闭合的字符串被拒绝
TEST(ProjectFile, RejectsUnterminatedString) {
    const auto error = ExpectRejected(R"({"schema_version": "abc)");
    EXPECT_NE(error.find("JSON 语法错误"), std::string::npos) << error;
}

/// @brief 顶层后面还有垃圾内容被拒绝
TEST(ProjectFile, RejectsTrailingGarbage) {
    const auto error = ExpectRejected(R"({"schema_version": 1} extra)");
    EXPECT_NE(error.find("JSON 语法错误"), std::string::npos) << error;
}

/// @brief 顶层不是对象被拒绝
TEST(ProjectFile, RejectsNonObjectRoot) {
    const auto error = ExpectRejected("[1, 2, 3]");
    EXPECT_NE(error.find("顶层"), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// 重复键语义（与计划 §3.3 定案一致：后者覆盖）
// ---------------------------------------------------------------------------

/// @brief 重复键按后者覆盖，不视为错误
TEST(ProjectFile, DuplicateKeyUsesLastValue) {
    const auto text = std::string(R"({
  "schema_version": 1, "a2l_path": "first.a2l", "a2l_path": "second.a2l",
  "connection": {"transport": "udp", "host": "h", "port": 1,
    "endpoint_from_a2l": false, "tcp_profile": ""},
  "session": {"mode": "poll"}, "observe": [], "calibrate": []
})");

    ProjectFile parsed;
    std::string error;
    ASSERT_TRUE(ParseProjectFileJson(text, parsed, error)) << error;
    EXPECT_EQ(parsed.a2l_path, "second.a2l");
}

// ---------------------------------------------------------------------------
// 数值与枚举边界
// ---------------------------------------------------------------------------

/// @brief port 上界 65535 被接受
TEST(ProjectFile, AcceptsPortUpperBound) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"port\": 5555", "\"port\": 65535");
    ProjectFile parsed;
    std::string error;
    ASSERT_TRUE(ParseProjectFileJson(text, parsed, error)) << error;
    EXPECT_EQ(parsed.connection.port, 65535U);
}

/// @brief port 超出 65535 被拒绝
TEST(ProjectFile, RejectsPortAboveRange) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"port\": 5555", "\"port\": 65536");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("connection.port"), std::string::npos) << error;
}

/// @brief port 为 0 被接受（表示由 A2L 提供端点）
TEST(ProjectFile, AcceptsPortZeroForA2lEndpoint) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"port\": 5555", "\"port\": 0");
    ReplaceLiteral(text, "\"endpoint_from_a2l\": false", "\"endpoint_from_a2l\": true");
    ProjectFile parsed;
    std::string error;
    ASSERT_TRUE(ParseProjectFileJson(text, parsed, error)) << error;
    EXPECT_EQ(parsed.connection.port, 0U);
    EXPECT_TRUE(parsed.connection.endpoint_from_a2l);
}

/// @brief interval_ms 为 0 被拒绝
TEST(ProjectFile, RejectsZeroInterval) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"interval_ms\": 500", "\"interval_ms\": 0");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("interval_ms"), std::string::npos) << error;
}

/// @brief 非法传输层取值被拒绝
TEST(ProjectFile, RejectsUnknownTransport) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"transport\": \"udp\"", "\"transport\": \"serial\"");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("transport"), std::string::npos) << error;
}

/// @brief UDP 传输层带非空 tcp_profile 被拒绝
TEST(ProjectFile, RejectsTcpProfileUnderUdp) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"tcp_profile\": \"\"", "\"tcp_profile\": \"asam-v1.4\"");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("tcp_profile"), std::string::npos) << error;
}

/// @brief TCP 传输层带非法 tcp_profile 被拒绝
TEST(ProjectFile, RejectsIllegalTcpProfile) {
    std::string text = R"({
  "schema_version": 1, "a2l_path": "demo.a2l",
  "connection": {"transport": "tcp", "host": "h", "port": 1,
    "endpoint_from_a2l": false, "tcp_profile": "asam-v9.9"},
  "session": {"mode": "poll"}, "observe": [], "calibrate": []
})";
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("tcp_profile"), std::string::npos) << error;
}

/// @brief packet_alignment_bits 非 8/16/32 被拒绝
TEST(ProjectFile, RejectsIllegalAlignment) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"packet_alignment_bits\": null", "\"packet_alignment_bits\": 12");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("packet_alignment_bits"), std::string::npos) << error;
}

/// @brief 非法 session.mode 被拒绝
TEST(ProjectFile, RejectsIllegalMode) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"mode\": \"poll\"", "\"mode\": \"burst\"");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("mode"), std::string::npos) << error;
}

/// @brief 版本号格式非法的 protocol_version 被拒绝
TEST(ProjectFile, RejectsMalformedProtocolVersion) {
    std::string text = MinimalJson();
    ReplaceLiteral(text, "\"protocol_version\": null", "\"protocol_version\": \"v1\"");
    const auto error = ExpectRejected(text);
    EXPECT_NE(error.find("protocol_version"), std::string::npos) << error;
}

// ---------------------------------------------------------------------------
// 连接配置 → TransportSelectionRequest 还原
// ---------------------------------------------------------------------------

/// @brief TCP + 显式 profile 能还原为请求
TEST(ProjectConnection, ConvertsTcpWithProfile) {
    const auto file = MakeBaseline();
    TransportSelectionRequest request;
    std::string error;
    ASSERT_TRUE(
        ProjectConnectionToRequest(file.connection, request, error)) << error;

    EXPECT_EQ(request.transport, TransportKind::Tcp);
    ASSERT_TRUE(request.tcp_profile.has_value());
    EXPECT_EQ(*request.tcp_profile, TcpWireProfile::AsamV14);
    EXPECT_FALSE(request.tcp_profile_auto);
    EXPECT_FALSE(request.tcp_profile_auto_legacy);
    ASSERT_TRUE(request.packet_alignment_bits.has_value());
    EXPECT_EQ(*request.packet_alignment_bits, 8U);
    ASSERT_TRUE(request.host.has_value());
    EXPECT_EQ(*request.host, std::string("127.0.0.1"));
    ASSERT_TRUE(request.port.has_value());
    EXPECT_EQ(*request.port, 5555U);
}

/// @brief `auto` 还原为严格自动选择标志
TEST(ProjectConnection, ConvertsAutoProfile) {
    auto file = MakeBaseline();
    file.connection.tcp_profile = "auto";
    TransportSelectionRequest request;
    std::string error;
    ASSERT_TRUE(
        ProjectConnectionToRequest(file.connection, request, error)) << error;
    EXPECT_TRUE(request.tcp_profile_auto);
    EXPECT_FALSE(request.tcp_profile.has_value());
}

/// @brief `auto-legacy` 还原为旧式自动选择标志
TEST(ProjectConnection, ConvertsAutoLegacyProfile) {
    auto file = MakeBaseline();
    file.connection.tcp_profile = "auto-legacy";
    TransportSelectionRequest request;
    std::string error;
    ASSERT_TRUE(
        ProjectConnectionToRequest(file.connection, request, error)) << error;
    EXPECT_TRUE(request.tcp_profile_auto_legacy);
}

/// @brief 端口为 0 时不写入请求，交给 A2L 提供
TEST(ProjectConnection, OmitsZeroPort) {
    auto file = MakeBaseline();
    file.connection.port = 0;
    file.connection.host.clear();
    file.connection.endpoint_from_a2l = true;
    TransportSelectionRequest request;
    std::string error;
    ASSERT_TRUE(
        ProjectConnectionToRequest(file.connection, request, error)) << error;
    EXPECT_FALSE(request.host.has_value());
    EXPECT_FALSE(request.port.has_value());
}

/// @brief UDP 传输能还原
TEST(ProjectConnection, ConvertsUdp) {
    ProjectConnection connection;
    connection.transport = "udp";
    connection.host = "10.0.0.1";
    connection.port = 5555;
    TransportSelectionRequest request;
    std::string error;
    ASSERT_TRUE(ProjectConnectionToRequest(connection, request, error)) << error;
    EXPECT_EQ(request.transport, TransportKind::Udp);
    EXPECT_FALSE(request.tcp_profile.has_value());
}

// ---------------------------------------------------------------------------
// 原子写入
// ---------------------------------------------------------------------------

/**
 * @brief 创建本次用例独占的临时目录
 * @return 临时目录路径
 */
std::filesystem::path MakeTempDir() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto name = "xcp_master_project_file_" + std::to_string(stamp);
    // 依次尝试：系统临时目录（受 TMPDIR/TEMP/TMP 影响）、当前目录。
    // 只试系统临时目录在受限环境（沙箱/CI）下会因访问被拒而失败，
    // 因此这里逐个尝试，成功即用。
    const auto base_candidates = {std::filesystem::temp_directory_path(),
                                  std::filesystem::current_path()};
    for (const auto& base : base_candidates) {
        std::error_code error_code;
        const auto dir = base / name;
        std::filesystem::create_directories(dir, error_code);
        if (!error_code) return dir;
    }
    throw std::runtime_error("无法创建测试临时目录");
}

/// @brief 写入新文件成功，且内容可被读回
TEST(ProjectFileIo, WritesNewFile) {
    const auto dir = MakeTempDir();
    const auto target = dir / "project.json";
    const auto text = ProjectFileToJson(MakeBaseline());

    std::string error;
    ASSERT_TRUE(WriteProjectFileAtomically(target, text, error)) << error;
    EXPECT_TRUE(std::filesystem::exists(target));

    ProjectFile loaded;
    ASSERT_TRUE(calmcar::xcp::tools::LoadProjectFile(target, loaded, error))
        << error;
    EXPECT_EQ(loaded.a2l_path, MakeBaseline().a2l_path);

    std::filesystem::remove_all(dir);
    SUCCEED();
}

/// @brief 目标已存在时拒绝覆盖，且原内容不变
TEST(ProjectFileIo, RefusesToOverwriteExistingFile) {
    const auto dir = MakeTempDir();
    const auto target = dir / "project.json";

    std::string error;
    ASSERT_TRUE(
        WriteProjectFileAtomically(target, "{\"first\":true}", error)) << error;

    std::string second_error;
    EXPECT_FALSE(WriteProjectFileAtomically(target, "{\"second\":true}",
                                            second_error));
    EXPECT_NE(second_error.find("已存在"), std::string::npos) << second_error;

    // 原内容必须保持不变（流需在删除目录前析构，否则 Windows 下文件被占用）
    std::string content;
    {
        std::ifstream stream(target, std::ios::binary);
        content.assign((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(content, "{\"first\":true}");

    std::filesystem::remove_all(dir);
}

/// @brief 写入后不残留临时文件
TEST(ProjectFileIo, LeavesNoTemporaryFile) {
    const auto dir = MakeTempDir();
    const auto target = dir / "project.json";
    std::string error;
    ASSERT_TRUE(
        WriteProjectFileAtomically(target, "{}", error)) << error;

    auto temp = target;
    temp += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(temp));

    std::filesystem::remove_all(dir);
}

/// @brief 读取不存在的文件时给出可定位的中文错误
TEST(ProjectFileIo, ReportsMissingFile) {
    std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "definitely_not_here.json";
    ProjectFile loaded;
    std::string error;
    EXPECT_FALSE(calmcar::xcp::tools::LoadProjectFile(missing, loaded, error));
    EXPECT_NE(error.find("无法读取"), std::string::npos) << error;
}

}  // namespace