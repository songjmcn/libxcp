# 验证实际 CLI 在进入交互界面或建连之前拒绝重复 profile。
foreach(first IN ITEMS auto auto-legacy)
  if(first STREQUAL "auto")
    set(second "auto-legacy")
  else()
    set(second "auto")
  endif()
  execute_process(
    COMMAND "${XCP_MASTER}" --tcp-profile "${first}" --tcp-profile "${second}" --help
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 10)
  if(result STREQUAL "0" OR NOT error MATCHES "--tcp-profile must not be specified more than once")
    message(FATAL_ERROR "重复 profile 未按预期拒绝: ${first}/${second}: ${result}; ${output}; ${error}")
  endif()
endforeach()
