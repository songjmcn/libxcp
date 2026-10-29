/**
 * @file a2l_tcp_alignment_sdk_differential_test.cpp
 * @brief 对同一文件字节比较隔离候选扫描器和 A2L 桥接结果。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "libxcp/a2l_tcp_alignment.hpp"
#include "libxcp/a2l/a2l_bridge.hpp"

namespace {
using calmcar::xcp::a2l::A2lBridge;
using calmcar::xcp::a2l::TransportEndpoint;
using calmcar::xcp::A2lAlignmentTrust;
using calmcar::xcp::CrossCheckA2lTcpAlignment;
using calmcar::xcp::ScanA2lTcpCandidate;
using calmcar::xcp::SdkTcpEndpointFacts;

/**
 * @brief 把桥接端点转成核对方所需的 SDK 侧事实
 * @param endpoint 桥接导出的传输端点
 * @return 仅含核对所需字段的视图
 */
SdkTcpEndpointFacts ToFacts(const TransportEndpoint& endpoint) {
    SdkTcpEndpointFacts facts;
    facts.host = endpoint.remote_host;
    facts.port = endpoint.remote_port;
    facts.version = endpoint.version;
    facts.alignment = endpoint.packet_alignment;
    facts.alignment_declared = endpoint.packet_alignment_declared;
    return facts;
}

/** @brief 构造扫描器支持的最小语法，不添加 SDK 必需结构。 */
std::string Sample(std::string_view options) {
    return "ASAP2_VERSION 1 71\n/begin PROJECT p \"\"\n/begin MODULE m \"\"\n"
           "/begin IF_DATA XCP\n/begin XCP_ON_TCP_IP 0x104 5555\n" +
           std::string(options) +
           "\n/end XCP_ON_TCP_IP\n/end IF_DATA\n/end MODULE\n/end PROJECT\n";
}

/** @brief 每个样本单独创建文件，扫描器读取与桥接器相同的文件内容。 */
class DifferentialTest : public ::testing::Test {
protected:
    /** @brief 创建独立临时目录。 */
    void SetUp() override {
        std::random_device random;
        for (int i = 0; i < 100; ++i) {
            dir_ = std::filesystem::temp_directory_path() /
                   ("libxcp_tcp_diff_" + std::to_string(random()));
            if (std::filesystem::create_directory(dir_)) {
                created_ = true;
                return;
            }
        }
        FAIL() << "无法创建临时目录";
    }
    /** @brief 清理本测试创建的临时目录。 */
    void TearDown() override {
        std::error_code ec;
        if (created_) std::filesystem::remove_all(dir_, ec);
    }
    /** @brief 将样本按二进制写入，再以二进制读取供扫描器使用。 */
    void Compare(const std::string& bytes, bool candidate_expected, bool sdk_expected,
                 int alignment = 0, std::string_view host = {},
                 const char* declared_host = nullptr, const char* declared_address = nullptr) {
        const auto path = dir_ / "sample.a2l";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(out.is_open());
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            ASSERT_TRUE(out.good());
        }
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.is_open());
        const std::string read_bytes{std::istreambuf_iterator<char>(in), {}};
        ASSERT_EQ(read_bytes, bytes);
        const auto candidate = ScanA2lTcpCandidate(read_bytes);
        EXPECT_EQ(candidate.has_value(), candidate_expected);
        auto loaded = A2lBridge::Load(path.string());
        ASSERT_EQ(loaded.HasValue(), sdk_expected);
        if (!sdk_expected) return;
        const auto* info = loaded.Value()->XcpInfo();
        ASSERT_NE(info, nullptr);
        EXPECT_TRUE(info->ok);
        ASSERT_EQ(info->transports.size(), 1U);
        if (!candidate_expected) return;
        ASSERT_TRUE(candidate);
        EXPECT_EQ(candidate->version, 0x104U);
        EXPECT_EQ(candidate->port, 5555U);
        EXPECT_EQ(candidate->host.has_value(), declared_host != nullptr);
        if (declared_host) EXPECT_EQ(candidate->host, declared_host);
        EXPECT_EQ(candidate->address.has_value(), declared_address != nullptr);
        if (declared_address) EXPECT_EQ(candidate->address, declared_address);
        EXPECT_EQ(candidate->alignment.has_value(), alignment != 0);
        if (alignment != 0) ASSERT_EQ(candidate->alignment, alignment);
        const auto& endpoint = info->transports.front();
        EXPECT_EQ(endpoint.kind, TransportEndpoint::Kind::TcpIp);
        EXPECT_EQ(endpoint.instance_count, 1U);
        EXPECT_EQ(endpoint.version, 0x104U);
        EXPECT_EQ(endpoint.remote_port, 5555U);
        EXPECT_EQ(endpoint.remote_host, host);
        EXPECT_EQ(endpoint.packet_alignment_declared, alignment != 0);
        EXPECT_EQ(endpoint.packet_alignment, alignment == 0 ? 8 : alignment);
        EXPECT_EQ(endpoint.version, candidate->version);
        EXPECT_EQ(endpoint.remote_port, candidate->port);
        EXPECT_EQ(endpoint.packet_alignment_declared, candidate->alignment.has_value());
        if (candidate->alignment) EXPECT_EQ(endpoint.packet_alignment, *candidate->alignment);
        // 每个可解析样本都追加一次核对：两源一致才允许信任。
        const auto checked =
            CrossCheckA2lTcpAlignment(*candidate, {ToFacts(endpoint)},
                                      {info->from_xcp_plus});
        if (alignment == 0) {
            EXPECT_EQ(checked.trust, A2lAlignmentTrust::kNotDeclared);
        } else {
            EXPECT_EQ(checked.trust, A2lAlignmentTrust::kTrusted);
            ASSERT_TRUE(checked.bits.has_value());
            EXPECT_EQ(*checked.bits, alignment);
        }
    }
    std::filesystem::path dir_;  ///< 此用例独占的目录。
    bool created_ = false;  ///< 防止清理并非由本用例创建的目录。
};

