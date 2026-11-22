# 验证「传输层与端点从 A2L 推导」在**进入交互界面之前**的行为：
#
# 这一步能自动断言的范围有限——`main()` 在加载 A2L 之前就要求交互式终端
# （`tools/xcp_master/main.cpp` 的 `IsInteractiveTerminal()` 守卫），因此把
# 输出重定向到管道时，双路嗅探（需要真的去连 TCP/UDP）根本不会执行。
# 真要验证嗅探，只能用交互式终端人工做（见实施记录 §6 人工验收）。
#
# 所以本脚本钉的是"参数层"的四条不变量，它们正好是这次改动最容易回归的地方：
#   1. 只给 `--a2l` 不再被判为缺少 --host/--port（规则 9 放宽后可回落到 A2L）；
#   2. 显式 `--transport tcp` 未给 profile 时给出确定的提示并继续（等效 auto）；
#   3. `--help` 里能查到端点来源与嗅探的说明，用户不用翻源码猜；
#   4. 只给 `--host` 必须在发起任何连接之前被成对校验拒绝（PR#24 评审问题 3）。
#
# 任何一条不成立都说明启动链路被改回去了，属于必须拦住的功能回归。

set(a2l_path "${CMAKE_CURRENT_BINARY_DIR}/xcp_master_transport_inference_dummy.a2l")
file(WRITE "${a2l_path}" "; 本文件不需要合法：断言都发生在解析 A2L 之前\n")

# ---- 1. 只给 --a2l：不得再要求 --host/--port ----
execute_process(
  COMMAND "${XCP_MASTER}" --a2l "${a2l_path}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(error MATCHES "为必填项")
  message(FATAL_ERROR
    "只给 --a2l 时仍在要求 --host/--port，规则 9 放宽未生效: ${error}")
endif()
# 必须走到"需要交互式终端"这一步，才证明参数校验整体通过。
if(NOT error MATCHES "交互式终端")
  message(FATAL_ERROR
    "只给 --a2l 时未走到交互终端守卫，说明更早处被拒: ${result}; ${output}; ${error}")
endif()

# ---- 2. 显式 --transport tcp 未给 profile：提示等效 auto ----
execute_process(
  COMMAND "${XCP_MASTER}" --a2l "${a2l_path}" --transport tcp
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
set(combined "${output}${error}")
if(NOT combined MATCHES "未指定 --tcp-profile")
  message(FATAL_ERROR
    "显式 --transport tcp 缺 profile 时未给出提示: ${result}; ${combined}")
endif()
if(NOT combined MATCHES "auto")
  message(FATAL_ERROR
    "提示里未说明等效 --tcp-profile auto: ${combined}")
endif()
if(NOT error MATCHES "交互式终端")
  message(FATAL_ERROR
    "提示之后未继续走到交互终端守卫: ${result}; ${output}; ${error}")
endif()

# ---- 3. --help 必须解释端点来源与嗅探 ----
execute_process(
  COMMAND "${XCP_MASTER}" --help
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
set(combined "${output}${error}")
foreach(keyword IN ITEMS "XCP_ON_TCP_IP" "XCP_ON_UDP_IP")
  string(FIND "${combined}" "${keyword}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "--help 未提到 ${keyword}（端点可取自 A2L）")
  endif()
endforeach()

# ---- 4. 只给 --host：必须在发起任何连接之前成对拒绝 ----
# 成对校验原先只在 ResolveTransportSelection() 里，而嗅探在它之前——只给
# --host 时嗅探会先回落到 A2L 端点，等于把 A2L 里的 ECU 连了一遍才发现参数
# 不全。前移之后这条命令必须停在参数解析，输出里不能出现任何试连记录。
execute_process(
  COMMAND "${XCP_MASTER}" --a2l "${a2l_path}" --host 127.0.0.1
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
set(combined "${output}${error}")
if(NOT combined MATCHES "--host 与 --port 必须成对出现")
  message(FATAL_ERROR
    "只给 --host 未被成对校验拒绝: ${result}; ${combined}")
endif()
if(combined MATCHES "试连")
  message(FATAL_ERROR
    "端点不完整时仍发起了嗅探试连（评审问题 3 未修复）: ${combined}")
endif()
if(NOT result EQUAL 2)
  message(FATAL_ERROR
    "端点不完整的退出码应为 2（配置不全），实际 ${result}")
endif()

file(REMOVE "${a2l_path}")
message(STATUS "传输层推导 CLI 冒烟通过")
