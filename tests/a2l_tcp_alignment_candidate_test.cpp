/**
 * @file a2l_tcp_alignment_candidate_test.cpp
 * @brief 受原文语法约束的 TCP 端点扫描器与双源核对规则的单元测试。
 *
 * @details 这些用例只验证"输入落在受限子集内时扫描结果是否正确、超出子集时是否
 *          拒绝"，以及核对规则是否只在两源一致时才给出信任；它们不构成
 *          "任意 A2L 都能安全解析"的证明。真实文件与 SDK 的差分见
 *          `a2l_tcp_alignment_sdk_differential_test.cpp`。
 */
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "libxcp/a2l_tcp_alignment.hpp"

namespace {

using calmcar::xcp::A2lAlignmentTrust;
using calmcar::xcp::A2lTcpCandidate;
using calmcar::xcp::CrossCheckA2lTcpAlignment;
using calmcar::xcp::ScanA2lTcpCandidate;
using calmcar::xcp::SdkTcpEndpointFacts;

/// @brief 生成最小受限文法样本：TCP 块直属于 MODULE 下的 IF_DATA XCP。
std::string Sample(const std::string& options) {
    return "ASAP2_VERSION 1 71\n/begin PROJECT p \"\"\n/begin MODULE m \"\"\n"
           "/begin IF_DATA XCP\n/begin XCP_ON_TCP_IP 0x104 5555\n" +
           options +
           "\n/end XCP_ON_TCP_IP\n/end IF_DATA\n/end MODULE\n/end PROJECT\n";
}

/**
 * @brief 生成贴近真实生成器的样本。
 * @param tcp_options TCP 块内选项。
 * @param with_include 是否在 MODULE 下加入 `/include "XCP_104.aml"`。
 * @return 含 INSTANCE 级 IF_DATA、SEGMENT 级 IF_DATA 与 TCP 块的 A2L 文本。
 * @details 真实 A2L 常把 `SEGMENT`/`CHECKSUM` 与 `PROTOCOL_LAYER`/`DAQ`/`TCP`
 *          分成两个 `IF_DATA XCP`，并在 `INSTANCE` 上也挂 `IF_DATA XCP`；
 *          扫描器必须接受这种形态，只要求"TCP 端点声明唯一"。
 */
std::string RealShapeSample(const std::string& tcp_options, bool with_include) {
    std::string text =
        "ASAP2_VERSION 1 71\n"
        "/begin PROJECT demo \"\"\n"
        "/begin HEADER \"\" VERSION \"1.0\" /end HEADER\n"
        "/begin MODULE demo \"\"\n";
    if (with_include) text += "/include \"XCP_104.aml\"\n";
    text +=
        "/* 中文注释：测量对象 */\n"
        "/begin TYPEDEF_STRUCTURE Fast \"10 ms 快速事件测量对象\" 0x30\n"
        "  /begin STRUCTURE_COMPONENT counter C_0 0x0 /end STRUCTURE_COMPONENT\n"
        "/end TYPEDEF_STRUCTURE\n"
        "/begin INSTANCE fast \"10 ms 快速测量组\" Fast 0x0 ECU_ADDRESS_EXTENSION 3 "
        "READ_WRITE /begin IF_DATA XCP /begin DAQ_EVENT FIXED_EVENT_LIST EVENT 0x0 "
        "/end DAQ_EVENT /end IF_DATA /end INSTANCE\n"
        "/begin IF_DATA XCP\n"
        "  /begin SEGMENT 0 2 0 0 0\n"
        "  /begin CHECKSUM XCP_CRC_16_CITT MAX_BLOCK_SIZE 0xFFFF /end CHECKSUM\n"
        "  /end SEGMENT\n"
        "/end IF_DATA\n"
        "/begin IF_DATA XCP\n"
        "/begin PROTOCOL_LAYER\n"
        " 0x102 1000 2000 0 0 0 0 0 248 1024 BYTE_ORDER_MSB_LAST "
        "ADDRESS_GRANULARITY_BYTE\n"
        "OPTIONAL_CMD GET_COMM_MODE_INFO\n"
        "/end PROTOCOL_LAYER\n"
        "/begin XCP_ON_TCP_IP\n"
        "  0x104 5555 ADDRESS \"127.0.0.1\"\n  " +
        tcp_options +
        "\n/end XCP_ON_TCP_IP\n"
        "/end IF_DATA\n"
        "/end MODULE\n"
        "/end PROJECT\n";
    return text;
}

/// @brief A2ML 占位内容：本组件不解析 A2ML，只核对它不是 A2L 片段。
constexpr const char* kAmlBody =
    "/begin A2ML\nstruct Protocol_Layer { enum { X = 1 }; };\n/end A2ML\n";

/// @brief 构造"原文与 SDK 一致"的核对输入。
SdkTcpEndpointFacts MatchingFacts(std::uint8_t alignment) {
    SdkTcpEndpointFacts facts;
    facts.host = "127.0.0.1";
    facts.port = 5555;
    facts.version = 0x104;
    facts.alignment = alignment;
    facts.alignment_declared = true;
    return facts;
}

TEST(A2lTcpCandidate, ExtractsAlignmentAndAbsence) {
    for (int bits : {8, 16, 32}) {
        const auto result = ScanA2lTcpCandidate(
            Sample("PACKET_ALIGNMENT PACKET_ALIGNMENT_" + std::to_string(bits)));
        ASSERT_TRUE(result) << bits;
        EXPECT_EQ(result->alignment, bits);
        EXPECT_EQ(result->port, 5555);
        EXPECT_EQ(result->version, 0x104);
    }
    const auto absent =
        ScanA2lTcpCandidate(Sample("HOST_NAME \"host\" /* PACKET_ALIGNMENT_32 */"));
    ASSERT_TRUE(absent);
    EXPECT_FALSE(absent->alignment) << "注释里的关键字不得当作声明";
    EXPECT_EQ(absent->host, "host");
}

TEST(A2lTcpCandidate, RejectsInputOutsideRestrictedSubset) {
    for (const auto& options :
         {"PACKET_ALIGNMENT PACKET_ALIGNMENT_64",
          "PACKET_ALIGNMENT PACKET_ALIGNMENT_8 PACKET_ALIGNMENT "
          "PACKET_ALIGNMENT_8",
          "HOST_NAME \"a\" HOST_NAME \"b\"", "ADDRESS \"a\" ADDRESS \"b\"",
          "ADDRESS \"/end IF_DATA\"", "/* /end IF_DATA */", "HOST_NAME \"a\\b\"",
          "/begin XCP_ON_TCP_IP 1 1 /end XCP_ON_TCP_IP",
          "IPV6 \"::1\"", "@ PACKET_ALIGNMENT PACKET_ALIGNMENT_8"}) {
        EXPECT_FALSE(ScanA2lTcpCandidate(Sample(options))) << options;
    }
    EXPECT_FALSE(ScanA2lTcpCandidate(Sample("HOST_NAME \"unterminated")));
    // SDK 协议名按原始空白分割：注释不能紧贴或插在协议名前。
    for (const auto& header : {"IF_DATA XCP/*c*/", "IF_DATA /*c*/ XCP",
                               "IF_DATA/*c*/ XCP", "IF_DATA XCP//c\n"}) {
        auto sample = Sample("");
        sample.replace(sample.find("IF_DATA XCP"), 11, header);
        EXPECT_FALSE(ScanA2lTcpCandidate(sample)) << header;
    }
    // 两个 MODULE、两个 TCP 块、XCPplus 与 NUL 都必须拒绝。
    EXPECT_FALSE(ScanA2lTcpCandidate(Sample("") + Sample("")));
    auto two_tcp = RealShapeSample("", false);
    two_tcp.insert(two_tcp.rfind("/end IF_DATA"),
                   "/begin XCP_ON_TCP_IP 0x104 5555 /end XCP_ON_TCP_IP\n");
    EXPECT_FALSE(ScanA2lTcpCandidate(two_tcp));
    auto plus = Sample("");
    plus.replace(plus.find("IF_DATA XCP"), 11, "IF_DATA XCPplus");
    EXPECT_FALSE(ScanA2lTcpCandidate(plus));
    auto nul = Sample("");
    nul.insert(10, 1, '\0');
    EXPECT_FALSE(ScanA2lTcpCandidate(nul));
}

TEST(A2lTcpCandidate, AcceptsRealisticMultiIfDataShape) {
    const auto text = RealShapeSample("PACKET_ALIGNMENT PACKET_ALIGNMENT_32", true);
    // 未提供解析器时，`/include` 无法核对，必须拒绝。
    EXPECT_FALSE(ScanA2lTcpCandidate(text));
    const auto result = ScanA2lTcpCandidate(
        text, [](const std::string&) { return std::optional<std::string>(kAmlBody); });
    ASSERT_TRUE(result) << "真实的 INSTANCE/SEGMENT 级 IF_DATA 不应导致拒绝";
    EXPECT_EQ(result->alignment, 32);
    EXPECT_EQ(result->port, 5555);
    EXPECT_EQ(result->address, "127.0.0.1");
    // 中文只在注释与字符串里出现，不应影响结构判定。
    EXPECT_TRUE(ScanA2lTcpCandidate(RealShapeSample("", false)));
}

TEST(A2lTcpCandidate, RejectsUnverifiableIncludes) {
    const auto text = RealShapeSample("", true);
    EXPECT_FALSE(ScanA2lTcpCandidate(
        text, [](const std::string&) { return std::optional<std::string>(); }))
        << "读不到的包含文件不能被当作已核对";
    EXPECT_FALSE(ScanA2lTcpCandidate(
        text, [](const std::string&) {
            // 非 A2ML 的包含文件可能是 A2L 片段，可能引入第二个端点。
            return std::optional<std::string>(
                "/begin MODULE other \"\" /end MODULE\n");
        }));
    EXPECT_FALSE(ScanA2lTcpCandidate(
        text, [](const std::string&) {
            return std::optional<std::string>(
                "/begin A2ML\n/* XCP_ON_TCP_IP */\n/end A2ML\n");
        }))
        << "包含文件出现端点字样时一律拒绝，避免端点声明跨文件转移";
}

TEST(A2lAlignmentCrossCheck, TrustsOnlyWhenBothSourcesAgree) {
    const auto scan = ScanA2lTcpCandidate(
        RealShapeSample("PACKET_ALIGNMENT PACKET_ALIGNMENT_16", false));
    ASSERT_TRUE(scan);
    ASSERT_TRUE(scan->alignment.has_value());

    const auto trusted =
        CrossCheckA2lTcpAlignment(*scan, {MatchingFacts(16)}, {false});
    EXPECT_EQ(trusted.trust, A2lAlignmentTrust::kTrusted);
    ASSERT_TRUE(trusted.bits.has_value());
    EXPECT_EQ(*trusted.bits, 16);

    // SDK 认为未声明：不能因为原文有值就信任。
    auto undeclared = MatchingFacts(16);
    undeclared.alignment_declared = false;
    EXPECT_EQ(CrossCheckA2lTcpAlignment(*scan, {undeclared}, {false}).trust,
              A2lAlignmentTrust::kUntrusted);

    // 位宽不一致、端口不一致、plus 来源、端点不唯一都不得信任。
    EXPECT_EQ(CrossCheckA2lTcpAlignment(*scan, {MatchingFacts(32)}, {false}).trust,
              A2lAlignmentTrust::kUntrusted);
    auto wrong_port = MatchingFacts(16);
    wrong_port.port = 5556;
    EXPECT_EQ(CrossCheckA2lTcpAlignment(*scan, {wrong_port}, {false}).trust,
              A2lAlignmentTrust::kUntrusted);
    EXPECT_EQ(CrossCheckA2lTcpAlignment(*scan, {MatchingFacts(16)}, {true}).trust,
              A2lAlignmentTrust::kUntrusted);
    EXPECT_EQ(CrossCheckA2lTcpAlignment(*scan, {}, {}).trust,
              A2lAlignmentTrust::kUntrusted);
    EXPECT_EQ(CrossCheckA2lTcpAlignment(*scan, {MatchingFacts(16), MatchingFacts(16)},
                                       {false, false})
                  .trust,
              A2lAlignmentTrust::kUntrusted);

    // 原文确实没声明：结论是"未声明"，不是"8 bit"。
    const auto absent = ScanA2lTcpCandidate(RealShapeSample("", false));
    ASSERT_TRUE(absent);
    const auto not_declared = CrossCheckA2lTcpAlignment(*absent, {MatchingFacts(8)}, {false});
    EXPECT_EQ(not_declared.trust, A2lAlignmentTrust::kNotDeclared);
    EXPECT_FALSE(not_declared.bits.has_value());
}

}  // namespace