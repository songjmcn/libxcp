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
        src/xcp_master.cpp
        src/daq/dto_envelope_decoder.cpp
        src/daq/dto_payload_decoder.cpp
        src/daq/daq_timestamp.cpp
        src/measurement/measurement_planner.cpp
        src/measurement/measurement_session.cpp)

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
# A2L 桥接层（必编组件；见 code-plan/A2L_集成_a2llib选型与适配层详细设计_R4.md）
#
# A2L 栈随主工程无条件构建：thirdparty/a2l-sdk 作为子目录直接编入主工程树
# （add_subdirectory），无需开关、无需先跑 build-sdk.ps1 生成"准备根"、无需
# 指定任何路径变量。a2l-sdk 工程自身产出 uchardet 桩 → 上游 a2l(static)
# → liba2l(SHARED) → libxcp_a2lbridge(static)，其 ALIAS 目标
# liba2l::liba2l / libxcp::a2lbridge 在主树直接可见，tests/examples 直接消费。
#
# Boost 因此成为硬依赖（configure 期解析）：优先用用户显式传入的
# -DBoost_DIR=<...>/cmake/Boost-<ver>；未传入时按 C:\boost、BOOST_ROOT 环境变量
# 等常见位置搜索 Config 包，找不到直接 FATAL_ERROR 给出明确指引。多平台约定：
# Linux/macOS 走系统 Boost 或用户 -DBoost_DIR，Windows 本机约定
# C:\boost\lib\cmake\Boost-*。
#
# build-sdk.ps1 保留为独立 SDK 开发入口（预构建产物场景），不再是主树前置步骤。
# ----------------------------------------------------------------------------

# --- Boost 自动发现 ---------------------------------------------------------
# 上游 a2llib/script/boost.cmake 在 Boost_FOUND 已置位时会跳过自身查找，
# 因此这里先行 find_package 即可统一决定 Boost 来源（幂等，缓存到主树）。
if(NOT Boost_DIR AND NOT Boost_FOUND)
  set(_libxcp_boost_search_hints "")
  # Windows 本机约定：C:/boost/lib/cmake/Boost-<ver>（存在多个版本取最新）
  if(WIN32 AND EXISTS "C:/boost/lib/cmake")
    file(GLOB _libxcp_boost_cfg_dirs "C:/boost/lib/cmake/Boost-*")
    if(_libxcp_boost_cfg_dirs)
      list(SORT _libxcp_boost_cfg_dirs COMPARE STRING ORDER DESCENDING)
      list(GET _libxcp_boost_cfg_dirs 0 _libxcp_boost_hint)
      string(APPEND _libxcp_boost_search_hints "${_libxcp_boost_hint};")
      message(STATUS "libxcp: 发现候选 Boost_DIR=${_libxcp_boost_hint}")
    endif()
  endif()
  # 其次尊重 BOOST_ROOT / Boost_ROOT 环境变量（多平台通用约定）
  if(DEFINED ENV{BOOST_ROOT})
    foreach(_sub IN ITEMS lib/cmake "" )
      file(GLOB _libxcp_boost_env_dirs "$ENV{BOOST_ROOT}/${_sub}/Boost-*")
      foreach(_d IN LISTS _libxcp_boost_env_dirs)
        string(APPEND _libxcp_boost_search_hints "${_d};")
      endforeach()
    endforeach()
    unset(_libxcp_boost_env_dirs)
  endif()
  unset(_libxcp_boost_hint)
  unset(_libxcp_boost_cfg_dirs)
endif()

# CONFIG 模式 + NO_CMAKE_ENVIRONMENT_PATH：只认显式 -DBoost_DIR 与上述搜索
# 提示，忽略 vcpkg 等环境注入的 Boost_ROOT，避免误选缺 locale/process 组件
# 的发行版（本机实测 vcpkg 全局集成会劫持裸 find_package）。
find_package(Boost CONFIG COMPONENTS locale filesystem process
             HINTS ${_libxcp_boost_search_hints} NO_CMAKE_ENVIRONMENT_PATH)
if(NOT Boost_FOUND)
  message(FATAL_ERROR
          "libxcp requires Boost (CONFIG package with components "
          "locale, filesystem, process) for the mandatory A2L stack.\n"
          "Install Boost or pass -DBoost_DIR=<prefix>/lib/cmake/Boost-<version> "
          "(e.g. C:/boost/lib/cmake/Boost-1.86.0 on this machine).")
endif()
unset(_libxcp_boost_search_hints)

