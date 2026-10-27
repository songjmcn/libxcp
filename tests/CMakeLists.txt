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

if(WIN32)
  # 与 libxcp 本体一致：关闭 Windows 头的 min/max 宏，避免 std::min/std::max
  # 在含 winsock 头的测试 TU 里被宏展开（MSVC C2589/C2059）。
  add_compile_definitions(NOMINMAX)
endif()

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
# GTest上游将FetchContent头文件标记为SYSTEM；AppleClang因此可能优先
# 命中/usr/local/include中的旧安装版，造成头文件与本次构建archive ABI不一致。
# 取消其SYSTEM分类，让本地Fetched头先于编译器默认系统目录搜索。
set_property(TARGET gtest PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
set_property(TARGET gtest_main PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")

# 使用项目自有 main 管理测试进程级日志；测试发现模式不初始化 glog。
add_library(libxcp_test_main STATIC test_main.cpp)
target_compile_features(libxcp_test_main PRIVATE cxx_std_20)
target_link_libraries(libxcp_test_main PUBLIC GTest::gtest)
libxcp_configure_program_logging(libxcp_test_main)

add_executable(libxcp_tests
        protocol_types_test.cpp
        xcp_error_test.cpp
        command_codec_test.cpp
        response_parser_test.cpp
        session_test.cpp
        udp_header_codec_test.cpp
        ethernet_header_codec_test.cpp
        tcp_stream_decoder_test.cpp
        tcp_framing_negotiation_test.cpp
        tcp_transport_test.cpp
        udp_transport_test.cpp
        udp_boundary_framing_test.cpp
        event_poller_test.cpp
        link_budget_test.cpp
        udp_test_slave_test.cpp
        memory_access_test.cpp
        recovery_test.cpp
        seed_key_test.cpp
        xcp_master_integration_test.cpp
        xcp_daq_test.cpp
        xcp_udp_loopback_test.cpp
        udp_test_slave.cpp
        tcp_test_slave.cpp
        xcp_tcp_loopback_test.cpp
        dto_envelope_decoder_test.cpp
        get_daq_event_info_test.cpp
        measurement_planner_test.cpp
        measurement_session_test.cpp
        daq_timestamp_test.cpp
        measurement_session_hardening_test.cpp
        logging_test.cpp
        version_resolver_test.cpp)

target_link_libraries(libxcp_tests PRIVATE
        libxcp::libxcp
        GTest::gtest
        libxcp_test_main)

# 测试设施头文件目录（mock_transport.hpp 等），仅 tests 内部可见；
# 同时把源码根目录加进来，使测试可以用 `#include "src/socket_util.hpp"`
# 直接覆盖内部 POD 与平台错误分类逻辑（如 IsConnectionLostCode）。
target_include_directories(libxcp_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR})

if(MSVC)
  target_compile_options(libxcp_tests PRIVATE /utf-8)
endif()

include(GoogleTest)
gtest_discover_tests(libxcp_tests DISCOVERY_TIMEOUT 60)

# ----------------------------------------------------------------------------
# 假 Socket 注入测试（code-plan/历史遗漏项_详细方案.md §1.3 L-A2）
#
# 独立可执行文件 + RUN_SERIAL 是刻意的：`SetSocketOpsForTesting()` 的注入是
# **进程级**的，若与其它用例并行在同一进程内切换实现，会污染它们的 Socket
# 行为。独立进程 + 串行执行使"全局注入"的副作用被限制在本文件内。
# ----------------------------------------------------------------------------
add_executable(TcpFakeSocketTest tcp_fake_socket_test.cpp socket_ops_pod_test.cpp)

target_link_libraries(TcpFakeSocketTest PRIVATE
        libxcp::libxcp
        GTest::gtest
        libxcp_test_main)

# 需要 `#include "src/socket_util.hpp"`：假实现要派生自内部的 SocketOps
target_include_directories(TcpFakeSocketTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR})

if(MSVC)
  target_compile_options(TcpFakeSocketTest PRIVATE /utf-8)
endif()

