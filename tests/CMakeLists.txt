# ============================================================================
# libxcp 测试构建
#
# 测试框架：GoogleTest（通过 FetchContent 获取，离线环境需预先配置
# CMAKE_FETCHCONTENT 缓存或 GIT_CONFIG 代理）。
#
# 本阶段（设计文档阶段 0~2）覆盖：
#   protocol_types_test.cpp     —— PID 分类、AG 换算、资源掩码、地址推进、名称映射
#   xcp_error_test.cpp          —— 异常字段保留与分类字符串
#   command_codec_test.cpp      —— 8 条命令黄金报文（Intel / Motorola）
#   response_parser_test.cpp    —— RES/ERR/EV/SERV/DTO 分类与字段解析
#   session_test.cpp            —— Session 状态机、参数校验、单 Pending Command
#
# 阶段 3+ 追加：udp_header_codec_test.cpp / udp_transport_test.cpp /
#               memory_access_test.cpp / recovery_test.cpp /
#               xcp_master_integration_test.cpp / xcp_udp_loopback_test.cpp
# ============================================================================

include(FetchContent)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

if(EXISTS "${LIBXCP_GOOGLETEST_LOCAL}/CMakeLists.txt")
  # 使用仓库内预取的离线源，避免在线克隆受本机 git SSL 后端影响
  set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${LIBXCP_GOOGLETEST_LOCAL}" CACHE PATH "" FORCE)
endif()

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(googletest)

add_executable(libxcp_tests
        protocol_types_test.cpp
        xcp_error_test.cpp
        command_codec_test.cpp
        response_parser_test.cpp
        session_test.cpp)

target_link_libraries(libxcp_tests PRIVATE
        libxcp::libxcp
        GTest::gtest
        GTest::gtest_main)

# 测试设施头文件目录（mock_transport.hpp 等），仅 tests 内部可见
target_include_directories(libxcp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
  target_compile_options(libxcp_tests PRIVATE /utf-8)
endif()

include(GoogleTest)
gtest_discover_tests(libxcp_tests DISCOVERY_TIMEOUT 60)
