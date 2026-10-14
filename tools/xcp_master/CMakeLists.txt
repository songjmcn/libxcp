include(FetchContent)

# 仅在构建 xcp_master 时获取固定版本的 FTXUI；不把依赖写入 thirdparty。
# 以普通变量关闭依赖自身的非必需目标，不覆盖用户全局 Cache 配置。
set(FTXUI_BUILD_EXAMPLES OFF)
set(FTXUI_BUILD_DOCS OFF)
set(FTXUI_BUILD_TESTS OFF)
set(FTXUI_ENABLE_INSTALL OFF)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG f921fad208912747c17d129a8ef75ec7624b6eec
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(ftxui)

add_executable(xcp_master main.cpp)
target_compile_features(xcp_master PRIVATE cxx_std_20)
target_link_libraries(xcp_master PRIVATE
  libxcp::libxcp libxcp::a2lbridge libxcp_measurement_adapter
  ftxui::ftxui)

if(MSVC)
  target_compile_options(xcp_master PRIVATE /utf-8)
endif()

# A2L bridge loads liba2l at runtime; keep the runtime library beside the tool.
add_custom_command(TARGET xcp_master POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:liba2l::liba2l>" "$<TARGET_FILE_DIR:xcp_master>")
