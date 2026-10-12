add_executable(xcp_master main.cpp)
target_compile_features(xcp_master PRIVATE cxx_std_20)
target_link_libraries(xcp_master PRIVATE
  libxcp::libxcp libxcp::a2lbridge libxcp_measurement_adapter)

if(MSVC)
  target_compile_options(xcp_master PRIVATE /utf-8)
endif()

# A2L bridge loads liba2l at runtime; keep the runtime library beside the tool.
add_custom_command(TARGET xcp_master POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:liba2l::liba2l>" "$<TARGET_FILE_DIR:xcp_master>")
