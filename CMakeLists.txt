# ============================================================================
# libxcp —— XCP 协议主站核心（V1.3–V1.5）+ XCP on UDP/IP Transport
#
# 版本 2.0.0（破坏性变更，批次 B1）：
#   * XcpMaster 构造函数由位置参数改为单一 XcpMasterOptions 聚合
#   * 新增版本发现能力（GET_VERSION）与 PL/TL 双版本快照接口
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

# 最低版本 3.20：本工程未使用 3.21–3.24 期间的任何新增命令，而降到这里
# 才能直接使用 Ubuntu 22.04 自带的 CMake 3.22；3.20 与 thirdparty/a2llib
# 自身声明的下限保持同一口径。已使用的 3.20 之后引入的 policy（CMP0128、
# CMP0135）在下方显式置 NEW，避免降版本后行为回退。
cmake_minimum_required(VERSION 3.20)

# 降 minimum 后，3.20 之后引入的 policy 会处于"未设置"状态（按 OLD 处理）。
# 这两条会实际改变行为：CMP0128 影响语言标准 flag 是否附加、CMP0135 影响
# FetchContent 解压时间戳。显式置 NEW 保持与原先 3.24 语义一致。
if(POLICY CMP0128)
  cmake_policy(SET CMP0128 NEW)
