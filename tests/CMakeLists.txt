# ============================================================================
# libxcp 测试构建
#
# 测试框架：GoogleTest（通过 FetchContent 获取，离线环境需预先配置
# CMAKE_FETCHCONTENT 缓存或 GIT_CONFIG 代理）。
#
# 本阶段（设计文档阶段 0~2）覆盖：
#   protocol_types_test.cpp     —— PID 分类、AG 换算、资源掩码、地址推进、名称映射
#   xcp_error_test.cpp          —— 异常字段保留与分类字符串
#   command_codec_test.cpp      —— 8 条命令黄金报文（Intel / Motorola）
#   response_parser_test.cpp    —— RES/ERR/EV/SERV/DTO 分类与字段解析
#   session_test.cpp            —— Session 状态机、参数校验、单 Pending Command
#
# 阶段 3~7 追加：memory_access_test.cpp / recovery_test.cpp /
#               xcp_master_integration_test.cpp / xcp_udp_loopback_test.cpp
# ============================================================================

include(FetchContent)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

if(EXISTS "${LIBXCP_GOOGLETEST_LOCAL}/CMakeLists.txt")
  # 使用仓库内预取的离线源，避免在线克隆受本机 git SSL 后端影响
  set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${LIBXCP_GOOGLETEST_LOCAL}" CACHE PATH "" FORCE)
endif()

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(googletest)

add_executable(libxcp_tests
        protocol_types_test.cpp
        xcp_error_test.cpp
        command_codec_test.cpp
        response_parser_test.cpp
        session_test.cpp
        udp_header_codec_test.cpp
        udp_transport_test.cpp
        udp_test_slave_test.cpp
        memory_access_test.cpp
        recovery_test.cpp
        seed_key_test.cpp
        xcp_master_integration_test.cpp
        xcp_daq_test.cpp
        xcp_udp_loopback_test.cpp
        udp_test_slave.cpp
        dto_envelope_decoder_test.cpp
        get_daq_event_info_test.cpp
        measurement_planner_test.cpp
        measurement_session_test.cpp
        daq_timestamp_test.cpp
        measurement_session_hardening_test.cpp)

target_link_libraries(libxcp_tests PRIVATE
        libxcp::libxcp
        GTest::gtest
        GTest::gtest_main)

# 测试设施头文件目录（mock_transport.hpp 等），仅 tests 内部可见
target_include_directories(libxcp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
  target_compile_options(libxcp_tests PRIVATE /utf-8)
endif()

include(GoogleTest)
gtest_discover_tests(libxcp_tests DISCOVERY_TIMEOUT 60)

# ----------------------------------------------------------------------------
# XCPlite 协议调试集成测试（code-plan/XCPlite_Slave_协议调试集成计划.md）
#
# 仅 LIBXCP_BUILD_XCPLITE_SLAVE=ON 时构建：
#   xcp_test_slave        —— 基于 XCPlite 的 Slave 独立进程（tests/xcp_test_slave/）
#   XcpliteIntegration    —— 真实 UDP + 真实 Slave 的集成测试（子进程由夹具管理）
# A2L 读取用例（xcplite_a2l_read_test.cpp）额外依赖桥接层，
# 仅当 LIBXCP_BUILD_A2L 同时开启时编入。
# ----------------------------------------------------------------------------
if(LIBXCP_BUILD_XCPLITE_SLAVE)
  add_subdirectory(xcp_test_slave)

  add_executable(XcpliteIntegration
          xcplite_slave_fixture.cpp
          xcplite_protocol_test.cpp
          xcplite_write_test.cpp
          xcplite_daq_test.cpp)
  target_compile_features(XcpliteIntegration PRIVATE cxx_std_20)
  target_link_libraries(XcpliteIntegration PRIVATE
          libxcp::libxcp
          GTest::gtest
          GTest::gtest_main)
  # 夹具与共享类型头的搜索路径（tests/ 与 tests/xcp_test_slave/）
  target_include_directories(XcpliteIntegration PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR})
  # Slave 可执行文件路径经 CMake 注入，禁止硬编码
  target_compile_definitions(XcpliteIntegration PRIVATE
          XCPLITE_SLAVE_EXECUTABLE="$<TARGET_FILE:xcp_test_slave>")
  add_dependencies(XcpliteIntegration xcp_test_slave)
  if(MSVC)
    target_compile_options(XcpliteIntegration PRIVATE /utf-8)
  endif()

  # A2L 读取用例：依赖桥接层（libxcp::a2lbridge + 运行期 liba2l.dll）
  if(LIBXCP_BUILD_A2L)
    target_sources(XcpliteIntegration PRIVATE xcplite_a2l_read_test.cpp)
    # 测量子系统 L3 全链路（v0.9）：桥接 + 适配器 + MeasurementSession
    target_sources(XcpliteIntegration PRIVATE xcplite_measurement_test.cpp)
    target_link_libraries(XcpliteIntegration PRIVATE
            libxcp::a2lbridge libxcp_measurement_adapter)
    target_compile_definitions(XcpliteIntegration PRIVATE
            LIBXCP_HAS_A2LBRIDGE=1)
    add_custom_command(TARGET XcpliteIntegration POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:liba2l::liba2l>"
                    "$<TARGET_FILE_DIR:XcpliteIntegration>")
  endif()

  gtest_discover_tests(XcpliteIntegration DISCOVERY_TIMEOUT 120)
