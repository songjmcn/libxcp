# =============================================================================
# examples/ —— 独立示例程序目录（LIBXCP_BUILD_EXAMPLES=ON 时进入，批次22 引入）
#
# 示例是交付用的可执行程序：不进 libxcp 静态库、不是 gtest 套件、不引用 tests/。
# 前置门（LIBXCP_BUILD_XCPLITE_SLAVE 与 LIBXCP_BUILD_A2L 必须同时为 ON）在根
# CMakeLists.txt 的门处校验，未满足直接 CONFIGURE 报错，不在本目录降级。
# =============================================================================

# 示例的 ctest 冒烟注册（ExampleXcpMasterUdp）需要本树开启测试采集。
enable_testing()

add_subdirectory(xcp_master_udp)
