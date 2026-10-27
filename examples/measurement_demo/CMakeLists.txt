# =============================================================================
# measurement_demo —— 测量子系统端到端示例（v0.9）
#
# 计划依据：code-plan/libxcp_测量子系统代码增长计划.md v0.9 交付物 3。
# 一条命令端到端：示例自行拉起/终止 examples/xcp_master_udp 提供的 cpp_demo，内部完成
# 「UPLOAD 拉回 A2L → 桥接加载 → 适配器 → MeasurementSession → 物理值帧」。
#
# 依赖：
#   - libxcp::libxcp（核心：XcpMaster + MeasurementSession）
#   - libxcp_measurement_adapter（A2L→核心窄接口适配器；A2L 栈为必编组件）
#   - libxcp::a2lbridge（经适配器 PUBLIC 传播）
#   - cpp_demo 可执行（由同级 examples/xcp_master_udp/ 定义）；示例可用
#     --slave 覆盖默认对手端，不依赖 tests/ 目标或源码
#   - demo_process.cpp 复用 examples/xcp_master_udp/ 的自包含子进程封装
# =============================================================================

add_executable(measurement_demo
        measurement_demo.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../xcp_master_udp/demo_process.cpp")
target_compile_features(measurement_demo PRIVATE cxx_std_20)
target_link_libraries(measurement_demo PRIVATE
        libxcp::libxcp
        libxcp_measurement_adapter)
libxcp_configure_program_logging(measurement_demo)
# cpp_demo 可执行文件路径经 CMake 注入，禁止硬编码
# （沿用 examples/xcp_master_udp/CMakeLists.txt:37-41 先例）
# XCP_104.aml 路径经 CMake 注入（照抄 examples/xcp_master_udp/CMakeLists.txt:18,41
# 先例）：cpp_demo 生成的 A2L 含 `/include "XCP_104.aml"`，示例在 run-dir 预置副本
# 保证 cpp_demo 上传 A2L 的 include 链可解析，已存在文件不覆盖。
set(XCPLITE_A2L_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/thirdparty/XCPlite")
target_compile_definitions(measurement_demo PRIVATE
        CPP_DEMO_EXECUTABLE="$<TARGET_FILE:cpp_demo>"
        XCP_104_AML_FILE="${XCPLITE_A2L_INCLUDE_DIR}/XCP_104.aml")
# demo_process.hpp 的搜索路径（与 xcp_master_udp 共享实现）
target_include_directories(measurement_demo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../xcp_master_udp")
add_dependencies(measurement_demo cpp_demo)
if(MSVC)
  target_compile_options(measurement_demo PRIVATE /utf-8)
endif()

# 运行期寻址：把 liba2l.dll 复制到可执行文件目录（沿用 :47-51 先例）
add_custom_command(TARGET measurement_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:measurement_demo>")

# ctest 默认冒烟：非 gtest 裸可执行 + 退出码；与 ExampleXcpMasterUdp 共用 5555，
# 均设 RUN_SERIAL 避免端口并发；端口被占时预检 exit 2，作为环境错误暴露。
# temperature 的变化由从站 mainloop counter 回绕驱动（counter_max=1000、1 ms/轮 →
# 约 1 s 一次），慢速 CI runner 上 4 s 窗口可能一次都不回绕，故用 --static-symbols
# 只断言覆盖、跳过"窗口内 ≥2 个不同原始值"的变化性断言。
add_test(NAME ExampleMeasurementDemo
        COMMAND measurement_demo
                --run-dir "${CMAKE_CURRENT_BINARY_DIR}/ctest_run"
                --static-symbols temperature)
set_tests_properties(ExampleMeasurementDemo PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 180)

# cpp_demo 多符号专项回归：检验多事件路由及同一 DAQ 样本的 raw/物理值换算；
# 与默认零参数冒烟保持独立 run-dir。
add_test(NAME ExampleCppDemoMeasurement
        COMMAND measurement_demo
                --slave "$<TARGET_FILE:cpp_demo>"
                --run-dir "${CMAKE_CURRENT_BINARY_DIR}/cpp_demo_ctest_run"
                --port 5555 --project cpp_demo
                --symbols temperature,speed,counter,sum,SigGen1.value_,SigGen2.value_
                --event mainloop --mode dynamic
                --range temperature=-50:100 --range speed=0:250
                --range counter=0:65535 --range sum=-100:100
                --range SigGen1.value_=-100:100 --range SigGen2.value_=-100:100
                --static-symbols temperature
                --seconds 4)
set_tests_properties(ExampleCppDemoMeasurement PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 180)

# 在线标定效果专项：独立 run-dir、同一 DAQ 会话中改变 counter_max 并恢复。
add_test(NAME ExampleCppDemoCalibration
        COMMAND measurement_demo
                --slave "$<TARGET_FILE:cpp_demo>"
                --run-dir "${CMAKE_CURRENT_BINARY_DIR}/cpp_demo_calibration_ctest_run"
                --port 5555 --project cpp_demo --symbols temperature,counter
                --event mainloop --mode dynamic --range temperature=-50:100
                --range counter=0:1000 --static-symbols temperature
                --seconds 4 --calibration-probe)
set_tests_properties(ExampleCppDemoCalibration PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 180)
