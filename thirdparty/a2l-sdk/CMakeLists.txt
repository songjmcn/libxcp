# =============================================================================
# liba2l_sdk —— A2L 栈唯一构建工程（设计文档 §5.3 / R4.3 + 批次9 工程合并）
#
# 一个工程一次构建，产出主树消费的全部产物：
#   liba2l.dll / liba2l.lib              —— a2llib 的 DLL 封装（上游 a2l 与全部
#                                           Boost 静态库整体吞入，A-12）
#   libxcp_a2lbridge.lib + include/libxcp —— 语义适配层 a2lbridge（C++20，B 类决策）
#   uchardet 桩                           —— 上游硬依赖的最小可链接面（子工程）
#
# 依赖解析策略（禁止写死绝对路径）：
#   Boost    : 两种消费形态——主树 add_subdirectory（默认形态，A2L 栈为必编
#              组件）时由根 CMakeLists.txt 先行 find_package(Boost CONFIG)
#              （自动发现 + FATAL_ERROR 指引）；独立构建（build-sdk.ps1，可选
#              SDK 开发入口）时由脚本传入 -DBoost_DIR=<...>/cmake/Boost-<ver>
#   uchardet : 本工程自有子工程（预置 uchardet_FOUND + ALIAS 目标使上游跳过
#              find_package；旧"假包文件对 + CMAKE_PREFIX_PATH"机制批次9退役）
#
# 上游 thirdparty/a2llib 为 git 子模块，本工程只 add_subdirectory 消费，
# 绝不修改其任何文件；仅通过缓存级 option 强制关闭 TOOLS/TEST/DOC/FLEX。
# =============================================================================

cmake_minimum_required(VERSION 3.24)

project(liba2l_sdk
        VERSION 0.1.0
        DESCRIPTION "Shared library wrapper around a2llib (MIT)"
        LANGUAGES CXX)

# --- 全局编译约束 -------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)          # 上游要求 C++23（§5.1）
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/utf-8 /W4)
endif ()

# --- uchardet 桩（本工程子项目；方案 A / A-8，§5.2）---------------------------
# 上游 script/uchardet.cmake:6 以 if(NOT uchardet_FOUND) 守卫
# find_package(uchardet CONFIG REQUIRED)：先 add_subdirectory 桩库，再预置
# uchardet_FOUND（普通变量，子作用域可见）即跳过查找；接口目标名
# uchardet::libuchardet 正是上游 a2l 目标 PUBLIC 链接（CMakeLists.txt:369）的名字。
add_subdirectory(uchardet-shim ${CMAKE_CURRENT_BINARY_DIR}/uchardet-shim-build)
set(uchardet_FOUND TRUE)

# --- 上游 a2llib（子模块）-----------------------------------------------------
# 强制缓存值：避免其 FetchContent(expat/googletest/doxygen/flex) 分支被激活
set(A2L_TOOLS OFF CACHE BOOL "" FORCE)
set(A2L_TEST OFF CACHE BOOL "" FORCE)
set(A2L_DOC OFF CACHE BOOL "" FORCE)
set(A2L_FLEX OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../a2llib
                 ${CMAKE_CURRENT_BINARY_DIR}/a2llib-build)

# --- Boost 库目录注入链接搜索路径 ---------------------------------------------
# a2l.lib 内嵌 /DEFAULTLIB:libboost_locale-vc143-mt-x64-1_86.lib 引用（裸文件名，
# 由上游把 Boost 头文件中的 autolink 指令一并编入静态库）。VS 生成器不会自动提供
# Boost 的库目录作为 AdditionalLibraryDirectories，导致 LNK1104。这里从已解析的
# Boost 库目录补进 liba2l 的链接搜索路径。
# Boost 来源（两种消费形态）：
#   - 主树 add_subdirectory：根 CMakeLists.txt 在挂载前 find_package(Boost CONFIG)
#     （自动发现 + FATAL_ERROR 指引），本工程直接复用其缓存结果；
#   - build-sdk.ps1 独立构建：脚本传入 -DBoost_DIR=<...>/cmake/Boost-<ver>。
# 两条路径最终都落到同一次 find_package（CONFIG 包幂等）。
find_package(Boost CONFIG REQUIRED COMPONENTS locale filesystem process)
if (TARGET Boost::locale)
    get_target_property(_liba2l_boost_loc Boost::locale IMPORTED_LOCATION_RELEASE)
    if (NOT _liba2l_boost_loc)
        get_target_property(_liba2l_boost_loc Boost::locale IMPORTED_IMPLIB_RELEASE)
    endif ()
