# ============================================================================
# uchardet 桩（shim）子项目 —— SDK 工程内部件（批次9 合并；§5.2 方案 A / A-8）
#
# 背景：上游 a2llib 无条件 include(script/uchardet.cmake)，其
# find_package(uchardet CONFIG REQUIRED)（第 6 行，if(NOT uchardet_FOUND) 守卫）
# 与 uchardet::libuchardet 的 PUBLIC 链接（a2l/CMakeLists.txt:369）是硬约束，
# 而本机没有 uchardet。SDK 根工程先 add_subdirectory 本目录、再预置
# uchardet_FOUND TRUE 跳过查找，链接名由本文件末尾的 INTERFACE IMPORTED
# 目标承接（ALIAS 形态会触发上游 INTERFACE_LOCATION 查询硬报错，见文末注释）。
# （旧"假 Config 包文件对 + build-shim.ps1 + CMAKE_PREFIX_PATH"机制批次9退役。）
#
#   - include/uchardet.h     与真 uchardet 同名的最小 C API 声明（仅 a2lhelper.cpp
#                            用到的 5 个符号：new/handle_data/data_end/get_charset/delete）
#   - src/uchardet_shim.cpp  恒返"未知编码"的实现；空 charset 使上游
#                            ConvertAllStrings() 提前返回、原始字节直通
#
# 语义边界（§5.2 P1 已实测）：uchardet 仅被 ASAP2 主解析器的编码归一化路径使用；
# IF_DATA XCP 子解析器完全不触碰。首里程碑要求 A2L 测试样本统一 UTF-8/ASCII。
#
# 本目录属本工程自建，不违反"不改 thirdparty/a2llib"约束。
# ============================================================================

cmake_minimum_required(VERSION 3.24)

project(uchardet_shim
        VERSION 0.0.8
        DESCRIPTION "Minimal no-op shim replacing uchardet for the libxcp A2L SDK build"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# 桩库：编译为静态库（输出名 uchardet.lib），符号在最终链接 liba2l.dll 时被吞掉
# ----------------------------------------------------------------------------
add_library(uchardet_shim STATIC
        src/uchardet_shim.cpp)

target_include_directories(uchardet_shim
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set_target_properties(uchardet_shim PROPERTIES OUTPUT_NAME uchardet)

if(MSVC)
  target_compile_options(uchardet_shim PRIVATE /utf-8 /W4)
endif()

# 上游链接名：target_link_libraries(a2l PUBLIC uchardet::libuchardet)。
# 形态约束（均实测踩过）：
#   - ALIAS 不行：上游 script/uchardet.cmake:18 查询 INTERFACE_LOCATION，
#     ALIAS→静态库 的该查询硬报错（LOCATION 不可从构建目标读取）；
#   - 直接 add_library(ns::name INTERFACE) 不行：CMake 4 禁止创建带命名空间的
#     非 ALIAS/非 IMPORTED 目标；
#   - INTERFACE IMPORTED 合法：属性查询返回空值不报错，与旧假包目标形态一致。
# 桩静态库经 INTERFACE_LINK_LIBRARIES 引用，随 a2l 的 PUBLIC 传播最终吞入 liba2l.dll。
add_library(uchardet::libuchardet INTERFACE IMPORTED GLOBAL)
set_target_properties(uchardet::libuchardet PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
        INTERFACE_LINK_LIBRARIES "uchardet_shim")