endif()

# ----------------------------------------------------------------------------
# A2lSmoke —— A2L 最小开发验证集（code-plan/A2L_CMake最小开发验证集_R4.md §5）
#
# 仅 LIBXCP_BUILD_A2L=ON 时构建；不依赖 GoogleTest（独立 main + 退出码）。
# 门禁 7（OFF 模式原构建不变）由 OFF 配置重新生成构建目录验证，不在本目标。
# ----------------------------------------------------------------------------
if(LIBXCP_BUILD_A2L)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  set(A2L_GOLDEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/a2l_gen")
  set(A2L_GOLDEN_FILE "${A2L_GOLDEN_DIR}/golden_basic.a2l")

  # P10（设计 §7.1-A / §8.2）：CTest fixture 先跑生成器（含全部 spec 与
  # 畸形合法变体），再运行 A2lSmoke/A2lGolden；生成文件只落 build tree。
  add_test(NAME a2l_gen
           COMMAND ${Python3_EXECUTABLE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/a2l_gen/gen_a2l.py"
                   --out "${A2L_GOLDEN_DIR}" --variants)
  set_tests_properties(a2l_gen PROPERTIES FIXTURES_SETUP A2lGolden)

  add_executable(A2lSmoke a2l_smoke_test.cpp)
  target_compile_features(A2lSmoke PRIVATE cxx_std_20)
  # 经桥接层消费（libxcp::a2lbridge PUBLIC 传播 liba2l 导入目标的头与链接库）
  target_link_libraries(A2lSmoke PRIVATE libxcp::a2lbridge)
  target_compile_definitions(A2lSmoke PRIVATE A2L_GOLDEN_PATH="${A2L_GOLDEN_FILE}")
  if(MSVC)
    target_compile_options(A2lSmoke PRIVATE /utf-8)
  endif()

  # 运行期寻址：把 liba2l.dll 复制到可执行文件目录（免改 PATH）
  add_custom_command(TARGET A2lSmoke POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "$<TARGET_FILE:liba2l::liba2l>" "$<TARGET_FILE_DIR:A2lSmoke>")

  add_test(NAME A2lSmoke COMMAND A2lSmoke)
  set_tests_properties(A2lSmoke PROPERTIES FIXTURES_REQUIRED A2lGolden)

  # ----------------------------------------------------------------------------
  # A2lGolden —— 黄金回归套件（设计 §7.2 用例分组 T1~T8，批次10）
  # ----------------------------------------------------------------------------
  add_executable(A2lGolden a2l_golden_test.cpp)
  target_compile_features(A2lGolden PRIVATE cxx_std_20)
  target_link_libraries(A2lGolden PRIVATE libxcp::a2lbridge GTest::gtest_main)
  target_compile_definitions(A2lGolden PRIVATE
          A2L_GOLDEN_DIR="${A2L_GOLDEN_DIR}"
          A2L_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/a2l"
          # T9 基线预算（ms）：本机实测 5 MB 解析+快照 = 264 ms（批次13 记录），
          # 按 3 倍放宽并取整锁定 = 900 ms；非 SLA，只拦量级劣化
          A2L_PERF_REFERENCE_MS=264
           A2L_PERF_HARD_LIMIT_MS=3000)
  target_include_directories(A2lGolden PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(MSVC)
    target_compile_options(A2lGolden PRIVATE /utf-8)
  endif()
  add_custom_command(TARGET A2lGolden POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "$<TARGET_FILE:liba2l::liba2l>" "$<TARGET_FILE_DIR:A2lGolden>")
  add_test(NAME A2lGolden COMMAND A2lGolden)
  set_tests_properties(A2lGolden PROPERTIES FIXTURES_REQUIRED A2lGolden
          TIMEOUT 300)

  # ----------------------------------------------------------------------------
  # A2lE2E —— 端到端验收（批次10 工作包 10.3）：
  # A2L 端点自动建链 + B-16 运行时一致性 + 变量名→地址→SHORT_UPLOAD 读值。
  # udp_test_slave.cpp 与 libxcp_tests 共享编译（测试设施，非生产代码）。
  # ----------------------------------------------------------------------------
  add_executable(A2lE2E a2l_e2e_test.cpp udp_test_slave.cpp)
  target_compile_features(A2lE2E PRIVATE cxx_std_20)
  target_link_libraries(A2lE2E PRIVATE
          libxcp::libxcp libxcp::a2lbridge GTest::gtest_main)
  target_compile_definitions(A2lE2E PRIVATE
          A2L_GOLDEN_DIR="${A2L_GOLDEN_DIR}")
  target_include_directories(A2lE2E PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(MSVC)
    target_compile_options(A2lE2E PRIVATE /utf-8)
  endif()
  add_custom_command(TARGET A2lE2E POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "$<TARGET_FILE:liba2l::liba2l>" "$<TARGET_FILE_DIR:A2lE2E>")
  add_test(NAME A2lE2E COMMAND A2lE2E)
  set_tests_properties(A2lE2E PROPERTIES FIXTURES_REQUIRED A2lGolden
          TIMEOUT 300)

  # ----------------------------------------------------------------------------
  # MeasurementAdapterTest —— A2L 测量数据库适配器（v0.5）：
  # 用内存 Fake IA2lDatabase 校验 Find/ToPhysical 的桥接层→核心映射与错误透传。
  # 链接 libxcp_measurement_adapter（适配器副产物），不并入 libxcp_tests。
  # ----------------------------------------------------------------------------
  add_executable(MeasurementAdapterTest measurement_adapter_test.cpp)
  target_compile_features(MeasurementAdapterTest PRIVATE cxx_std_20)
  target_link_libraries(MeasurementAdapterTest PRIVATE
          libxcp_measurement_adapter
          GTest::gtest
          GTest::gtest_main)
  target_include_directories(MeasurementAdapterTest PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
  if(MSVC)
    target_compile_options(MeasurementAdapterTest PRIVATE /utf-8)
  endif()
  add_custom_command(TARGET MeasurementAdapterTest POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "$<TARGET_FILE:liba2l::liba2l>"
                  "$<TARGET_FILE_DIR:MeasurementAdapterTest>")
  gtest_discover_tests(MeasurementAdapterTest DISCOVERY_TIMEOUT 60)

  # ----------------------------------------------------------------------------
  # T9 性能基线（批次13 T13-16/T13-17）：≥5 MB 合成 A2L，**只记基线不设 SLA**。
  # 产物落 build tree（不入库）；预算常量 = 首次实测值放宽 3 倍后锁定。
  # ----------------------------------------------------------------------------
  add_test(NAME a2l_perf_gen
           COMMAND ${Python3_EXECUTABLE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/a2l_gen/gen_a2l.py"
                   --out "${A2L_GOLDEN_DIR}" --perf 5)
  set_tests_properties(a2l_perf_gen PROPERTIES
          FIXTURES_SETUP A2lPerf TIMEOUT 300)

  # 单独入口：只跑基线用例（避免把 5 MB 解析塞进 A2lGolden 全量运行里重复计次）
  add_test(NAME ParseLargeFileUnderBudget
           COMMAND A2lGolden --gtest_filter=A2lGoldenTest.ParseLargeFileUnderBudget)
  set_tests_properties(ParseLargeFileUnderBudget PROPERTIES
          FIXTURES_REQUIRED A2lPerf TIMEOUT 900)

  # ----------------------------------------------------------------------------
  # T10 隔离自动化（批次13 T13-18）：把 configure 期 P9 扫描变成 ctest 条目。
  # 两条大小写敏感扫描（桥接层禁含上游头 / 主树禁引用 A2L 栈），命中即非零退出。
  # ----------------------------------------------------------------------------
  add_test(NAME A2lIsolation
           COMMAND ${CMAKE_COMMAND}
                   "-DA2L_BRIDGE_DIR=${CMAKE_SOURCE_DIR}/thirdparty/a2l-sdk/a2lbridge"
                   "-DLIBXCP_ROOT_DIR=${CMAKE_SOURCE_DIR}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/a2l_isolation_check.cmake")
endif()
