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

# 测量子系统端到端示例（v0.9，测量子系统代码增长计划 交付物3）：
# UPLOAD A2L → 桥接 → 适配器 → MeasurementSession → 物理值帧。
# 另需 xcp_test_slave 目标（LIBXCP_BUILD_TESTS=ON，其目录在根构建中先于本
# 目录加入），缺位在子目录 CONFIGURE 期 FATAL_ERROR 明示。
add_subdirectory(measurement_demo)
