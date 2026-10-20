# CTest 黑盒验证：日志仅写 glog 文件、不污染程序终端输出。

if(NOT DEFINED MODE OR NOT DEFINED TEST_ROOT)
  message(FATAL_ERROR "MODE 与 TEST_ROOT 必须提供")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef RUN_ID)
set(WORK_DIR "${TEST_ROOT}/${MODE}-${RUN_ID}")
file(MAKE_DIRECTORY "${WORK_DIR}")

if(MODE STREQUAL "file-output")
  if(NOT DEFINED PROBE_EXE)
    message(FATAL_ERROR "PROBE_EXE 必须提供")
  endif()
  execute_process(
    COMMAND "${PROBE_EXE}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  string(REPLACE "\r\n" "\n" stdout "${stdout}")
  string(REPLACE "\r\n" "\n" stderr "${stderr}")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "日志探针失败（${result}）：${stderr}")
  endif()
  if(NOT stdout STREQUAL "PROBE_STDOUT\n")
    message(FATAL_ERROR "日志写入改变了 stdout：'${stdout}'")
  endif()
  if(NOT stderr STREQUAL "PROBE_STDERR\n")
    message(FATAL_ERROR "glog 污染了 stderr：'${stderr}'")
  endif()

  file(GLOB_RECURSE log_files LIST_DIRECTORIES FALSE
       "${WORK_DIR}/logs/tests/libxcp_logging_probe/*")
  if(NOT log_files)
    message(FATAL_ERROR "没有在预期日志目录中生成文件")
  endif()
  set(log_content "")
  foreach(log_file IN LISTS log_files)
    get_filename_component(log_name "${log_file}" NAME)
    if(log_name MATCHES "^\\.")
      message(FATAL_ERROR "日志文件名不能以点开头：'${log_name}'")
    endif()
    if(NOT log_name MATCHES "^libxcp_logging_probe\\.(INFO|WARNING|ERROR)\\..+\\.glog$")
      message(FATAL_ERROR "日志文件名不符合程序名/级别/.glog 约定：'${log_name}'")
    endif()
    file(READ "${log_file}" part)
    string(APPEND log_content "${part}")
  endforeach()
  foreach(expected IN ITEMS
      "probe-info-17"
      "probe-warning-23"
      "probe-error-42"
      "Session state Disconnected -> Connecting; pending=none"
      "Session state Connecting -> Connected; max_cto=8 max_dto=8"
      "Session pending none -> 248 state=Connected"
      "Session pending 248 -> none state=Connected"
      "XCP control route: EV_CMD_PENDING marked; waiter notified; command not replayed"
      "XCP control route: SERV delivered asynchronously; response slot unchanged"
      "XCP control route: RES stored; waiter notified"
      "XCP response slot consumed; pending event cleared"
      "XCP response wait started timeout_ms="
      "XCP TX cmd=0xF8 bytes=3 shown_bytes=0 truncated=false redacted=true"
      "XCP RX RES pid=0xFF bytes=4 command=0xF8 shown_bytes=0 truncated=false redacted=true"
      "XCP RX DTO pid=0x00 bytes=70 command=n/a shown_bytes=64 truncated=true redacted=false"
      "XCP RX RES pid=0xFF bytes=3 command=0xFD shown_bytes=0 truncated=false redacted=true"
      "Memory read rejected: XCP session is not connected")
    string(FIND "${log_content}" "${expected}" found)
    if(found EQUAL -1)
      message(FATAL_ERROR "日志文件缺少 '${expected}'")
    endif()
  endforeach()
  # 与摘要实际格式 0xHH 0xHH 一起检查，避免错误的负向断言漏检。
  foreach(secret IN ITEMS "A5 5A" "C1 5E" "0xA5 0x5A" "0xC1 0x5E")
    string(FIND "${log_content}" "${secret}" found)
    if(NOT found EQUAL -1)
      message(FATAL_ERROR "日志文件暴露 Seed/Key 字节 '${secret}'")
    endif()
  endforeach()