gtest_discover_tests(TcpFakeSocketTest
        DISCOVERY_TIMEOUT 60
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

# ----------------------------------------------------------------------------
# XCPlite 协议调试集成测试（code-plan/XCPlite_Slave_协议调试集成计划.md）
#
# 仅 LIBXCP_BUILD_XCPLITE_SLAVE=ON 时构建：
#   xcp_test_slave        —— 基于 XCPlite 的 Slave 独立进程（tests/xcp_test_slave/）
#   XcpliteIntegration    —— 真实 UDP + 真实 Slave 的集成测试（子进程由夹具管理）
# A2L 读取用例（xcplite_a2l_read_test.cpp）依赖桥接层；A2L 栈现为必编组件，
# 随本目标无条件编入。
# ----------------------------------------------------------------------------
if(LIBXCP_BUILD_XCPLITE_SLAVE)
  add_subdirectory(xcp_test_slave)

  add_executable(XcpliteIntegration
          xcplite_slave_fixture.cpp
          xcplite_slave_fixture_test.cpp
          udp_test_slave.cpp
          xcplite_protocol_test.cpp
          xcplite_write_test.cpp
          xcplite_daq_test.cpp
          # A2L 读取用例：依赖桥接层（libxcp::a2lbridge + 运行期 liba2l.dll）
          xcplite_a2l_read_test.cpp
          # 测量子系统 L3 全链路（v0.9）：桥接 + 适配器 + MeasurementSession
          xcplite_measurement_test.cpp
          # B-T5：XCPlite 真实 TCP 互操作 Gate（计划 §8.2）
          xcplite_tcp_interop_test.cpp)
  target_compile_features(XcpliteIntegration PRIVATE cxx_std_20)
  target_link_libraries(XcpliteIntegration PRIVATE
          libxcp::libxcp
          libxcp::a2lbridge libxcp_measurement_adapter
          GTest::gtest
          libxcp_test_main)
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
  # 运行期寻址：把 liba2l.dll 复制到可执行文件目录（免改 PATH）
  add_custom_command(TARGET XcpliteIntegration POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "$<TARGET_FILE:liba2l::liba2l>"
                  "$<TARGET_FILE_DIR:XcpliteIntegration>")

  # examples 子目录在 tests 之后配置；这里登记源文件与延迟求值的目标路径，
  # 由 examples/xcp_slave_demo/CMakeLists.txt 在目标创建后补充构建依赖。
  if(LIBXCP_BUILD_EXAMPLES)
    target_sources(XcpliteIntegration PRIVATE
            xcp_slave_demo_model_test.cpp
            xcp_slave_demo_integration_test.cpp
            "${PROJECT_SOURCE_DIR}/examples/xcp_slave_demo/slave_model.cpp")
    target_include_directories(XcpliteIntegration PRIVATE
            "${PROJECT_SOURCE_DIR}/examples/xcp_slave_demo")
    target_compile_definitions(XcpliteIntegration PRIVATE
            XCP_SLAVE_DEMO_EXECUTABLE="$<TARGET_FILE:xcp_slave_demo>"
            XCP_THIRDPARTY_DIR="${PROJECT_SOURCE_DIR}/thirdparty")
  endif()

  # 该目标包含从站日志布点的黑盒用例（xcp_slave_demo_integration_test.cpp
  # 的 ServiceEmitsPeriodicHeartbeatLog），需要与示例一致的 0/1 开关。
  libxcp_configure_program_logging(XcpliteIntegration)

  gtest_discover_tests(XcpliteIntegration DISCOVERY_TIMEOUT 120)
endif()

# ----------------------------------------------------------------------------
# A2L 专项测试（A2lSmoke/A2lGolden/A2lE2E/MeasurementAdapterTest/T9/T10）
# （code-plan/A2L_CMake最小开发验证集_R4.md §5、批次10/13）
#
# A2L 栈为必编组件，本段随 LIBXCP_BUILD_TESTS 无条件构建。
# A2lSmoke 不依赖 GoogleTest（独立 main + 退出码）。
# ----------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 公共 TCP/UDP 层禁止原生 Socket API，平台实现仅允许位于 detail/socket/*_{win,posix}.cpp。
add_test(NAME SocketPlatformBoundary
         COMMAND ${Python3_EXECUTABLE}
                 "${CMAKE_CURRENT_SOURCE_DIR}/check_socket_platform_boundary.py"
                 "${PROJECT_SOURCE_DIR}")

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
libxcp_configure_program_logging(A2lSmoke)
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
target_link_libraries(A2lGolden PRIVATE libxcp::a2lbridge libxcp_test_main
        # T-05：需要 libxcp 的 PacketAlignment 强类型与 XcpFrameBytes()，
        # 用以断言 A2L PACKET_ALIGNMENT 贯通到本侧 Tail 公式
        libxcp::libxcp)
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
        libxcp::libxcp libxcp::a2lbridge libxcp_test_main)
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
        libxcp_test_main)
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
# 界面性能基线（A2L 变大后交互变慢的回归门）：
# 合成 40 MB 样本 ≈ 198k 符号，复现现场 A2L 的候选规模。
# 样本只落 build tree（不入库）；也可用环境变量 LIBXCP_UI_PERF_A2L 指向真实文件。
# ----------------------------------------------------------------------------
add_test(NAME a2l_ui_perf_gen
         COMMAND ${Python3_EXECUTABLE}
                 "${CMAKE_CURRENT_SOURCE_DIR}/a2l_gen/gen_a2l.py"
                 --out "${A2L_GOLDEN_DIR}" --perf 40)
