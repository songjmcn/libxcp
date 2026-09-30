# =============================================================================
# measurement_demo —— 测量子系统端到端示例（v0.9）
#
# 计划依据：code-plan/libxcp_测量子系统代码增长计划.md v0.9 交付物 3。
# 一条命令端到端：示例自行拉起/终止 xcp_test_slave，内部完成
# 「UPLOAD 拉回 A2L → 桥接加载 → 适配器 → MeasurementSession → 物理值帧」。
#
# 依赖：
#   - libxcp::libxcp（核心：XcpMaster + MeasurementSession）
#   - libxcp_measurement_adapter（A2L→核心窄接口适配器，LIBXCP_BUILD_A2L 门）
#   - libxcp::a2lbridge（经适配器 PUBLIC 传播）
#   - xcp_test_slave 可执行（tests/xcp_test_slave/，需 LIBXCP_BUILD_TESTS=ON
#     ——该目标在 tests 子目录定义；缺位时 CONFIGURE 期 FATAL_ERROR 明示，
#     不静默降级、不在本目录二次定义）
#   - demo_process.cpp 复用 examples/xcp_master_udp/ 的自包含子进程封装
#     （示例目录内部共享，不引用 tests/ 源码）
# =============================================================================

if(NOT TARGET xcp_test_slave)
  message(FATAL_ERROR
          "measurement_demo requires target xcp_test_slave "
          "(LIBXCP_BUILD_TESTS=ON in addition to the examples prerequisites)")
endif()

add_executable(measurement_demo
        measurement_demo.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../xcp_master_udp/demo_process.cpp")
target_compile_features(measurement_demo PRIVATE cxx_std_20)
target_link_libraries(measurement_demo PRIVATE
        libxcp::libxcp
        libxcp_measurement_adapter)
# Slave 可执行文件路径经 CMake 注入，禁止硬编码
# （沿用 examples/xcp_master_udp/CMakeLists.txt:37-41 先例）
# XCP_104.aml 路径经 CMake 注入（照抄 examples/xcp_master_udp/CMakeLists.txt:18,41
# 先例）：cpp_demo 生成的 A2L 含 `/include "XCP_104.aml"`，示例在 run-dir 预置副本
# 保证桥接 include 链可解析（xcp_test_slave 自写 AML，预置块以 skip_existing 让位）。
set(XCPLITE_A2L_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/thirdparty/XCPlite")
target_compile_definitions(measurement_demo PRIVATE
        XCP_TEST_SLAVE_EXECUTABLE="$<TARGET_FILE:xcp_test_slave>"
        XCP_104_AML_FILE="${XCPLITE_A2L_INCLUDE_DIR}/XCP_104.aml")
# demo_process.hpp 的搜索路径（与 xcp_master_udp 共享实现）
target_include_directories(measurement_demo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../xcp_master_udp")
add_dependencies(measurement_demo xcp_test_slave)
if(MSVC)
  target_compile_options(measurement_demo PRIVATE /utf-8)
endif()

# 运行期寻址：把 liba2l.dll 复制到可执行文件目录（沿用 :47-51 先例）
add_custom_command(TARGET measurement_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:measurement_demo>")

# ctest 冒烟：非 gtest 裸可执行 + 退出码；RUN_SERIAL 与 ExampleXcpMasterUdp
# 同口径（端口不同：5556 vs 5555，仍串行降概率）；5556 被占 → 预检 exit 2 →
# 计 FAIL 暴露环境问题。
add_test(NAME ExampleMeasurementDemo
        COMMAND measurement_demo --run-dir "${CMAKE_CURRENT_BINARY_DIR}/ctest_run")
set_tests_properties(ExampleMeasurementDemo PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 180)
