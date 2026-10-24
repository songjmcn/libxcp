# =============================================================================
# examples/ —— 独立示例程序目录（LIBXCP_BUILD_EXAMPLES=ON 时进入，批次22 引入）
#
# 示例是交付用的可执行程序：不进 libxcp 静态库，也不作为 gtest 测试目标；
# 示例集成测试仍由 tests/ 定义，并可按需依赖相应示例可执行文件。
# 前置依赖 LIBXCP_BUILD_XCPLITE_SLAVE 由根 CMakeLists.txt 自动跟随打开
# （EXAMPLES=ON ⇒ SLAVE=ON），A2L 桥接层现为必编组件无需开关——本目录不再校验。
# =============================================================================

# 示例的 ctest 冒烟注册（ExampleXcpMasterUdp）需要本树开启测试采集。
enable_testing()

add_subdirectory(xcp_master_udp)

# 测量子系统端到端示例（v0.9，测量子系统代码增长计划 交付物3）：
# UPLOAD A2L → 桥接 → 适配器 → MeasurementSession → 物理值帧。
# 默认对手端 cpp_demo 已由上面的 xcp_master_udp 子目录定义；不依赖 tests/。
add_subdirectory(measurement_demo)

# 通用 XCP 测量与标定集成示例：目标 profile 明确提供 event/envelope 元数据。
add_subdirectory(xcp_integration_demo)

# 独立 XCPlite Slave：可运行 UDP 服务或显式生成 A2L 后退出。
add_subdirectory(xcp_slave_demo)

# 基于 XCPlite V1.2（协议层 1.2 / 传输层 1.4）的从站测试程序：自带独立静态库
# xcplite_v12，不复用 V2.2 的 xcplite 目标（原因见该目录 CMakeLists.txt 文件头）。
add_subdirectory(xcp_slave_demo_v12)