endif ()
if (_liba2l_boost_loc)
    get_filename_component(_liba2l_boost_libdir "${_liba2l_boost_loc}" DIRECTORY)
else ()
    # 兜底：Boost_DIR = <prefix>/lib/cmake/Boost-<ver> → <prefix>/lib
    get_filename_component(_liba2l_boost_libdir "${Boost_DIR}/../../" ABSOLUTE)
endif ()
message(STATUS "liba2l: Boost 库搜索目录 = ${_liba2l_boost_libdir}")

# --- liba2l 共享库 ------------------------------------------------------------
add_library(liba2l SHARED
        src/liba2l_export.cpp       # 唯一触碰 <a2l/...> 的翻译单元
        src/dll_main.cpp)           # DllMain 占位（无自定义初始化）

target_include_directories(liba2l
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# 导出宏方向：本目标 dllexport；消费方不定义 → dllimport（liba2l_export.h）
target_compile_definitions(liba2l PRIVATE LIBA2L_MAKE_SHARED)

# 上游静态库整体链入（PRIVATE：其接口绝不出现在 liba2l 的使用要求里）
target_link_libraries(liba2l PRIVATE a2l)

set_target_properties(liba2l PROPERTIES
        OUTPUT_NAME liba2l
        POSITION_INDEPENDENT_CODE ON)

# 标准隔离：本工程目录作用域设 C++23，但禁止其作为 INTERFACE 使用要求传播给
# 主树消费者。被 libxcp 主工程 add_subdirectory 时，若不剥离 cxx_std_23，
# gtest / libxcp_tests 等 C++20 目标会被强制抬到 C++23（独立构建时因无主树
# 消费者而无此表现）。消费方仍按各自 target 的标准编译；跨 liba2l.dll 边界的
# ABI 不受影响（导出宏整类导出，见 include/liba2l/liba2l_export.h）。
target_compile_features(liba2l PRIVATE cxx_std_23)
get_target_property(_liba2l_sdk_cxx_feature liba2l INTERFACE_COMPILE_FEATURES)
if(_liba2l_sdk_cxx_feature MATCHES "cxx_std_")
    list(FILTER _liba2l_sdk_cxx_feature EXCLUDE REGEX "^cxx_std_")
    set_target_properties(liba2l PROPERTIES INTERFACE_COMPILE_FEATURES "${_liba2l_sdk_cxx_feature}")
endif()
unset(_liba2l_sdk_cxx_feature)

# 把 Boost 库目录加入链接搜索路径（仅作用于本目标的 link.exe 命令行 /LIBPATH）
target_link_directories(liba2l PRIVATE "${_liba2l_boost_libdir}")

# 命名空间 ALIAS：a2lbridge 与主树文档统一以 liba2l::liba2l 为目标名。
add_library(liba2l::liba2l ALIAS liba2l)

# --- A2L 桥接层（C++20 静态库；目录级说明见 a2lbridge/CMakeLists.txt 头）-------
# bridge 只依赖本工程的 liba2l::liba2l（不引用主树 libxcp 头，已实测 0 命中），
# 故可完全脱离主树构建；其 C++20 约束与 P9 隔离扫描在子工程内自带。
add_subdirectory(a2lbridge ${CMAKE_CURRENT_BINARY_DIR}/a2lbridge-build)