elseif(MODE STREQUAL "control-recovery")
  if(NOT DEFINED TEST_EXE)
    message(FATAL_ERROR "TEST_EXE 必须提供")
  endif()
  execute_process(
    COMMAND "${TEST_EXE}"
            "--gtest_filter=RecoveryTimeout.TimeoutTriggersSynchThenRetrySucceeds:EventCmdPending.RestartsTimerWithoutResendingCommand"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "控制面恢复测试失败（${result}）：${stdout} ${stderr}")
  endif()
  if(NOT stderr STREQUAL "")
    message(FATAL_ERROR "控制面日志污染了 stderr：'${stderr}'")
  endif()
  file(GLOB_RECURSE log_files LIST_DIRECTORIES FALSE
       "${WORK_DIR}/logs/tests/libxcp_tests/*")
  if(NOT log_files)
    message(FATAL_ERROR "控制面恢复测试没有生成文件日志")
  endif()
  set(log_content "")
  foreach(log_file IN LISTS log_files)
    file(READ "${log_file}" part)
    string(APPEND log_content "${part}")
  endforeach()
  foreach(expected IN ITEMS
      "Session state Connected -> Recovering"
      "XCP recovery initialized: response slot cleared; awaiting ERR_CMD_SYNCH"
      "XCP control route: ERR_CMD_SYNCH confirms recovery; SYNCH waiter notified"
      "XCP recovery confirmed by ERR_CMD_SYNCH"
      "Session state Recovering -> Connected"
      "XCP EV_CMD_PENDING restarted response timer timeout_ms=400; command not replayed")
    string(FIND "${log_content}" "${expected}" found)
    if(found EQUAL -1)
      message(FATAL_ERROR "控制面日志缺少 '${expected}'")
    endif()
  endforeach()
elseif(MODE STREQUAL "unwritable")
  if(NOT DEFINED PROBE_EXE)
    message(FATAL_ERROR "PROBE_EXE 必须提供")
  endif()
  file(WRITE "${WORK_DIR}/logs" "阻止创建日志目录")
  execute_process(
    COMMAND "${PROBE_EXE}"
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT result EQUAL 2)
    message(FATAL_ERROR "不可写日志路径应返回 2，实际为 '${result}'")
  endif()
  if(NOT stdout STREQUAL "")
    message(FATAL_ERROR "初始化失败前不应产生程序 stdout：'${stdout}'")
  endif()
  string(FIND "${stderr}" "日志初始化失败" diagnostic)
  if(diagnostic EQUAL -1)
    message(FATAL_ERROR "缺少普通启动诊断：'${stderr}'")
  endif()
  if(IS_DIRECTORY "${WORK_DIR}/logs")
    message(FATAL_ERROR "探针意外覆盖了阻塞日志路径")
  endif()
elseif(MODE STREQUAL "discovery")
  if(NOT DEFINED TEST_EXE)
    message(FATAL_ERROR "TEST_EXE 必须提供")
  endif()
  file(WRITE "${WORK_DIR}/logs" "发现模式不应创建日志目录")
  execute_process(
    COMMAND "${TEST_EXE}" --gtest_list_tests
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "GoogleTest discovery 失败（${result}）：${stderr}")
  endif()
  if(IS_DIRECTORY "${WORK_DIR}/logs")
    message(FATAL_ERROR "GoogleTest discovery 创建了日志目录")
  endif()
  if(NOT stderr STREQUAL "")
    message(FATAL_ERROR "GoogleTest discovery 的 stderr 不为空：'${stderr}'")
  endif()
elseif(MODE STREQUAL "cli-error")
  if(NOT DEFINED PROGRAM_EXE)
    message(FATAL_ERROR "PROGRAM_EXE 必须提供")
  endif()
  execute_process(
    COMMAND "${PROGRAM_EXE}" --not-a-valid-option
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
  if(NOT result EQUAL 2)
    message(FATAL_ERROR "非法 CLI 应返回 2，实际为 '${result}'")
  endif()
  if(NOT stdout STREQUAL "")
    message(FATAL_ERROR "非法 CLI 不应改变 stdout：'${stdout}'")
  endif()
  string(FIND "${stderr}" "Error:" diagnostic)
  if(diagnostic EQUAL -1)
    message(FATAL_ERROR "缺少普通 CLI 错误诊断：'${stderr}'")
  endif()
  string(FIND "${stderr}" "Logging before InitGoogleLogging" premature_glog)
  if(NOT premature_glog EQUAL -1)
    message(FATAL_ERROR "glog 在初始化前污染 stderr：'${stderr}'")
  endif()
  if(IS_DIRECTORY "${WORK_DIR}/logs")
    message(FATAL_ERROR "非法 CLI 路径不应初始化日志或创建目录")
  endif()
else()
  message(FATAL_ERROR "未知 MODE '${MODE}'")
endif()
