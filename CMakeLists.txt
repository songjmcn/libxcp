# ============================================================================
# libxcp —— XCP 1.3.0 最小协议核心 + XCP on UDP/IP Transport
#
# 依据设计文档：code-plan/XCP_1.3.0_详细设计_类接口与头文件.md
# 语言标准：C++20；目标平台：Windows / Linux / macOS
# 命名空间：calmcar::xcp
#
# 常用构建命令（相对路径，禁止写死绝对路径）：
#   Release: cmake -B cmake-build-release -S .
#            cmake --build cmake-build-release --config Release
#            ctest --test-dir cmake-build-release -C Release --output-on-failure
#   Debug:   cmake -B cmake-build-debug -S .
#            cmake --build cmake-build-debug --config Debug
#
# 注意：本机（受限沙箱 + CMake 4.0 + VS 生成器）下 `cmake --build -j N` 会在
# MSBuild 并行解析项目引用阶段失败于 gtest.vcxproj -> ZERO_CHECK（报“0 个错误”
# 但退出码非 0）。去掉 -j 使用串行构建即稳定通过；产物本身不受影响。
# ============================================================================

cmake_minimum_required(VERSION 3.24)

project(libxcp
        VERSION 1.3.0
        DESCRIPTION "XCP 1.3.0 minimal protocol core with UDP/IP transport"
        LANGUAGES CXX)

# ----------------------------------------------------------------------------
# 全局构建选项
# ----------------------------------------------------------------------------
option(LIBXCP_BUILD_TESTS "构建 libxcp 单元测试与集成测试" ON)

# ----------------------------------------------------------------------------
# MSVC 工具链定位
#
# 本机 VS 2022 Community 未向 CMake 注册（vswhere 不可用），因此显式指定 x64
# 架构并使用 VS 生成器。Ninja 生成器在本机沙箱下会挂起：ninja 需要派生子进程
# 执行编译命令，而该管道被沙箱阻断（连 `cmake -E echo` 规则都不返回）。
# 换机器时可直接命令行覆盖：
#   cmake -B build -S . -G Ninja（需先加载 vcvars64.bat）
# ----------------------------------------------------------------------------
if(WIN32 AND NOT DEFINED CMAKE_GENERATOR)
  set(CMAKE_GENERATOR "Visual Studio 17 2022" CACHE STRING "构建生成器" FORCE)
endif()

