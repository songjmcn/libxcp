# =============================================================================
# adapter —— A2L 适配层静态库（libxcp_measurement_adapter）
#
# 核心 <-> 桥接层之间的唯一接触点：把 a2lbridge 的 A2L 数据库适配成核心的
# IMeasurementDatabase 窄接口，并把测量会话的运行时画像落地为可复用的 Profile。
#
# 依赖方向（单向，不可反转）：本库 -> libxcp 核心 + libxcp::a2lbridge。
# 因此本目标只在主树构建时存在：父工程 thirdparty/a2l-sdk/CMakeLists.txt 以
# if(TARGET libxcp) 守卫本目录；独立 SDK 构建（build-sdk.ps1）没有核心目标，
# 整目录跳过，与批次9 之前的产出范围一致。
#
# 标准：SDK 工程整体为 C++23（上游 a2llib 要求），本目标与核心保持一致用
# C++20，写法与 a2lbridge/CMakeLists.txt 相同（目标级覆盖父工程默认值）。
# =============================================================================

add_library(libxcp_measurement_adapter STATIC
    src/a2l_measurement_database.cpp
    src/measurement_runtime_profile.cpp
)

# 与核心一致的 C++20；PUBLIC 使消费者也至少拿到 cxx_std_20。
target_compile_features(libxcp_measurement_adapter PUBLIC cxx_std_20)
set_target_properties(libxcp_measurement_adapter PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# include/ 为适配层的公开头根，消费方沿用 "a2l/xxx.hpp" 的既有写法，
# 因此迁移不改变任何消费方的 #include 行。
target_include_directories(libxcp_measurement_adapter
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# 核心与桥接层均为静态库：PUBLIC 以便把二者的公开头路径继续传播给消费方。
target_link_libraries(libxcp_measurement_adapter PUBLIC
    libxcp::libxcp libxcp::a2lbridge
)

if(MSVC)
    target_compile_options(libxcp_measurement_adapter PRIVATE /utf-8 /W4)
else()
    target_compile_options(libxcp_measurement_adapter PRIVATE -Wall -Wextra)
endif()