endif()
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# ExternalProject 直到 CMake 3.24 才认识 DOWNLOAD_EXTRACT_TIMESTAMP 这个关键字；
# 更早的版本（如 Ubuntu 22.04 自带的 3.22）会把它当成 URL_HASH 的后续片段，
# 直接以 "URL_HASH is set to ...;DOWNLOAD_EXTRACT_TIMESTAMP;TRUE" 报错中止。
# 因此统一用该变量按版本传参，而不是在各处重复写死。
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
  set(LIBXCP_DOWNLOAD_EXTRACT_TIMESTAMP_ARGS
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
else()
  set(LIBXCP_DOWNLOAD_EXTRACT_TIMESTAMP_ARGS "")
endif()

project(libxcp
        VERSION 2.0.0
        DESCRIPTION "XCP protocol master core (V1.3-V1.5) with UDP/IP transport"
        LANGUAGES CXX)

# ----------------------------------------------------------------------------
# 全局构建选项
# ----------------------------------------------------------------------------
option(LIBXCP_BUILD_TESTS "构建 libxcp 单元测试与集成测试" ON)
option(LIBXCP_ENABLE_LOG "启用 libxcp 日志（默认 glog 后端）" OFF)

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

# ---------------------------------------------------------------------------
# std::format 能力探测与 fmt 回退
#
# 基准测试 tests/xcp_master_render_bench_test.cpp 无条件使用 std::format，
# 日志宏（log.hpp）在 LIBXCP_ENABLE_LOG=ON 时同样需要；而 GCC 11 的 libstdc++
# 尚未提供 <format>。因此在配置期统一探测一次：可用则全程使用 std::format，
# 不可用则回退到 fmt（优先复用系统 CONFIG 包，其次按固定 URL 下载）。
#
# 降级不影响调用点语法：两者共用 C++20 格式串，差异由
# include/libxcp/detail/format_compat.hpp 在编译期消化。
# ---------------------------------------------------------------------------
include(CheckCXXSourceCompiles)
# 探测同时校验 __cpp_lib_format 与真实调用（含本项目在用的 {:06d} / {:.1f}），
# 避免"有头文件但功能不完整"的标准库被误判为可用。
check_cxx_source_compiles([[
  #include <version>
  #if !defined(__cpp_lib_format) || (__cpp_lib_format < 201907L)
  #  error "std::format is unavailable"
  #endif
  #include <format>
  int main() {
    const auto formatted = std::format("value={} hex={:06d} f={:.1f}", 42, 42, 1.5);
    return formatted.empty();
  }
]] LIBXCP_HAS_CXX20_STD_FORMAT)

# 回退 fmt 时默认总是使用本工程固定版本的源码（FetchContent）。
# 之所以不默认 find_package：跨平台工程里 CMake 的默认搜索路径可能把其它平台的
# 安装（例如 WSL 下 /mnt/c 的 conda）解析进来，造成头文件与二进制不匹配。
option(LIBXCP_USE_SYSTEM_FMT
       "回退 fmt 时优先使用系统已安装的 fmt（默认关闭）" OFF)

if(LIBXCP_HAS_CXX20_STD_FORMAT)
  message(STATUS "libxcp: 使用标准库 std::format")
else()
  message(STATUS "libxcp: 当前工具链缺少 std::format，回退到 fmt")
  if(LIBXCP_USE_SYSTEM_FMT AND NOT TARGET fmt::fmt-header-only
     AND NOT TARGET fmt::fmt)
    find_package(fmt CONFIG QUIET)
  endif()
  if(NOT TARGET fmt::fmt-header-only AND NOT TARGET fmt::fmt)
    include(FetchContent)
    # 在函数作用域内固定 fmt 的构建选项，避免改写父工程缓存。
    function(libxcp_fetch_fmt)
      set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
      set(FMT_TEST OFF CACHE BOOL "" FORCE)
      set(FMT_DOC OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(
        fmt
        URL https://github.com/fmtlib/fmt/archive/refs/tags/11.2.0.tar.gz
        URL_HASH SHA256=bc23066d87ab3168f27cef3e97d545fa63314f5c79df5ea444d41d56f962c6af
        ${LIBXCP_DOWNLOAD_EXTRACT_TIMESTAMP_ARGS})
      FetchContent_MakeAvailable(fmt)
    endfunction()
    libxcp_fetch_fmt()
  endif()
  if(NOT TARGET fmt::fmt-header-only AND NOT TARGET fmt::fmt)
    message(FATAL_ERROR
      "当前工具链缺少 std::format，且未能提供 fmt：请安装 fmt 11.x CONFIG 包、"
      "设置 FETCHCONTENT_SOURCE_DIR_FMT，或改用支持 std::format 的编译器。")
  endif()
endif()

# 格式化后端入口目标：只负责把宏与包含路径传播给需要的目标；
# 具体用哪个后端由公共头自探测，避免依赖传播遗漏导致静默换实现。
add_library(libxcp_format INTERFACE)
add_library(libxcp::format ALIAS libxcp_format)
target_include_directories(libxcp_format INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
if(NOT LIBXCP_HAS_CXX20_STD_FORMAT)
  target_compile_definitions(libxcp_format INTERFACE LIBXCP_HAVE_FMT=1)
  # header-only 形态：不产生链接产物，公共头因此不引入额外的二进制 ABI 依赖。
  if(TARGET fmt::fmt-header-only)
    target_link_libraries(libxcp_format INTERFACE fmt::fmt-header-only)
  else()
    target_link_libraries(libxcp_format INTERFACE fmt::fmt)
  endif()
endif()

# 日志后端 glog 仅在开启日志时解析；关闭时不触碰任何日志依赖。
if(LIBXCP_ENABLE_LOG)
  if(NOT TARGET glog::glog)
    # 程序日志生命周期依赖 IsGoogleLoggingInitialized，旧版 glog 不满足此接口。
    find_package(glog 0.7.1 CONFIG QUIET)
  endif()
  if(NOT TARGET glog::glog)
    include(FetchContent)
    # 在函数作用域为按需构建的 glog 关闭可选测试/依赖，避免改写父工程缓存。
    function(libxcp_fetch_glog)
      set(BUILD_SHARED_LIBS OFF)
      set(BUILD_TESTING OFF)
      set(WITH_GFLAGS OFF)
      set(WITH_GTEST OFF)
      set(WITH_UNWIND OFF)
      set(CMAKE_POSITION_INDEPENDENT_CODE ON)
      FetchContent_Declare(
        glog
        URL https://github.com/google/glog/archive/v0.7.1.zip
        URL_HASH SHA256=c17d85c03ad9630006ef32c7be7c65656aba2e7e2fbfc82226b7e680c771fc88
        ${LIBXCP_DOWNLOAD_EXTRACT_TIMESTAMP_ARGS})
      FetchContent_MakeAvailable(glog)
    endfunction()
    libxcp_fetch_glog()
  endif()
  if(NOT TARGET glog::glog)
    message(FATAL_ERROR
      "LIBXCP_ENABLE_LOG=ON 但未能提供 glog::glog；请安装 glog 0.7.1+ CONFIG 包、"
      "设置 FETCHCONTENT_SOURCE_DIR_GLOG，或关闭 LIBXCP_ENABLE_LOG。")
  endif()

  # 复用父工程预建 target 时也校验版本，不能绕过 IsGoogleLoggingInitialized 要求。
  if(DEFINED glog_VERSION AND NOT glog_VERSION STREQUAL "")
    set(LIBXCP_GLOG_VERSION "${glog_VERSION}")
  else()
    get_target_property(LIBXCP_GLOG_VERSION glog::glog VERSION)
  endif()
  if(NOT LIBXCP_GLOG_VERSION OR LIBXCP_GLOG_VERSION STREQUAL "LIBXCP_GLOG_VERSION-NOTFOUND")
    message(FATAL_ERROR
      "LIBXCP_ENABLE_LOG=ON 但无法确认 glog 版本；请使用 glog 0.7.1+ CONFIG 包"
      "或固定版本 FetchContent 源码。")
  elseif(LIBXCP_GLOG_VERSION VERSION_LESS 0.7.1)
    message(FATAL_ERROR
      "LIBXCP_ENABLE_LOG=ON 需要 glog 0.7.1+，当前检测到 ${LIBXCP_GLOG_VERSION}。")
  endif()
  unset(LIBXCP_GLOG_VERSION)

endif()

if(MSVC)
  # 将源文件字符集声明为 UTF-8，避免中文注释在非英文 Windows 控制台下解析异常
  add_compile_options(/utf-8 /W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

# ----------------------------------------------------------------------------
# 运行时检查（Sanitizer）
#
# 仅用于 CI 与本地排查并发、内存缺陷；默认留空时完全不注入任何标志，正式产物
# 与未启用时保持一致。可用取值：
#   -DLIBXCP_SANITIZER=thread     数据竞争检测（ThreadSanitizer）
#   -DLIBXCP_SANITIZER=address    越界与释放后使用（AddressSanitizer）
#   -DLIBXCP_SANITIZER=undefined  未定义行为（UndefinedBehaviorSanitizer）
# 取值不在此做白名单校验，交由编译器诊断；MSVC 不具备对应命令行开关，命中时
# 直接报错终止，避免在 Windows 上静默生成"看似带检查、实际没有"的构建。
# ----------------------------------------------------------------------------
set(LIBXCP_SANITIZER "" CACHE STRING
    "运行时检查类型：留空/thread/address/undefined")

if(LIBXCP_SANITIZER)
  if(MSVC)
    message(FATAL_ERROR
      "LIBXCP_SANITIZER=${LIBXCP_SANITIZER} 需要 GCC 或 Clang 工具链；"
      "MSVC 不支持 ThreadSanitizer 等同类命令行开关。请在 Linux/macOS 上启用，"
      "或留空该选项。")
  endif()
  set(LIBXCP_SANITIZER_FLAGS
      "-fsanitize=${LIBXCP_SANITIZER}"
      "-fno-omit-frame-pointer"
      "-g")
  # 编译与链接都必须注入：sanitizer 的运行时库由链接阶段决定，只加编译标志会
  # 在链接时报未定义符号。
  add_compile_options(${LIBXCP_SANITIZER_FLAGS})
  add_link_options(${LIBXCP_SANITIZER_FLAGS})
  message(STATUS "已启用运行时检查：-fsanitize=${LIBXCP_SANITIZER}")
endif()

include(GNUInstallDirs)

# ----------------------------------------------------------------------------
# libxcp 静态库
# ----------------------------------------------------------------------------
add_library(libxcp STATIC)
add_library(libxcp::libxcp ALIAS libxcp)

target_sources(libxcp PRIVATE
        src/protocol_types.cpp
        src/protocol_version.cpp
        src/version_resolver.cpp
        src/xcp_error.cpp
        src/command_codec.cpp
        src/response_parser.cpp
        src/session.cpp
        src/udp_header_codec.cpp
        src/ethernet_header_codec.cpp
        src/tcp_stream_decoder.cpp
        src/event_poller.cpp
        src/socket_util.cpp
        src/tcp_transport.cpp
        src/tcp_framing_negotiation.cpp
        src/tcp_framing_candidates.cpp
        src/tcp_framing_verification.cpp
        src/udp_transport.cpp
        src/link_budget.cpp
        src/command_executor.cpp
        src/memory_access.cpp
        # B1：BUILD_CHECKSUM 的本地复算（XCP V1.5 Table 75 / Table 77）
        src/checksum.cpp
        src/xcp_master.cpp
        src/daq/dto_envelope_decoder.cpp
        src/daq/dto_payload_decoder.cpp
        src/daq/daq_timestamp.cpp
        src/measurement/measurement_planner.cpp
        src/measurement/measurement_session.cpp
        # 第二阶段提取：A2L 对齐证据（受限扫描 + 双源核对）。
        src/a2l_tcp_alignment.cpp
        # 第二阶段提取：CLI/A2L 传输层选择解析（§8.1 规则）。
        src/transport_selection.cpp
        # 第二阶段提取：受控 A2L 快照（Windows ACL / POSIX 权限校验）。
        src/trusted_a2l_snapshot.cpp)

# 平台原生 Socket/轮询实现严格二选一；公共注入/转发 TU 已包含在上方列表。
if(WIN32)
  target_sources(libxcp PRIVATE
    src/detail/socket/socket_ops_win.cpp
    src/detail/socket/event_poller_win.cpp
    # 受控 A2L 快照的 Windows 原生实现（ACL 句柄校验 + 独占创建）。
    src/detail/fs/trusted_snapshot_win.cpp)
else()
  target_sources(libxcp PRIVATE
    src/detail/socket/socket_ops_posix.cpp
    src/detail/socket/event_poller_posix.cpp
    # 受控 A2L 快照的 POSIX 原生实现（stat/fstat 权限校验 + 独占创建）。
    src/detail/fs/trusted_snapshot_posix.cpp)
endif()

target_include_directories(libxcp
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(libxcp PUBLIC
        LIBXCP_ENABLE_LOG=$<BOOL:${LIBXCP_ENABLE_LOG}>)
# std::format / fmt 后端入口：公共头 log.hpp 依赖它，必须对消费者可见。
target_link_libraries(libxcp PUBLIC libxcp::format)

# 协议核心仅依赖线程库（Session/Executor 内部同步）；
# UdpTransport 使用平台 Socket，Windows 需显式链接 ws2_32。
find_package(Threads REQUIRED)
target_link_libraries(libxcp PUBLIC Threads::Threads)
if(LIBXCP_ENABLE_LOG)
  # log.hpp 是安装的公共头文件；静态库消费者也必须获得 glog 链接依赖。
  target_link_libraries(libxcp PUBLIC glog::glog)
endif()

if(WIN32)
  # advapi32：受控 A2L 快照用 GetNamedSecurityInfoW/SetNamedSecurityInfoW
  # 等 ACL API 校验快照目录（`src/detail/fs/trusted_snapshot_win.cpp`）。
  target_link_libraries(libxcp PUBLIC ws2_32 advapi32)
  # Windows 头（经 winsock 头链传递带入的 minwindef.h）默认定义 min/max 宏，
  # 会把 `std::min(a, b)` 预处理成语法错误（MSVC C2589/C2059）。
  # 全仓无任何代码依赖这两个宏，故对本目标统一关闭；测试/工具目标在各自的
  # CMakeLists 里做同样设置，第三方库保持原状以免影响其构建。
  target_compile_definitions(libxcp PRIVATE NOMINMAX)
endif()

set_target_properties(libxcp PROPERTIES
        OUTPUT_NAME xcp
        POSITION_INDEPENDENT_CODE ON)

if(LIBXCP_ENABLE_LOG)
  # 自有程序入口的内部初始化辅助代码，不作为 libxcp 公共接口或安装目标。
  add_library(libxcp_program_logging STATIC src/program_logging.cpp)
  target_compile_features(libxcp_program_logging PUBLIC cxx_std_20)
  target_compile_definitions(libxcp_program_logging PUBLIC LIBXCP_ENABLE_LOG=1)
  target_include_directories(libxcp_program_logging PUBLIC
          ${PROJECT_SOURCE_DIR}/include
          ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(libxcp_program_logging PUBLIC glog::glog)
  set_target_properties(libxcp_program_logging PROPERTIES
          POSITION_INDEPENDENT_CODE ON)
endif()

# 所有自有工具/示例/测试入口均显式获得 0/1 开关；ON 时共享内部文件日志初始化器。
function(libxcp_configure_program_logging target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "libxcp_configure_program_logging: 未知 target '${target}'")
  endif()
  if(LIBXCP_ENABLE_LOG)
    target_link_libraries(${target} PRIVATE libxcp_program_logging)
    # 日志宏内部走 format_compat：除 glog 外还必须获得格式化后端
    # （有 std::format 时为空实现，否则是 fmt）。不链接 libxcp 核心的目标
    # （如纯 XCPlite 的 xcp_slave_demo）尤其不能漏。
    target_link_libraries(${target} PRIVATE libxcp::format)
  else()
    target_compile_definitions(${target} PRIVATE LIBXCP_ENABLE_LOG=0)
  endif()
  # log.hpp 为公共头。不链接 libxcp 核心或 libxcp_program_logging 的目标
  # （如纯 XCPlite 的 xcp_slave_demo）必须显式获得 include 搜索路径，
  # 否则关闭日志时 #include "libxcp/log.hpp" 会编译失败。
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
endfunction()

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

# --- 测量适配器 --------------------------------------------------------------
# 已并入 A2L 模块：目标 libxcp_measurement_adapter 由 thirdparty/a2l-sdk/adapter
# 定义，并依赖核心 libxcp，故由 SDK 父工程以 if(TARGET libxcp) 守卫。

# ----------------------------------------------------------------------------
# 构建开关（先于子目录声明；判定与 add_subdirectory 分离，见下方各段）
#
# EXAMPLES 门依赖 XCPlite 子项目——其 option 必须出现在 XCPlite 段的跟随
# 判定之前，故在此集中声明（L30 的 LIBXCP_BUILD_TESTS 保持原位）。
# ----------------------------------------------------------------------------
option(LIBXCP_BUILD_EXAMPLES
       "Build the standalone example programs under examples/"
       OFF)
option(LIBXCP_BUILD_TOOLS
       "Build standalone libxcp tools under tools/"
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
# 独立工具（LIBXCP_BUILD_TOOLS 不联动 EXAMPLES / XCPlite）
# 工具目标先定义，测试可在两门同时开启时对其做 CLI 冒烟验证。
# ----------------------------------------------------------------------------
if(LIBXCP_BUILD_TOOLS)
  add_subdirectory(tools)
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
