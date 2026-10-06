#include "xcplite_slave_fixture.hpp"

#include <filesystem>

#include <gtest/gtest.h>

#include "udp_test_slave.hpp"

namespace calmcar::xcp::test {
namespace {

TEST(XcpliteSlaveProcessTest,
     OccupiedPortDoesNotSatisfyReadinessAndRetriesNextCandidate) {
    UdpTestSlave owner;
    ASSERT_NO_THROW(owner.Start());
    const std::uint16_t occupied_port = owner.Port();

    // 延迟首个子进程绑定，确保父进程有机会探测到占用端口上的 XCP 服务。
    XcpliteSlaveProcess contender(occupied_port, 750U);
    ASSERT_NO_THROW(contender.Start());

    EXPECT_NE(contender.Port(), occupied_port);
    EXPECT_TRUE(contender.IsRunning());
    EXPECT_TRUE(std::filesystem::exists(contender.A2lPath()));
    EXPECT_TRUE(std::filesystem::exists(contender.WorkDir() /
                                        "xcp_test_slave.ready"));
}

}  // namespace
}  // namespace calmcar::xcp::test
