# =============================================================================
# xcp_master_tcp —— TCP Master 端到端示例与黑盒集成测试
#
# 该目录位于 xcp_slave_demo 之后配置，以便生成器/服务端可通过目标路径注入。
# =============================================================================

add_executable(xcp_master_tcp
        xcp_master_tcp.cpp
        ../xcp_master_udp/demo_process.cpp)
target_compile_features(xcp_master_tcp PRIVATE cxx_std_20)
target_link_libraries(xcp_master_tcp PRIVATE
        libxcp::libxcp
        libxcp::a2lbridge)
target_compile_definitions(xcp_master_tcp PRIVATE
        XCP_SLAVE_DEMO_EXECUTABLE="$<TARGET_FILE:xcp_slave_demo>")
add_dependencies(xcp_master_tcp xcp_slave_demo)
libxcp_configure_program_logging(xcp_master_tcp)

if(WIN32)
  target_link_libraries(xcp_master_tcp PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(xcp_master_tcp PRIVATE /utf-8)
endif()

# A2L 桥接库的运行期动态库与示例程序放在一起，免去用户手动设置 PATH。
add_custom_command(TARGET xcp_master_tcp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:xcp_master_tcp>")

# 黑盒测试位于 tests/，覆盖自动模式及外部端点写入保留值。
if(LIBXCP_BUILD_TESTS)
  add_executable(XcpMasterTcpIntegration
          "${PROJECT_SOURCE_DIR}/tests/xcp_master_tcp_integration_test.cpp"
          "${PROJECT_SOURCE_DIR}/examples/xcp_master_udp/demo_process.cpp")
  target_compile_features(XcpMasterTcpIntegration PRIVATE cxx_std_20)
  target_link_libraries(XcpMasterTcpIntegration PRIVATE
          libxcp::libxcp
          libxcp::a2lbridge
          GTest::gtest
          libxcp_test_main)
  target_include_directories(XcpMasterTcpIntegration PRIVATE
          "${PROJECT_SOURCE_DIR}"
          "${PROJECT_SOURCE_DIR}/tests")
  target_compile_definitions(XcpMasterTcpIntegration PRIVATE
          XCP_MASTER_TCP_EXECUTABLE="$<TARGET_FILE:xcp_master_tcp>"
          XCP_SLAVE_DEMO_EXECUTABLE="$<TARGET_FILE:xcp_slave_demo>")
  add_dependencies(XcpMasterTcpIntegration xcp_master_tcp xcp_slave_demo)
  if(WIN32)
    target_link_libraries(XcpMasterTcpIntegration PRIVATE ws2_32)
  endif()
  if(MSVC)
    target_compile_options(XcpMasterTcpIntegration PRIVATE /utf-8)
  endif()
  add_custom_command(TARGET XcpMasterTcpIntegration POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "$<TARGET_FILE:liba2l::liba2l>"
                  "$<TARGET_FILE_DIR:XcpMasterTcpIntegration>")
  include(GoogleTest)
  gtest_discover_tests(XcpMasterTcpIntegration
          DISCOVERY_TIMEOUT 60
          PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
endif()