TEST_F(DifferentialTest, MinimalGrammarAlignmentAndEndpoints) {
    Compare(Sample("HOST_NAME \"ecu.local\""), true, true, 0, "ecu.local", "ecu.local");
    Compare(Sample("ADDRESS \"192.0.2.1\" PACKET_ALIGNMENT PACKET_ALIGNMENT_8"),
            true, true, 8, "192.0.2.1", nullptr, "192.0.2.1");
    Compare(Sample("HOST_NAME \"ecu.local\" PACKET_ALIGNMENT PACKET_ALIGNMENT_16"),
            true, true, 16, "ecu.local", "ecu.local");
    Compare(Sample("HOST_NAME \"ecu.local\" ADDRESS \"192.0.2.1\" PACKET_ALIGNMENT PACKET_ALIGNMENT_32"),
            true, true, 32, "ecu.local", "ecu.local", "192.0.2.1");
}

TEST_F(DifferentialTest, ProtocolAdjacentCommentCounterexample) {
    auto bytes = Sample("HOST_NAME \"ecu.local\"");
    const auto pos = bytes.find("IF_DATA XCP\n");
    ASSERT_NE(pos, std::string::npos);
    bytes.replace(pos, std::string("IF_DATA XCP\n").size(), "IF_DATA XCP/*c*/\n");
    Compare(bytes, false, false);
}

