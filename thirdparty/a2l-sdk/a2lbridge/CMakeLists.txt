# =============================================================================
# a2lbridge —— A2L 桥接层静态库（设计文档 §5 / R4；批次9 起并入 SDK 工程）
#
# 边界纪律（P9 断言，CI 强制）：
#   - 本目录源码禁止 #include <a2l/...>（只允许 liba2l/liba2l_api.hpp）；
#   - libxcp 主库禁止依赖本目标（对等库，方向只能是本库 → libxcp）。
# C++20 标准（主树一致），仅通过 liba2l::liba2l 目标接触 C++23 DLL。
# 唯一挂载点 = thirdparty/a2l-sdk/CMakeLists.txt 的 add_subdirectory(a2lbridge)。
# =============================================================================

add_library(libxcp_a2lbridge STATIC
    src/a2l_result.cpp
    src/a2l_types.cpp
    src/compu_method_eval.cpp
    src/a2l_dto_map.cpp
    src/a2l_database_impl.cpp
    src/if_data_xcp_impl.cpp
    src/daq_layout_impl.cpp
    src/record_layout_impl.cpp
    src/a2l_bridge.cpp
)

add_library(libxcp::a2lbridge ALIAS libxcp_a2lbridge)

target_compile_features(libxcp_a2lbridge PUBLIC cxx_std_20)
set_target_properties(libxcp_a2lbridge PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(libxcp_a2lbridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# liba2l::liba2l 由父工程（thirdparty/a2l-sdk）提供：工程内为真实目标的 ALIAS。
# PUBLIC：本库为静态库，SDK 头文件类型随公开接口传播给消费者；主树侧经
# IMPORTED libxcp::a2lbridge 的 INTERFACE_LINK_LIBRARIES 继续传递。
target_link_libraries(libxcp_a2lbridge PUBLIC liba2l::liba2l)

# P9 隔离断言（设计文档 §5.3）：桥接层禁止直接包含上游 a2llib 头文件。
# CONFIGURE_DEPENDS 只在目录本身变化时重扫描，不覆盖文件内容编辑，
# 因此每次构建都用 glob 命中的当前文件列表重新读取内容做检查。
file(GLOB_RECURSE _a2lbridge_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*" "${CMAKE_CURRENT_SOURCE_DIR}/include/*")
foreach(_src IN LISTS _a2lbridge_sources)
    file(READ "${_src}" _content)
    if(_content MATCHES "#include[ \t]*[<\"]a2l/")
        message(FATAL_ERROR "P9 隔离断言失败：${_src} 直接包含了上游 a2llib 头文件")
    endif()
endforeach()

if(MSVC)
    target_compile_options(libxcp_a2lbridge PRIVATE /utf-8 /W4)
else()
    target_compile_options(libxcp_a2lbridge PRIVATE -Wall -Wextra)
endif()
