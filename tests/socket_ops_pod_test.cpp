/// @file socket_ops_pod_test.cpp
/// @brief 平台无关 IPv4 POD 和解析注入的确定性回归。

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "src/socket_util.hpp"

namespace calmcar::xcp {
namespace {

/// @brief 注入两条候选地址，避免测试依赖 DNS 网络状态。
int TwoCandidates(const char* node, const char* service,
                  detail::SocketKind kind,
                  std::vector<detail::Ipv4Endpoint>* endpoints,
                  std::string* error_out) {
    if (node == nullptr || service == nullptr ||
        kind != detail::SocketKind::Tcp || endpoints == nullptr) {
        if (error_out != nullptr) {
            *error_out = "测试解析器参数错误";
        }
        return -1;
    }
    endpoints->assign({{{127, 0, 0, 1}, 1001}, {{127, 0, 0, 2}, 1002}});
    if (error_out != nullptr) {
        error_out->clear();
    }
    return 0;
}

/// @brief 注入失败并故意保留候选，验证公共分发层清理输出。
int FailingResolver(const char*, const char*, detail::SocketKind,
                    std::vector<detail::Ipv4Endpoint>* endpoints,
                    std::string*) {
    endpoints->push_back({{192, 0, 2, 1}, 80});
    return -7;
}

/// @brief 注入成功但无候选的解析器，模拟违反约定的替身。
int EmptyResolver(const char*, const char*, detail::SocketKind,
                  std::vector<detail::Ipv4Endpoint>* endpoints, std::string*) {
    endpoints->clear();
    return 0;
}

/// @brief 确保全局解析钩子在测试结束前恢复，避免污染其它用例。
class ResolverGuard {
public:
    /// @brief 安装 POD 解析替身。
    explicit ResolverGuard(detail::NameResolver resolver) {
        detail::SetNameResolverForTest(resolver);
    }
    /// @brief 卸载替身。
    ~ResolverGuard() { detail::SetNameResolverForTest(nullptr); }
    ResolverGuard(const ResolverGuard&) = delete;
    ResolverGuard& operator=(const ResolverGuard&) = delete;
};

TEST(SocketOpsPodTest, ParsesAndFormatsNetworkOrderedIpv4) {
    detail::Ipv4Address ip{};
    ASSERT_TRUE(detail::ParseIpv4Literal("127.0.0.1", &ip));
    EXPECT_EQ((detail::Ipv4Address{127, 0, 0, 1}), ip);
    EXPECT_EQ(detail::FormatIpv4(ip), "127.0.0.1");
    ASSERT_TRUE(detail::ParseIpv4Literal("0.0.0.0", &ip));
    EXPECT_EQ((detail::Ipv4Address{0, 0, 0, 0}), ip);
    EXPECT_FALSE(detail::ParseIpv4Literal("256.0.0.1", &ip));
    EXPECT_FALSE(detail::ParseIpv4Literal("001.0.0.1", &ip));
    EXPECT_FALSE(detail::ParseIpv4Literal("127.0.0.1x", &ip));
    EXPECT_FALSE(detail::ParseIpv4Literal("", &ip));
    EXPECT_FALSE(detail::ParseIpv4Literal("127.0.0.1", nullptr));
}

TEST(SocketOpsPodTest, ResolvesNumericIpv4WithHostOrderedPort) {
    std::vector<detail::Ipv4Endpoint> endpoints;
    std::string error;
    ASSERT_EQ(
        detail::ResolveIpv4Candidates(
            "127.0.0.1", "42345", detail::SocketKind::Tcp, &endpoints, &error),
        0)
        << error;
    ASSERT_FALSE(endpoints.empty());
    EXPECT_EQ(endpoints.front().address, (detail::Ipv4Address{127, 0, 0, 1}));
    EXPECT_EQ(endpoints.front().port, 42345U);
}

TEST(SocketOpsPodTest, ResolutionFailureClearsOldCandidates) {
    std::vector<detail::Ipv4Endpoint> endpoints{{{127, 0, 0, 1}, 1234}};
    std::string error;
    // 数字地址 + 未知服务名：解析必然失败，且不会触发 DNS 查询。
    EXPECT_NE(detail::ResolveIpv4Candidates("127.0.0.1", "no-such-service",
                                            detail::SocketKind::Udp, &endpoints,
                                            &error),
              0);
    EXPECT_TRUE(endpoints.empty());
    EXPECT_FALSE(error.empty());
}

TEST(SocketOpsPodTest, InjectedFailureClearsPartialCandidatesAndReportsError) {
    ResolverGuard guard(&FailingResolver);
    std::vector<detail::Ipv4Endpoint> endpoints{{{127, 0, 0, 1}, 1234}};
    std::string error;
    EXPECT_EQ(detail::ResolveIpv4Candidates("example.invalid", "80",
                                            detail::SocketKind::Tcp, &endpoints,
                                            &error),
              -7);
    EXPECT_TRUE(endpoints.empty());
    EXPECT_FALSE(error.empty());
}

TEST(SocketOpsPodTest, RejectsInjectedEmptySuccess) {
    ResolverGuard guard(&EmptyResolver);
    std::vector<detail::Ipv4Endpoint> endpoints;
    std::string error;
    EXPECT_NE(detail::ResolveIpv4Candidates("example.invalid", "80",
                                            detail::SocketKind::Tcp, &endpoints,
                                            &error),
              0);
    EXPECT_TRUE(endpoints.empty());
    EXPECT_FALSE(error.empty());
}

TEST(SocketOpsPodTest, UsesInjectedCandidateListWithoutNativePointers) {
    ResolverGuard guard(&TwoCandidates);
    EXPECT_EQ(detail::CurrentNameResolver(), &TwoCandidates);
    std::vector<detail::Ipv4Endpoint> endpoints;
    std::string error;
    ASSERT_EQ(detail::ResolveIpv4Candidates("test.invalid", "1001",
                                            detail::SocketKind::Tcp, &endpoints,
                                            &error),
              0)
        << error;
    ASSERT_EQ(endpoints.size(), 2U);
    EXPECT_EQ(endpoints[0].address, (detail::Ipv4Address{127, 0, 0, 1}));
    EXPECT_EQ(endpoints[0].port, 1001U);
    EXPECT_EQ(endpoints[1].port, 1002U);
}

}  // namespace
}  // namespace calmcar::xcp