TEST_F(DifferentialTest, RealFixtureAndInsertedTcp) {
    const auto fixture = std::filesystem::path(A2L_DATA_DIR) / "cpp_demo_V201_corrected.a2l";
    std::ifstream source(fixture, std::ios::binary);
    ASSERT_TRUE(source.is_open());
    const std::string original{std::istreambuf_iterator<char>(source), {}};
    const std::string anchor = "/begin PROTOCOL_LAYER";
    const auto pos = original.find(anchor);
    ASSERT_NE(pos, std::string::npos);
    ASSERT_EQ(original.find(anchor, pos + 1), std::string::npos);
    const std::string if_data = "/begin IF_DATA XCP";
    const auto block_pos = original.rfind(if_data, pos);
    ASSERT_NE(block_pos, std::string::npos);
    EXPECT_EQ(original.substr(block_pos + if_data.size(), pos - block_pos - if_data.size()).find_first_not_of(" \t\r\n"), std::string::npos);
    ASSERT_EQ(original.find("XCP_ON_TCP_IP"), std::string::npos);
    auto modified = original;
    modified.insert(pos,
                    "/begin XCP_ON_TCP_IP 0x104 5555\nHOST_NAME \"ecu.local\"\n"
                    "PACKET_ALIGNMENT PACKET_ALIGNMENT_16\n/end XCP_ON_TCP_IP\n");
    for (const auto* bytes : std::vector<const std::string*>{&original, &modified}) {
        SCOPED_TRACE(bytes == &original ? "original" : "inserted TCP");
        const auto path = dir_ / "fixture.a2l";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(out.is_open());
            out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
            ASSERT_TRUE(out.good());
        }
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.is_open());
        const std::string read_bytes{std::istreambuf_iterator<char>(in), {}};
        ASSERT_EQ(read_bytes, *bytes);
        const auto candidate = ScanA2lTcpCandidate(read_bytes);
        if (bytes == &original) {
            // 原文件没有 TCP 端点声明：扫描器必须给"无候选"，而不是编一个默认值。
            EXPECT_FALSE(candidate);
        }
        auto loaded = A2lBridge::Load(path.string());
        ASSERT_TRUE(loaded.HasValue());
        const auto* info = loaded.Value()->XcpInfo();
        ASSERT_NE(info, nullptr);
        EXPECT_TRUE(info->ok);
        ASSERT_EQ(info->transports.size(), bytes == &original ? 0U : 1U);
        if (bytes == &modified) {
            // 真实生成器形态（MOD_PAR、MEMORY_SEGMENT、INSTANCE 级 IF_DATA、
            // 中文注释与 °C 字符串、指数形式的数字）必须被扫描器接受。
            ASSERT_TRUE(candidate) << "受限子集必须覆盖真实生成器的常见形态";
            EXPECT_EQ(candidate->port, 5555U);
            EXPECT_EQ(candidate->host, "ecu.local");
            ASSERT_TRUE(candidate->alignment.has_value());
            EXPECT_EQ(*candidate->alignment, 16);

            const auto& endpoint = info->transports.front();
            EXPECT_EQ(endpoint.kind, TransportEndpoint::Kind::TcpIp);
            EXPECT_EQ(endpoint.instance_count, 1U);
            EXPECT_EQ(endpoint.version, 0x104U);
            EXPECT_EQ(endpoint.remote_port, 5555U);
            EXPECT_EQ(endpoint.remote_host, "ecu.local");
            EXPECT_TRUE(endpoint.packet_alignment_declared);
            EXPECT_EQ(endpoint.packet_alignment, 16);
            // 两源一致 -> 才允许作为 A2L 静态证据。
            const auto checked = CrossCheckA2lTcpAlignment(
                *candidate, {ToFacts(endpoint)}, {info->from_xcp_plus});
            EXPECT_EQ(checked.trust, A2lAlignmentTrust::kTrusted);
            ASSERT_TRUE(checked.bits.has_value());
            EXPECT_EQ(*checked.bits, 16);
        }
    }
}

TEST_F(DifferentialTest, RejectsDuplicatesAndLexicalCounterexamples) {
    // SDK 对部分扫描器拒绝的输入宽松接受，不能反推输入合法或安全。
    for (const auto& options : {"PACKET_ALIGNMENT PACKET_ALIGNMENT_8 PACKET_ALIGNMENT PACKET_ALIGNMENT_16",
                                "HOST_NAME \"a\" HOST_NAME \"b\"", "ADDRESS \"a\" ADDRESS \"b\"",
                                "PACKET_ALIGNMENT PACKET_ALIGNMENT_64", "HOST_NAME \"bad\\escape\"", "@"}) {
        SCOPED_TRACE(options);
        Compare(Sample(options), false, true);
    }
    Compare(Sample("/* /end IF_DATA */"), false, false);
    Compare(Sample("HOST_NAME \"unterminated"), false, false);
    auto nul = Sample("");
    nul.insert(10, 1, '\0');
    Compare(nul, false, false);
}
}  // namespace
