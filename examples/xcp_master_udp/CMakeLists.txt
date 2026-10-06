# =============================================================================
# xcp_master_udp —— 以 UDP 连接上游 XCPlite cpp_demo 原样对手端的 Master 示例
#
# 计划依据：code-plan/XCPlite_cpp_demo_示例对手端计划.md（v2.1，批次22）。
# 一条命令端到端：示例自行拉起/终止 cpp_demo，内部完成
# 「Run A 生成 A2L → 桥接加载 → Run B 测量 + 写回读」两轮流程。
#
# 两个目标：
#   cpp_demo       —— 对手 Slave：直接引用 thirdparty 示例源文件编译，
#                     零修改第三方文件。自备目标的原因：根门固定
#                     XCPLITE_BUILD_EXAMPLES=OFF（打开会构建全部上游示例，
#                     Windows 不一定全可编），这里只取本示例需要的一个。
#   xcp_master_udp —— 示例本体：链接 libxcp::libxcp + libxcp::a2lbridge。
# =============================================================================

set(CPP_DEMO_SRC_DIR "${PROJECT_SOURCE_DIR}/thirdparty/XCPlite/examples/cpp_demo/src")
# 上游 A2L 生成的 include 文件（a2l_writer.c:474 固定写 /include "XCP_104.aml"）
set(XCPLITE_A2L_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/thirdparty/XCPlite")

add_executable(cpp_demo
        "${CPP_DEMO_SRC_DIR}/main.cpp"
        "${CPP_DEMO_SRC_DIR}/sig_gen.cpp"
        "${CPP_DEMO_SRC_DIR}/lookup.cpp")
target_compile_features(cpp_demo PRIVATE cxx_std_20)
target_link_libraries(cpp_demo PRIVATE xcplite)
if(MSVC)
  target_compile_options(cpp_demo PRIVATE /utf-8)
endif()

add_executable(xcp_master_udp
        xcp_master_udp.cpp
        demo_process.cpp)
target_compile_features(xcp_master_udp PRIVATE cxx_std_20)
target_link_libraries(xcp_master_udp PRIVATE
        libxcp::libxcp
        libxcp::a2lbridge)
# 示例定位 cpp_demo 可执行文件：经 CMake 注入，禁止硬编码
# （沿用 tests/CMakeLists.txt:84-96 XCPLITE_SLAVE_EXECUTABLE 先例）
target_compile_definitions(xcp_master_udp PRIVATE
        CPP_DEMO_EXECUTABLE="$<TARGET_FILE:cpp_demo>"
    XCP_104_AML_FILE="${XCPLITE_A2L_INCLUDE_DIR}/XCP_104.aml")
add_dependencies(xcp_master_udp cpp_demo)
if(MSVC)
  target_compile_options(xcp_master_udp PRIVATE /utf-8)
endif()

# 运行期寻址：把 liba2l.dll 复制到可执行文件目录（沿用 tests/CMakeLists.txt:146-148）
add_custom_command(TARGET xcp_master_udp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:xcp_master_udp>")

# ctest 冒烟（C4 批准口径）：非 gtest 裸可执行 + 退出码；
# RUN_SERIAL 降低端口冲突概率；5555 被占 → 示例预检 exit 2 → 计 FAIL 暴露环境问题。
add_test(NAME ExampleXcpMasterUdp
        COMMAND xcp_master_udp --run-dir "${CMAKE_CURRENT_BINARY_DIR}/ctest_run")
set_tests_properties(ExampleXcpMasterUdp PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 180)
