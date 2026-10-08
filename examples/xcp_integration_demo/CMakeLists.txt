add_executable(xcp_integration_demo
        xcp_integration_demo.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../xcp_master_udp/demo_process.cpp")
target_compile_features(xcp_integration_demo PRIVATE cxx_std_20)
target_link_libraries(xcp_integration_demo PRIVATE
        libxcp::libxcp
        libxcp_measurement_adapter)
target_include_directories(xcp_integration_demo PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../xcp_master_udp")
target_compile_definitions(xcp_integration_demo PRIVATE
        CPP_DEMO_EXECUTABLE="$<TARGET_FILE:cpp_demo>"
        XCP_104_AML_FILE="${PROJECT_SOURCE_DIR}/thirdparty/XCPlite/XCP_104.aml")
add_dependencies(xcp_integration_demo cpp_demo)
add_custom_command(TARGET xcp_integration_demo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:xcp_integration_demo>")
if(MSVC)
  target_compile_options(xcp_integration_demo PRIVATE /utf-8)
endif()

add_test(NAME ExampleXcpIntegrationDemoHelp
        COMMAND xcp_integration_demo --help)
set_tests_properties(ExampleXcpIntegrationDemoHelp PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 10)

add_test(NAME ExampleXcpIntegrationDemoCppDemo
        COMMAND xcp_integration_demo
                --profile cpp_demo
                --run-dir "${CMAKE_CURRENT_BINARY_DIR}/cpp_demo_run"
                --seconds 4
                --write-address 0x80010000
                --write-value 1400
                --expected-original e803
                --confirm-write)
set_tests_properties(ExampleXcpIntegrationDemoCppDemo PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 180)

# Fail closed before any transport connection or write when the explicit opt-in
# flag is missing. WILL_FAIL turns the expected usage rejection into a passing test.
add_test(NAME ExampleXcpIntegrationDemoWriteRequiresConsent
        COMMAND xcp_integration_demo
                --a2l unused.a2l --write-address 0x1000 --write-value 0102)
set_tests_properties(ExampleXcpIntegrationDemoWriteRequiresConsent PROPERTIES
        WILL_FAIL TRUE
        RUN_SERIAL TRUE
        TIMEOUT 10)

add_test(NAME ExampleXcpIntegrationDemoWriteRequiresExpectedOriginal
        COMMAND xcp_integration_demo
                --a2l unused.a2l --write-address 0x1000 --write-value 0102
                --confirm-write)
set_tests_properties(ExampleXcpIntegrationDemoWriteRequiresExpectedOriginal PROPERTIES
        WILL_FAIL TRUE
        RUN_SERIAL TRUE
        TIMEOUT 10)

add_test(NAME ExampleXcpIntegrationDemoModifyBitsRequiresExpectedOriginal
        COMMAND xcp_integration_demo
                --a2l unused.a2l --modify-bits 0x1000:0:65535:1
                --confirm-modify-bits)
set_tests_properties(ExampleXcpIntegrationDemoModifyBitsRequiresExpectedOriginal PROPERTIES
        WILL_FAIL TRUE
        RUN_SERIAL TRUE
        TIMEOUT 10)

foreach(_unsafe_args IN ITEMS
        "--a2l unused.a2l --set-page 0:1"
        "--a2l unused.a2l --freeze-segment 0"
        "--a2l unused.a2l --copy-page 0:0:0:1"
        "--a2l unused.a2l --modify-bits 0x1000:0:65535:1")
  string(REPLACE " " ";" _args_list "${_unsafe_args}")
  list(GET _args_list 2 _operation)
  string(REPLACE "--" "" _test_suffix "${_operation}")
  add_test(NAME "ExampleXcpIntegrationDemo_${_test_suffix}_RequiresConsent"
          COMMAND xcp_integration_demo ${_args_list})
  set_tests_properties("ExampleXcpIntegrationDemo_${_test_suffix}_RequiresConsent" PROPERTIES
          WILL_FAIL TRUE
          RUN_SERIAL TRUE
          TIMEOUT 10)
endforeach()
