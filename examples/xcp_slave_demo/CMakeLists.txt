# =============================================================================
# xcp_slave_demo —— 独立 XCPlite UDP Slave 与显式 A2L 生成示例
#
# 该目标仅在 LIBXCP_BUILD_EXAMPLES=ON 时构建；A2L AML 源路径经 CMake 注入，
# 不向代码写入开发机绝对路径，也不修改 thirdparty/XCPlite。
# =============================================================================

set(XCPLITE_A2L_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/thirdparty/XCPlite")

add_executable(xcp_slave_demo
        main.cpp
        slave_model.cpp
        slave_registry.cpp)
target_compile_features(xcp_slave_demo PRIVATE cxx_std_20)
target_link_libraries(xcp_slave_demo PRIVATE
        xcplite::xcplite
        libxcp::a2lbridge)
target_compile_definitions(xcp_slave_demo PRIVATE
        XCP_104_AML_FILE="${XCPLITE_A2L_INCLUDE_DIR}/XCP_104.aml"
        XCP_THIRDPARTY_DIR="${PROJECT_SOURCE_DIR}/thirdparty")
if(WIN32)
  target_link_libraries(xcp_slave_demo PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(xcp_slave_demo PRIVATE /utf-8)
endif()

# A2L 桥接库可能依赖运行期 liba2l 动态库，复制到示例可执行文件目录。
add_custom_command(TARGET xcp_slave_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:xcp_slave_demo>")

# tests 子目录先于 examples 配置：测试源码和 TARGET_FILE genex 已在
# tests/CMakeLists.txt 注册，此处在目标存在后补全构建依赖。
if(TARGET XcpliteIntegration)
  add_dependencies(XcpliteIntegration xcp_slave_demo)
endif()