# --- a2l-sdk 子工程（真实 target，非 IMPORTED） ------------------------------
# a2l-sdk 内部按 target 设置 C++23（上游 a2llib 要求），主工程保持 C++20；
# liba2l 为 SHARED 边界，标准差异不影响 ABI。a2lbridge 本身即 C++20。
add_subdirectory(thirdparty/a2l-sdk)

# --- 测量适配器（v0.5，测量子系统代码增长计划 §5.5 / D1）--------------------
# 独立顶层目录 adapter/a2l + 独立 target libxcp_measurement_adapter。
# 副产物不并入 libxcp；adapter/*.cpp 是核心<->桥接层唯一接触点
# （A2lIsolation S2 仅扫描 include/ 与 src/，本目录不在扫描范围）。
add_library(libxcp_measurement_adapter STATIC
        adapter/a2l/a2l_measurement_database.cpp
        adapter/a2l/measurement_runtime_profile.cpp)
target_include_directories(libxcp_measurement_adapter PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapter>)
target_link_libraries(libxcp_measurement_adapter PUBLIC
        libxcp::libxcp libxcp::a2lbridge)

# ----------------------------------------------------------------------------
# 构建开关（先于子目录声明；判定与 add_subdirectory 分离，见下方各段）
#
# EXAMPLES 门依赖 XCPlite 子项目——其 option 必须出现在 XCPlite 段的跟随
# 判定之前，故在此集中声明（L30 的 LIBXCP_BUILD_TESTS 保持原位）。
# ----------------------------------------------------------------------------
option(LIBXCP_BUILD_EXAMPLES
       "Build the standalone example programs under examples/"
       OFF)

# ----------------------------------------------------------------------------
# XCPlite 协议调试集成（code-plan/XCPlite_Slave_协议调试集成计划.md）
#
# 默认 OFF：不改变原构建。开启时把 thirdparty/XCPlite 作为子项目仅构建库
# 目标（不建示例/测试/工具/安装），并在此后由 tests 子目录挂接测试 Slave
# 可执行文件与 XCPlite 集成测试。不修改 XCPlite 任何源文件，全部配置通过
# 其公开的 CMake 变量（CMP0077 正常变量）控制。
#
# LIBXCP_BUILD_EXAMPLES=ON 时本门自动跟随打开（见下方判定块）。
# ----------------------------------------------------------------------------
option(LIBXCP_BUILD_XCPLITE_SLAVE
       "Build the XCPlite-based test slave and protocol-debug integration tests"
       OFF)

if(LIBXCP_BUILD_EXAMPLES AND NOT LIBXCP_BUILD_XCPLITE_SLAVE)
  # 示例依赖 xcplite 库与 cpp_demo 对手端——EXAMPLES 开则本门自动跟随打开。
  # CACHE INTERNAL 立即生效（当前作用域可见，XCPlite 子项目段紧随其后），
  # 且不会在 GUI 中与 option 条目重复；语义仍是 BOOL 缓存项。
  message(STATUS "libxcp: LIBXCP_BUILD_EXAMPLES=ON，自动开启 LIBXCP_BUILD_XCPLITE_SLAVE")
  set(LIBXCP_BUILD_XCPLITE_SLAVE ON CACHE INTERNAL
      "Build the XCPlite-based test slave (auto-enabled by LIBXCP_BUILD_EXAMPLES)")
endif()

if(LIBXCP_BUILD_XCPLITE_SLAVE)
  # default 配置含 A2L 运行时生成 + UDP/TCP 传输，满足协议调试需要
  set(XCPLITE_CONFIGURATION "default")
  set(XCPLITE_BUILD_EXAMPLES OFF)
  set(XCPLITE_BUILD_TESTS OFF)
  set(XCPLITE_BUILD_TOOLS OFF)
  set(XCPLITE_BUILD_RUST_TOOLS OFF)
  set(XCPLITE_BUILD_BPF_DEMO OFF)
  set(XCPLITE_INSTALL OFF)
  add_subdirectory(thirdparty/XCPlite)
endif()

# ----------------------------------------------------------------------------
# 测试
# ----------------------------------------------------------------------------
if(LIBXCP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ----------------------------------------------------------------------------
# 示例程序目录（批次22：examples/xcp_master_udp）
#
# 默认 OFF——不改变既有构建。LIBXCP_BUILD_EXAMPLES=ON 时前置依赖
# LIBXCP_BUILD_XCPLITE_SLAVE 自动置 ON（见上方子项目段的跟随判定），无需
# 手工同开两门；xcplite 库与上游示例源均来自该子项目段。
# A2L 桥接层（libxcp::a2lbridge）现为必编组件，不再作为本门的检查项。
# 计划：code-plan/XCPlite_cpp_demo_示例对手端计划.md（v2.1）。
# ----------------------------------------------------------------------------
if(LIBXCP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
