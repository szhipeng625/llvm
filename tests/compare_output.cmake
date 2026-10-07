# 运行一个 e2e 可执行文件,把它 stdout 与期望文件逐字符比对。
#
#   cmake -DEXE=<可执行文件> -DEXPECTED=<期望输出文件> -P compare_output.cmake
#
# 用 CMake 脚本而不是 shell 来做,是为了在 Windows/MinGW 下也能跨平台地
# 规范化换行 —— 程序输出是 CRLF,而期望文件是 LF,不处理会全部误报失败。
#
# ⚠️ 被比较的输出必须是 **ASCII**。
# Windows 上 execute_process 会用当前代码页重新解码子进程的 stdout,而
# file(READ) 是原样读字节,于是同一段 UTF-8 中文在两边会变成不同的字节序列,
# 比对必然失败(实测:报错信息里"期望"一侧正常、"实际"一侧是乱码)。
# 现在所有 .expected 都是 ASCII,所以没踩到;要加含中文输出的用例,
# 得先解决这个解码不对称,不能只改期望文件。
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

# 统一换行符,并去掉首尾空白。
#
# 两边都要规范化。这里之前只处理了 actual —— 于是"期望文件是 LF、程序输出是
# CRLF"就会误报失败,而 .expected 究竟存成哪种换行取决于它是由什么工具生成的
# (手工写的多半是 LF,Python 生成的是 CRLF)。用 string(REPLACE) 而不是
# string(REGEX REPLACE):这里是字面替换,不需要正则。
foreach(var actual expected)
  string(REPLACE "\r\n" "\n" ${var} "${${var}}")
  string(REPLACE "\r" "\n" ${var} "${${var}}")
  string(STRIP "${${var}}" ${var})
endforeach()

if(NOT actual STREQUAL expected)
  message(FATAL_ERROR
    "输出与期望不符\n"
    "--- 期望 ---\n${expected}\n"
    "--- 实际 ---\n${actual}\n"
    "-------------")
endif()
