# 验证实际 CLI 的工程配置文件选项（-c/--config）在进入交互界面之前的行为：
# 取值缺失/重复/空值/文件不存在都必须以非零退出码结束并给出确定的中文诊断，
# 而不是静默忽略或带着默认值继续启动。
foreach(case_duplicate IN ITEMS 1)
  if(case_duplicate)
    foreach(arg IN ITEMS --config -c)
      execute_process(
        COMMAND "${XCP_MASTER}" "${arg}" a.json "${arg}" b.json
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
      if(result STREQUAL "0" OR NOT error MATCHES "--config must not be specified more than once")
        message(FATAL_ERROR "${arg} 重复指定未按预期拒绝: ${result}; ${output}; ${error}")
      endif()
    endforeach()
  endif()
endforeach()

foreach(arg IN ITEMS --config -c)
  execute_process(
    COMMAND "${XCP_MASTER}" "${arg}" ""
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
  if(result STREQUAL "0" OR NOT error MATCHES "--config must not be empty")
    message(FATAL_ERROR "${arg} 空值未按预期拒绝: ${result}; ${output}; ${error}")
  endif()
endforeach()

# --a2l 与 -c 至少给一个；两者都没有时必须提示这一条，否则用户不知道 -c 存在。
execute_process(
  COMMAND "${XCP_MASTER}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(result STREQUAL "0" OR NOT error MATCHES "--a2l is required \\(or supply --config/-c\\)")
  message(FATAL_ERROR "缺少全部必填参数时的提示不正确: ${result}; ${output}; ${error}")
endif()

# 不存在的配置文件：诊断里必须带上用户给出的路径，便于排查路径拼写。
set(missing_path "${CMAKE_CURRENT_BINARY_DIR}/xcp_master_missing_project_file.json")
file(REMOVE "${missing_path}")
execute_process(
  COMMAND "${XCP_MASTER}" -c "${missing_path}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(result STREQUAL "0" OR NOT error MATCHES "读取工程配置文件失败")
  message(FATAL_ERROR "缺失配置文件未按预期拒绝: ${result}; ${output}; ${error}")
endif()
string(FIND "${error}" "xcp_master_missing_project_file.json" position)
if(position EQUAL -1)
  message(FATAL_ERROR "缺失配置文件的诊断未包含原路径: ${error}")
endif()
