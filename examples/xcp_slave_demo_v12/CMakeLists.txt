# =============================================================================
# xcp_slave_demo_v12 —— 基于 XCPlite V1.2（PL 0x0102 / TL 0x0104）的 XCP 从站测试程序
#
# 计划文档：code-plan/XCPliteV1.2_Slave测试程序_实施计划.md
#
# 三条硬约束（文档 §1、§3 已给出源码依据）：
#   1. 禁止 add_subdirectory(thirdparty/XCPliteV1.2)：其 CMakeLists.txt:120 也创建
#      名为 xcplib 的目标，与 V2.2 子项目同名冲突；此处以显式源列表建立独立目标
#      xcplite_v12，且只链接本目录的可执行文件（同一进程不能同时链接两份，C 符号重名）。
#   2. V1.2 的 src/main_cfg.h 默认把 OPTION_CAL_SEGMENTS 注释掉，标定段能力缺失；
#      而 #include "main_cfg.h" 为引号包含、编译器优先搜索包含者所在目录，
#      无法用自带头文件覆盖 —— 改用编译定义注入：-D 先于任何头文件生效，
#      且 src/main_cfg.h 中同名行为注释，不会触发重定义冲突。
#   3. 不修改 thirdparty/XCPliteV1.2 的任何文件（配置全部经编译定义注入）。
# =============================================================================

set(XCPLITE_V12_DIR "${PROJECT_SOURCE_DIR}/thirdparty/XCPliteV1.2")

# 根工程只声明了 LANGUAGES CXX（CMakeLists.txt:26-29），而 V1.2 源码是 C。
# 此处按目录作用域启用 C 语言，避免改动根工程声明。
enable_language(C)

# 源文件清单与 thirdparty/XCPliteV1.2/CMakeLists.txt:113 的 xcplib_SOURCES 保持一致；
# 此处复制该清单而非引用其变量，是因为不把对方目录作为子项目配置。
set(XCPLITE_V12_SOURCES
        "${XCPLITE_V12_DIR}/src/xcpAppl.c"
        "${XCPLITE_V12_DIR}/src/xcpLite.c"
        "${XCPLITE_V12_DIR}/src/xcpEthServer.c"
        "${XCPLITE_V12_DIR}/src/xcpEthTl.c"
        "${XCPLITE_V12_DIR}/src/xcpQueue32.c"
        "${XCPLITE_V12_DIR}/src/xcpQueue64.c"
        "${XCPLITE_V12_DIR}/src/a2l.c"
        "${XCPLITE_V12_DIR}/src/persistence.c"
        "${XCPLITE_V12_DIR}/src/platform.c"
        "${XCPLITE_V12_DIR}/src/xcpLog.c")

add_library(xcplite_v12 STATIC ${XCPLITE_V12_SOURCES})
target_include_directories(xcplite_v12 PUBLIC
        "${XCPLITE_V12_DIR}/inc"
        "${XCPLITE_V12_DIR}/src")  # main_cfg.h / xcp_cfg.h 位于 src/
target_link_libraries(xcplite_v12 PUBLIC Threads::Threads)

# 标定段能力注入（见文件头第 2 条）：未开启时 xcp_cfg.h 不会定义
# XCP_ENABLE_CALSEG_LIST，标定命令会静默不可用。
target_compile_definitions(xcplite_v12 PRIVATE
        OPTION_CAL_SEGMENTS
        OPTION_CAL_SEGMENT_COUNT=8)

if(MSVC)
  # V1.2 源码含非 ASCII 注释，统一按 UTF-8 编译，避免 MSVC 本地代码页告警。
  target_compile_options(xcplite_v12 PRIVATE /utf-8)
endif()

add_executable(xcp_slave_demo_v12
        main.cpp
        slave_model.cpp
        slave_registry.cpp)
target_compile_features(xcp_slave_demo_v12 PRIVATE cxx_std_20)
# libxcp::a2lbridge 仅用于 --verify-a2l 的自校验解析（计划 §5.3 第 6 步）。
target_link_libraries(xcp_slave_demo_v12 PRIVATE
        xcplite_v12
        libxcp::a2lbridge)

# A2L 生成所需的 AML 源文件与 thirdparty 保护目录，经 CMake 注入而非写死路径。
target_compile_definitions(xcp_slave_demo_v12 PRIVATE
        XCP_104_AML_FILE="${PROJECT_SOURCE_DIR}/thirdparty/XCPlite/XCP_104.aml"
        XCP_THIRDPARTY_DIR="${PROJECT_SOURCE_DIR}/thirdparty")

if(WIN32)
  # Windows 下 Winsock 需要显式链接（V1.2 platform.c / xcpEthTl.c 依赖）。
  target_link_libraries(xcp_slave_demo_v12 PRIVATE ws2_32)
endif()
if(MSVC)
  target_compile_options(xcp_slave_demo_v12 PRIVATE /utf-8)
endif()

# 桥接层依赖运行期 liba2l 动态库；复制到可执行文件目录，保证 --verify-a2l 可运行。
add_custom_command(TARGET xcp_slave_demo_v12 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:xcp_slave_demo_v12>")

# C++ Master 通过独立进程启动 V1.2 从站，避免 Python 原始报文探针掩盖
# Master/A2L 集成问题；从站与测试进程分离，避免 V1.2/V2.2 C 符号冲突。
if(LIBXCP_BUILD_TESTS)
  add_executable(XcpliteV12InteropTest
          "${PROJECT_SOURCE_DIR}/tests/xcp_v12_interop_test.cpp"
          "${PROJECT_SOURCE_DIR}/tests/xcp_v12_slave_process.cpp")
  target_compile_features(XcpliteV12InteropTest PRIVATE cxx_std_20)
  target_include_directories(XcpliteV12InteropTest PRIVATE
          "${PROJECT_SOURCE_DIR}"
          "${PROJECT_SOURCE_DIR}/tests"
          "${PROJECT_SOURCE_DIR}/examples/xcp_slave_demo_v12")
  target_link_libraries(XcpliteV12InteropTest PRIVATE
          libxcp::libxcp
          libxcp::a2lbridge
          GTest::gtest
          libxcp_test_main)
  target_compile_definitions(XcpliteV12InteropTest PRIVATE
          XCP_V12_SLAVE_EXECUTABLE="$<TARGET_FILE:xcp_slave_demo_v12>"
          XCP_V12_TEST_ROOT="${PROJECT_BINARY_DIR}/.tmp/xcp_v12_interop")
  add_dependencies(XcpliteV12InteropTest xcp_slave_demo_v12)
  if(MSVC)
    target_compile_options(XcpliteV12InteropTest PRIVATE /utf-8)
  endif()
  gtest_discover_tests(XcpliteV12InteropTest DISCOVERY_TIMEOUT 60
                       PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()