# ----------------------------------------------------------------------------
# 离线依赖源（可选）
#
# 若本机 git 的 schannel SSL 后端无法取得凭据句柄，FetchContent 在线拉取
# googletest 会长时间挂起。此时可把源码预先放到仓库的 .deps-cache/ 目录：
#   .deps-cache/googletest/            （googletest v1.15.2 源码）
# 存在时自动改用本地源，不改动任何用户级 git/CMake 配置；不存在则回退在线拉取。
# ----------------------------------------------------------------------------
set(LIBXCP_GOOGLETEST_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/.deps-cache/googletest")

# 默认构建类型：未显式指定时按行为良好地设为 Release（缓存变量，不覆盖用户输入）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # 将源文件字符集声明为 UTF-8，避免中文注释在非英文 Windows 控制台下解析异常
  add_compile_options(/utf-8 /W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(GNUInstallDirs)

# ----------------------------------------------------------------------------
# libxcp 静态库
# ----------------------------------------------------------------------------
add_library(libxcp STATIC)
add_library(libxcp::libxcp ALIAS libxcp)

target_sources(libxcp PRIVATE
        src/protocol_types.cpp
        src/xcp_error.cpp
        src/command_codec.cpp
        src/response_parser.cpp
        src/session.cpp
        src/udp_header_codec.cpp
        src/udp_transport.cpp
        src/command_executor.cpp
        src/memory_access.cpp
        src/xcp_master.cpp)

target_include_directories(libxcp
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# 协议核心仅依赖线程库（Session/Executor 内部同步）；
# UdpTransport 使用平台 Socket，Windows 需显式链接 ws2_32。
find_package(Threads REQUIRED)
target_link_libraries(libxcp PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(libxcp PUBLIC ws2_32)
endif()

set_target_properties(libxcp PROPERTIES
        OUTPUT_NAME xcp
        POSITION_INDEPENDENT_CODE ON)

# ----------------------------------------------------------------------------
# 安装规则
# ----------------------------------------------------------------------------
install(TARGETS libxcp
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/libxcp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ----------------------------------------------------------------------------
# A2L 桥接层（最小开发验证集；见 code-plan/A2L_集成_a2llib选型与适配层详细设计_R4.md）
#
# 批次9 起 a2lbridge 并入 thirdparty/a2l-sdk 工程：build-sdk.ps1 一次产出
# liba2l.dll/lib + libxcp_a2lbridge.lib + include/liba2l 与 include/libxcp 头树。
# 本工程不编译任何 thirdparty 源码，只创建 IMPORTED target 消费准备根产物。
# 默认 OFF：不改变原构建。开启时必须显式提供准备根路径
# （LIBXCP_LIBA2L_ROOT），不做环境变量/注册表/.deps-cache 回退猜测。
# ----------------------------------------------------------------------------
option(LIBXCP_BUILD_A2L "Consume the A2L bridge artifacts" OFF)
set(LIBXCP_LIBA2L_ROOT "" CACHE PATH "Prepared liba2l root")

if(LIBXCP_BUILD_A2L)
  if(NOT LIBXCP_LIBA2L_ROOT)
    message(FATAL_ERROR
            "LIBXCP_BUILD_A2L=ON requires LIBXCP_LIBA2L_ROOT "
            "(prepared by thirdparty/a2l-sdk/build-sdk.ps1)")
  endif()

  find_path(LIBXCP_LIBA2L_INCLUDE_DIR
            NAMES liba2l/liba2l_api.hpp
            PATHS "${LIBXCP_LIBA2L_ROOT}/include"
            NO_DEFAULT_PATH REQUIRED)
  find_library(LIBXCP_LIBA2L_LIBRARY
               NAMES liba2l
               PATHS "${LIBXCP_LIBA2L_ROOT}/lib"
               NO_DEFAULT_PATH REQUIRED)
  find_file(LIBXCP_LIBA2L_RUNTIME_DLL
            NAMES liba2l.dll
            PATHS "${LIBXCP_LIBA2L_ROOT}/bin"
            NO_DEFAULT_PATH REQUIRED)

  add_library(liba2l::liba2l SHARED IMPORTED GLOBAL)
  set_target_properties(liba2l::liba2l PROPERTIES
      IMPORTED_IMPLIB "${LIBXCP_LIBA2L_LIBRARY}"
      IMPORTED_LOCATION "${LIBXCP_LIBA2L_RUNTIME_DLL}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBXCP_LIBA2L_INCLUDE_DIR}")

  # 桥接层：SDK 工程编好的静态库 + 公开头树（IMPORTED 消费，见上注释）。
  find_path(LIBXCP_A2LBRIDGE_INCLUDE_DIR
            NAMES libxcp/a2l/a2l_bridge.hpp
            PATHS "${LIBXCP_LIBA2L_ROOT}/include"
            NO_DEFAULT_PATH REQUIRED)
  find_library(LIBXCP_A2LBRIDGE_LIBRARY
               NAMES libxcp_a2lbridge
               PATHS "${LIBXCP_LIBA2L_ROOT}/lib"
               NO_DEFAULT_PATH REQUIRED)

  add_library(libxcp::a2lbridge STATIC IMPORTED GLOBAL)
  set_target_properties(libxcp::a2lbridge PROPERTIES
      IMPORTED_LOCATION "${LIBXCP_A2LBRIDGE_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBXCP_A2LBRIDGE_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES liba2l::liba2l)
endif()

# ----------------------------------------------------------------------------
# 测试
# ----------------------------------------------------------------------------
if(LIBXCP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
