# ============================================================================
# a2l_isolation_check.cmake —— A2L 栈隔离自动化（设计 §7.2 T10 / 批次13 T13-18）
#
# 这是 configure 期 P9 扫描的 **ctest 化**版本：门禁必须在"跑测试"时也可复现，
# 而不是只在 cmake 配置时生效（配置缓存复用会跳过 P9）。
#
# 两条大小写敏感扫描（CMake 的 MATCHES 默认大小写敏感）：
#   S1 桥接层目录内 `#include <a2l/`（或 "a2l/）命中数必须 == 0
#      —— 上游 a2llib 头只允许出现在 SDK 的导出层（liba2l_export.cpp）。
#   S2 主树 include/ 与 src/ 内 `liba2l` / `a2lbridge` / `calmcar::xcp::a2l`
#      命中数必须 == 0 —— 主库 libxcp 不得知道 A2L 栈的存在（对等库方向纪律）。
#
# 用法（由 tests/CMakeLists.txt 的 add_test 调用）：
#   cmake -DA2L_BRIDGE_DIR=<dir> -DLIBXCP_ROOT_DIR=<dir> -P a2l_isolation_check.cmake
# 命中即非零退出（FATAL_ERROR → cmake -P 返回非 0）。
# ============================================================================

foreach(_var A2L_BRIDGE_DIR LIBXCP_ROOT_DIR)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "缺少变量 ${_var}（调用方必须传入）")
    endif()
endforeach()

# --- S1：桥接层禁止直接包含上游 a2llib 头 --------------------------------------
file(GLOB_RECURSE _bridge_files
     LIST_DIRECTORIES false
     "${A2L_BRIDGE_DIR}/src/*" "${A2L_BRIDGE_DIR}/include/*")
set(_s1_hits 0)
foreach(_f IN LISTS _bridge_files)
    file(READ "${_f}" _content)
    # 与 configure 期 P9 同一正则（尖括号与引号两种形式都拦）
    if(_content MATCHES "#include[ \t]*[<\"]a2l/")
        message(STATUS "S1 命中：${_f}")
        math(EXPR _s1_hits "${_s1_hits} + 1")
    endif()
endforeach()

# --- S2：主库不得引用 A2L 栈 ----------------------------------------------------
file(GLOB_RECURSE _main_files
     LIST_DIRECTORIES false
     "${LIBXCP_ROOT_DIR}/include/*" "${LIBXCP_ROOT_DIR}/src/*")
set(_s2_hits 0)
set(_s2_files 0)
foreach(_f IN LISTS _main_files)
    file(READ "${_f}" _content)
    math(EXPR _s2_files "${_s2_files} + 1")
    if(_content MATCHES "liba2l" OR _content MATCHES "a2lbridge" OR
       _content MATCHES "calmcar::xcp::a2l")
        message(STATUS "S2 命中：${_f}")
        math(EXPR _s2_hits "${_s2_hits} + 1")
    endif()
endforeach()

if(_bridge_files STREQUAL "")
    message(FATAL_ERROR "S1 扫描目录为空（A2L_BRIDGE_DIR=${A2L_BRIDGE_DIR} 是否正确？）")
endif()
if(_s2_files EQUAL 0)
    message(FATAL_ERROR "S2 扫描文件数为 0（LIBXCP_ROOT_DIR=${LIBXCP_ROOT_DIR} 是否正确？）")
endif()

if(NOT _s1_hits EQUAL 0)
    message(FATAL_ERROR
            "T10 隔离自动化失败：桥接层内出现 ${_s1_hits} 处上游 a2llib 头包含"
            "（A2L 类型只允许经 liba2l DTO 跨界）")
endif()
if(NOT _s2_hits EQUAL 0)
    message(FATAL_ERROR
            "T10 隔离自动化失败：主树 include/ 或 src/ 内出现 ${_s2_hits} 处"
            " A2L 栈引用（libxcp 必须与 A2L 栈无耦合）")
endif()
list(LENGTH _bridge_files _bridge_count)
message(STATUS "T10 隔离自动化通过：S1 扫描 ${_bridge_count} 个桥接层文件、"
               "S2 扫描 ${_s2_files} 个主树文件，命中均为 0")