set_tests_properties(a2l_ui_perf_gen PROPERTIES
        FIXTURES_SETUP A2lUiPerf TIMEOUT 600)

# ----------------------------------------------------------------------------
# T10 隔离自动化（批次13 T13-18）：把 configure 期 P9 扫描变成 ctest 条目。
# 两条大小写敏感扫描（桥接层禁含上游头 / 主树禁引用 A2L 栈），命中即非零退出。
# ----------------------------------------------------------------------------
add_test(NAME A2lIsolation
         COMMAND ${CMAKE_COMMAND}
                 "-DA2L_BRIDGE_DIR=${CMAKE_SOURCE_DIR}/thirdparty/a2l-sdk/a2lbridge"
                 "-DLIBXCP_ROOT_DIR=${CMAKE_SOURCE_DIR}"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/a2l_isolation_check.cmake")

# XCP Master CLI tool smoke tests (requires LIBXCP_BUILD_TOOLS=ON).
if(LIBXCP_BUILD_TOOLS)
  add_test(NAME XcpMasterToolHelp COMMAND xcp_master --help)
  add_test(NAME XcpMasterToolRejectsMissingArgs COMMAND xcp_master)
  set_tests_properties(XcpMasterToolRejectsMissingArgs PROPERTIES WILL_FAIL TRUE)
  add_test(NAME XcpMasterToolRejectsPollingIntervalInDaq
           COMMAND xcp_master --a2l ignored.a2l --host 127.0.0.1 --port 5555
                   --mode daq --interval-ms 500)
  # DTO 布局改为按取证自动选择后，--daq-profile 已被移除；
  # 该选项现应作为未知参数被拒绝（不再存在厂商 profile 开关）。
  add_test(NAME XcpMasterToolRejectsRemovedDaqProfileOption
           COMMAND xcp_master --a2l ignored.a2l --host 127.0.0.1 --port 5555
                   --daq-profile xcplite)
  set_tests_properties(XcpMasterToolRejectsPollingIntervalInDaq
                       XcpMasterToolRejectsRemovedDaqProfileOption
                       PROPERTIES WILL_FAIL TRUE)

  add_executable(XcpMasterSymbolPolicyTest
          xcp_master_symbol_policy_test.cpp
          xcp_master_calibration_policy_test.cpp
          xcp_master_tui_model_test.cpp
          xcp_master_symbol_display_test.cpp
          # B-T5：传输层选择解析层（纯函数）的规则级用例。
          xcp_master_transport_selection_test.cpp)
  target_compile_features(XcpMasterSymbolPolicyTest PRIVATE cxx_std_20)
  target_include_directories(XcpMasterSymbolPolicyTest PRIVATE
          ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
  # B-T5：传输层选择解析层使用 `XcpProtocolVersion::ToString()` 等实现符号，
  # 故需链接 libxcp 本体（a2lbridge 自身不导出这些符号）。
  target_link_libraries(XcpMasterSymbolPolicyTest PRIVATE
          libxcp::libxcp libxcp::a2lbridge GTest::gtest libxcp_test_main
          Threads::Threads)
  if(MSVC)
    target_compile_options(XcpMasterSymbolPolicyTest PRIVATE /utf-8)
  endif()
  gtest_discover_tests(XcpMasterSymbolPolicyTest DISCOVERY_TIMEOUT 60)

  # --------------------------------------------------------------------------
  # 预览行索引与增量刷新（P1）：纯头文件逻辑，只依赖标准库与 GTest，
  # 因此不需要 FTXUI，也不需要链接 libxcp 本体。
  # --------------------------------------------------------------------------
  add_executable(XcpMasterPreviewRefreshTest xcp_master_preview_refresh_test.cpp
          xcp_master_preview_window_test.cpp)
  target_compile_features(XcpMasterPreviewRefreshTest PRIVATE cxx_std_20)
  target_include_directories(XcpMasterPreviewRefreshTest PRIVATE
          ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(XcpMasterPreviewRefreshTest PRIVATE
          GTest::gtest libxcp_test_main)
  if(MSVC)
    target_compile_options(XcpMasterPreviewRefreshTest PRIVATE /utf-8)
  endif()
  gtest_discover_tests(XcpMasterPreviewRefreshTest DISCOVERY_TIMEOUT 60)

  # --------------------------------------------------------------------------
  # 界面性能基准：headless 渲染（Screen::Create + Render，不需要 TTY）。
  # FTXUI 由 tools/xcp_master/CMakeLists.txt 在 tools 子目录中 FetchContent 引入，
  # 而 tools 先于 tests add_subdirectory，故此处只需判断目标是否已存在。
  # --------------------------------------------------------------------------
  if(TARGET ftxui::ftxui)
    add_executable(XcpMasterRenderBenchTest xcp_master_render_bench_test.cpp)
    target_compile_features(XcpMasterRenderBenchTest PRIVATE cxx_std_20)
    target_include_directories(XcpMasterRenderBenchTest PRIVATE
            ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(XcpMasterRenderBenchTest PRIVATE
            libxcp::a2lbridge ftxui::ftxui GTest::gtest libxcp_test_main)
    target_compile_definitions(XcpMasterRenderBenchTest PRIVATE
            # 40 MB 合成样本（约 198k 符号）的路径；也可用环境变量
            # LIBXCP_UI_PERF_A2L 指向真实大 A2L。
            XCP_UI_PERF_A2L="${A2L_GOLDEN_DIR}/golden_perf_40mb.a2l")
    if(MSVC)
      target_compile_options(XcpMasterRenderBenchTest PRIVATE /utf-8)
    endif()
    # liba2l 由 A2lBridge 运行时加载，须与测试可执行文件同目录。
    add_custom_command(TARGET XcpMasterRenderBenchTest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:liba2l::liba2l>"
                    "$<TARGET_FILE_DIR:XcpMasterRenderBenchTest>")
    # 三条用例共用同一个 40 MB 样本夹具（a2l_ui_perf_gen 生成；缺失时检索用例
    # 自行跳过）。gtest_discover_tests 在构建期发现用例名，故夹具属性按目标整体设置。
    gtest_discover_tests(XcpMasterRenderBenchTest DISCOVERY_TIMEOUT 60
            PROPERTIES FIXTURES_REQUIRED A2lUiPerf TIMEOUT 900)
  endif()
endif()

# ----------------------------------------------------------------------------
# A2lCalibrationAccessTest —— CALIBRATION_ACCESS 可写性判定（方案 A）
# 语料内联在用例里（写到临时目录），不依赖 golden 夹具。
# ----------------------------------------------------------------------------
add_executable(A2lCalibrationAccessTest a2l_calibration_access_test.cpp)
target_compile_features(A2lCalibrationAccessTest PRIVATE cxx_std_20)
target_link_libraries(A2lCalibrationAccessTest PRIVATE
        libxcp::a2lbridge GTest::gtest libxcp_test_main)
if(MSVC)
  target_compile_options(A2lCalibrationAccessTest PRIVATE /utf-8)
endif()
# 运行期寻址：把 liba2l.dll 复制到可执行文件目录（免改 PATH）
add_custom_command(TARGET A2lCalibrationAccessTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:A2lCalibrationAccessTest>")
gtest_discover_tests(A2lCalibrationAccessTest DISCOVERY_TIMEOUT 120)

# ----------------------------------------------------------------------------
# A2lIfDataConflictTest —— XCP vs XCPplus 的对齐"声明状态"差异（历史遗漏项 L-B2）
# 语料内联在用例里（写到临时目录），不依赖 golden 夹具。
# ----------------------------------------------------------------------------
add_executable(A2lIfDataConflictTest a2l_if_data_conflict_test.cpp)
target_compile_features(A2lIfDataConflictTest PRIVATE cxx_std_20)
target_link_libraries(A2lIfDataConflictTest PRIVATE
        libxcp::a2lbridge GTest::gtest libxcp_test_main)
if(MSVC)
  target_compile_options(A2lIfDataConflictTest PRIVATE /utf-8)
endif()
add_custom_command(TARGET A2lIfDataConflictTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:A2lIfDataConflictTest>")
gtest_discover_tests(A2lIfDataConflictTest DISCOVERY_TIMEOUT 120)

# ----------------------------------------------------------------------------
# A2lMultiEndpointTest —— 多实例传输层端点（历史遗漏项 L-C，ABI v10）
# 语料内联在用例里（写到临时目录），不依赖 golden 夹具。
# ----------------------------------------------------------------------------
add_executable(A2lMultiEndpointTest a2l_multi_endpoint_test.cpp)
target_compile_features(A2lMultiEndpointTest PRIVATE cxx_std_20)
target_link_libraries(A2lMultiEndpointTest PRIVATE
        libxcp::a2lbridge GTest::gtest libxcp_test_main)
if(MSVC)
  target_compile_options(A2lMultiEndpointTest PRIVATE /utf-8)
endif()
add_custom_command(TARGET A2lMultiEndpointTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:liba2l::liba2l>"
                "$<TARGET_FILE_DIR:A2lMultiEndpointTest>")
gtest_discover_tests(A2lMultiEndpointTest DISCOVERY_TIMEOUT 120)

# Boost.Locale-backed A2L log timestamp compatibility test.
add_executable(A2lLogTimestampTest a2l_log_timestamp_test.cpp)
target_compile_features(A2lLogTimestampTest PRIVATE cxx_std_20)
target_include_directories(A2lLogTimestampTest PRIVATE
        ${PROJECT_SOURCE_DIR}/thirdparty/a2l-sdk/src)
target_link_libraries(A2lLogTimestampTest PRIVATE
        a2l Boost::locale GTest::gtest libxcp_test_main)
gtest_discover_tests(A2lLogTimestampTest DISCOVERY_TIMEOUT 60)

if(LIBXCP_ENABLE_LOG)
  # 独立探针用于黑盒检查日志文件、标准流隔离、不可写目录与 discovery。
  add_executable(libxcp_logging_probe logging_probe.cpp)
  target_compile_features(libxcp_logging_probe PRIVATE cxx_std_20)
  target_link_libraries(libxcp_logging_probe PRIVATE libxcp::libxcp)
  libxcp_configure_program_logging(libxcp_logging_probe)

  set(LIBXCP_LOG_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/log-behavior")
  add_test(NAME LibxcpLogFileOutput
           COMMAND ${CMAKE_COMMAND}
                   "-DMODE=file-output"
                   "-DPROBE_EXE=$<TARGET_FILE:libxcp_logging_probe>"
                   "-DTEST_ROOT=${LIBXCP_LOG_TEST_ROOT}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/logging_behavior_test.cmake")
  add_test(NAME LibxcpLogControlRecovery
           COMMAND ${CMAKE_COMMAND}
                   "-DMODE=control-recovery"
                   "-DTEST_EXE=$<TARGET_FILE:libxcp_tests>"
                   "-DTEST_ROOT=${LIBXCP_LOG_TEST_ROOT}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/logging_behavior_test.cmake")
  add_test(NAME LibxcpLogUnwritableDirectory
           COMMAND ${CMAKE_COMMAND}
                   "-DMODE=unwritable"
                   "-DPROBE_EXE=$<TARGET_FILE:libxcp_logging_probe>"
                   "-DTEST_ROOT=${LIBXCP_LOG_TEST_ROOT}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/logging_behavior_test.cmake")
  add_test(NAME LibxcpLogDiscoveryNoDirectory
           COMMAND ${CMAKE_COMMAND}
                   "-DMODE=discovery"
                   "-DTEST_EXE=$<TARGET_FILE:libxcp_tests>"
                   "-DTEST_ROOT=${LIBXCP_LOG_TEST_ROOT}"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/logging_behavior_test.cmake")
endif()

# XCPlite V1.2 从站示例的模型层单测（计划 §8：只做模型层，端到端靠手工联调）。
# 目标刻意**只编译** examples/xcp_slave_demo_v12/slave_model.cpp，不链接
# xcplite_v12，也不链接 libxcp，避免与 XcpliteIntegration 已链接的 V2.2 xcplite
# 产生同名符号冲突。
add_executable(XcpSlaveV12ModelTest
        xcp_slave_demo_v12_model_test.cpp
        ${PROJECT_SOURCE_DIR}/examples/xcp_slave_demo_v12/slave_model.cpp)
target_compile_features(XcpSlaveV12ModelTest PRIVATE cxx_std_20)
target_include_directories(XcpSlaveV12ModelTest PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/examples/xcp_slave_demo_v12)
target_link_libraries(XcpSlaveV12ModelTest PRIVATE
        GTest::gtest libxcp_test_main)
if(MSVC)
  target_compile_options(XcpSlaveV12ModelTest PRIVATE /utf-8)
endif()
gtest_discover_tests(XcpSlaveV12ModelTest DISCOVERY_TIMEOUT 60)
