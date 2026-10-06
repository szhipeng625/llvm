# 运行一个 e2e 可执行文件,把它 stdout 与期望文件逐字符比对。
#
#   cmake -DEXE=<可执行文件> -DEXPECTED=<期望输出文件> -P compare_output.cmake
#
# 用 CMake 脚本而不是 shell 来做,是为了在 Windows/MinGW 下也能跨平台地
# 规范化换行 —— 程序输出是 CRLF,而期望文件是 LF,不处理会全部误报失败。
if(NOT DEFINED EXE OR NOT DEFINED EXPECTED)
  message(FATAL_ERROR "需要 -DEXE=<exe> 和 -DEXPECTED=<file>")
endif()

# 可选:用 -DINPUT=<file> 给程序喂 stdin(测 input(...) 的用例需要)
if(DEFINED INPUT)
  execute_process(
    COMMAND "${EXE}"
    INPUT_FILE "${INPUT}"
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE err
    RESULT_VARIABLE rc
  )
else()
  execute_process(
    COMMAND "${EXE}"
    OUTPUT_VARIABLE actual
    ERROR_VARIABLE err
    RESULT_VARIABLE rc
  )
endif()

if(NOT rc EQUAL 0)
  message(FATAL_ERROR "程序退出码 ${rc}\nstderr:\n${err}")
endif()

file(READ "${EXPECTED}" expected)

# 统一换行符,并去掉首尾空白
foreach(var actual expected)
  string(REGEX REPLACE "\r\n" "\n" ${var} "${${var}}")
  string(REGEX REPLACE "\r" "\n" ${var} "${${var}}")
  string(STRIP "${${var}}" ${var})
endforeach()

if(NOT actual STREQUAL expected)
  message(FATAL_ERROR
    "输出与期望不符\n"
    "--- 期望 ---\n${expected}\n"
    "--- 实际 ---\n${actual}\n"
    "-------------")
endif()
