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
        src/session.cpp)

# 阶段 3 起启用（UDP Transport 实现）：
#   src/udp_header_codec.cpp
#   src/udp_transport.cpp

target_include_directories(libxcp
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# 协议核心仅依赖线程库（Session/Executor 内部同步），不依赖任何 Socket API
find_package(Threads REQUIRED)
target_link_libraries(libxcp PUBLIC Threads::Threads)

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
# 测试
# ----------------------------------------------------------------------------
if(LIBXCP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
