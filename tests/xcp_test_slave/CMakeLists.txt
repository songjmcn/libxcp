# ============================================================================
# xcp_test_slave —— 基于 XCPlite 的 XCP Slave 测试对手端（独立进程）
#
# 仅在 LIBXCP_BUILD_XCPLITE_SLAVE=ON 时构建（见上级 tests/CMakeLists.txt）。
# 不修改 thirdparty/XCPlite 的任何文件，仅链接其 xcplite::xcplite 目标。
# ============================================================================

add_executable(xcp_test_slave main.cpp)

# C++20（Slave 端使用 designated initializer 等 C++20 语法）
target_compile_features(xcp_test_slave PRIVATE cxx_std_20)

# 链接 XCPlite 库；其 PUBLIC 头目录（inc/ 与 src/）与
# XCPLITE_CONFIGURATION 宏定义随之传播
target_link_libraries(xcp_test_slave PRIVATE xcplite::xcplite)

# Windows：xcplite 的 socket 层依赖 ws2_32（与主工程同一处理）
if(WIN32)
  target_link_libraries(xcp_test_slave PRIVATE ws2_32)
  # 与 xcplite 库自身一致：测试代码使用标准 C 运行时函数（fopen 等），
  # 不切换到 MSVC 的 *_s 安全扩展
  target_compile_definitions(xcp_test_slave PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(MSVC)
  target_compile_options(xcp_test_slave PRIVATE /utf-8)
endif()
