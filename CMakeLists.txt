cmake_minimum_required(VERSION 3.20)
project(PyLite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行文件和 DLL 都放到同一个 bin/ 下,这样 exe 能找到 pylite_runtime.dll,
# 不用额外配置 PATH。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT WIN32)
  message(FATAL_ERROR "PyLite 目前只针对 Windows/MinGW 构建")
endif()

# ---------------------------------------------------------------------------
# LLVM
# ---------------------------------------------------------------------------
# 这个 MSYS2 包是 shared 模式(llvm-config --shared-mode 返回 shared),
# 所以 LLVMConfig.cmake 导出了一个名为 `LLVM` 的 imported target,
# 指向 libLLVM-22.dll。链接它就够了,不需要枚举组件 —— 枚举组件名在这种
# 配置下反而容易写错(LLVM 22 的组件名全小写,且 native/nativecodegen 是别名)。
find_package(LLVM REQUIRED CONFIG)
message(STATUS "找到 LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")

# LLVM_DEFINITIONS 是带空格的字符串,含 -D 前缀,要用 add_compile_options
# 而不是 target_compile_definitions(后者不接受 -D 前缀)。
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_compile_options(${LLVM_DEFINITIONS_LIST})

# ---------------------------------------------------------------------------
# 运行时库
# ---------------------------------------------------------------------------
# 必须是 SHARED(DLL),不能是静态库。原因:JIT 生成的代码要在运行时
# 解析 py_add 这类符号,而 Windows 上 MinGW 链接出的 exe 几乎不导出任何
# 符号,"搜索当前进程导出符号"这条路走不通。做成 DLL 后用
# EPCDynamicLibrarySearchGenerator::Load 按名字显式加载即可。
#
# 刻意不加 __declspec(dllexport):MinGW 对 -shared 库默认自动导出全部符号,
# 一旦出现第一个 dllexport,后续符号就不再自动导出,反而会漏掉一批。
add_library(pylite_runtime SHARED
  runtime/error.cpp
  runtime/arena.cpp
  runtime/arith.cpp
  runtime/string.cpp
  runtime/tuple.cpp
  runtime/io.cpp
)
target_include_directories(pylite_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# ---------------------------------------------------------------------------
# 前端(词法 / 语法 / AST)—— 不依赖 LLVM
# ---------------------------------------------------------------------------
# 刻意与 LLVM 完全解耦:前端可以用最朴素的方式构建和单元测试,
# 报错也只可能是语言层面的问题,不会和 JIT/代码生成的坑混在一起。
add_library(pylite_frontend STATIC
  frontend/lexer.cpp
  frontend/ast.cpp
  frontend/parser.cpp
)
target_include_directories(pylite_frontend PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/frontend>
)

add_executable(lex_dump tests/lex_dump.cpp)
target_link_libraries(lex_dump PRIVATE pylite_frontend)

add_executable(ast_dump tests/ast_dump.cpp)
target_link_libraries(ast_dump PRIVATE pylite_frontend)

add_executable(frontend_test tests/frontend_test.cpp)
target_link_libraries(frontend_test PRIVATE pylite_frontend)

# ---------------------------------------------------------------------------
# 代码生成(依赖 LLVM)
# ---------------------------------------------------------------------------
# irgen 与 aot 放同一个库里:两者都直接用 LLVM C++ API,且共享
# kTargetTriple / initializeCodeGenTargets 这些必须成对出现的约定。
add_library(pylite_irgen STATIC
  irgen/abi.cpp
  irgen/irgen.cpp
  aot/compiler.cpp
)
# 公开项目根目录与 include/:irgen 的源文件会 include "frontend/lexer.h"
# 和 "pylite/value.h" 这种路径
target_include_directories(pylite_irgen PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(pylite_irgen PUBLIC pylite_frontend LLVM)

add_executable(pylitec tools/pylitec.cpp)
target_link_libraries(pylitec PRIVATE pylite_irgen)

# ---------------------------------------------------------------------------
# Stage 0 验证
# ---------------------------------------------------------------------------
add_executable(abi_test tests/abi_test.cpp)
target_link_libraries(abi_test PRIVATE pylite_runtime)

add_executable(jit_spike tests/jit_spike.cpp)
target_include_directories(jit_spike PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(jit_spike PRIVATE LLVM pylite_runtime)

# ---------------------------------------------------------------------------
# 诊断工具:确认 LLVM 目标注册表状态(非测试,出问题时手动跑)
# ---------------------------------------------------------------------------
add_executable(target_probe tests/target_probe.cpp)
target_include_directories(target_probe PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(target_probe PRIVATE LLVM)

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME abi_test COMMAND abi_test)
add_test(NAME jit_spike COMMAND jit_spike)
add_test(NAME frontend_test COMMAND frontend_test)

# examples/ 下的每个示例都必须能被成功解析(退出码 0)。
# 这是最轻量的检查:源码进、AST 出,覆盖前端全链路。
foreach(example range_print fib containers)
  add_test(NAME parse_${example}
           COMMAND ast_dump ${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.pys)
endforeach()

# ---------------------------------------------------------------------------
# 端到端测试:.pys -> 目标文件 -> 与 C++ 链接 -> 运行 -> 比对 stdout
# ---------------------------------------------------------------------------
# 每个用例配一个最小的 C++ 驱动,它只做一件事:调用 pylitec 编译出来的入口。
# 这一层验证的正是项目的核心诉求 —— "在编译期集成进 C++"。
# 只做 AST 层面的检查是不够的:之前 print(a, b) 的 GEP 语义写错时,
# IR 验证通过、编译成功、链接成功,只有真正运行才暴露出来。
set(E2E_CASES
  arith_ops
  fib
  for_step
  if_else
  input_types
  logic
  loop_sum
  multiassign
  range_print
  unpack_call
  while_ctl
)

foreach(t ${E2E_CASES})
  set(pys ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/${t}.pys)
  set(exp ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/${t}.expected)
  set(inp ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/${t}.in)
  set(obj ${CMAKE_CURRENT_BINARY_DIR}/e2e/${t}.o)
  set(drv ${CMAKE_CURRENT_BINARY_DIR}/e2e/${t}_main.cpp)

  # 驱动源码:唯一的职责就是把编译产物调起来
  file(WRITE ${drv}
    "extern \"C\" void pylite_${t}_main();\n"
    "int main() { pylite_${t}_main(); return 0; }\n")

  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/e2e
    COMMAND pylitec ${pys} -o ${obj}
    DEPENDS pylitec ${pys}
    COMMENT "pylitec ${t}.pys")

  add_executable(e2e_${t} ${drv} ${obj})
  target_link_libraries(e2e_${t} PRIVATE pylite_runtime)

  # 有同名 .in 文件时,把它作为 stdin 喂给程序(测 input(...) 的用例)
  set(inputArg)
  if(EXISTS ${inp})
    list(APPEND inputArg -DINPUT=${inp})
  endif()

  add_test(NAME e2e_${t}
           COMMAND ${CMAKE_COMMAND}
                   -DEXE=$<TARGET_FILE:e2e_${t}>
                   -DEXPECTED=${exp}
                   ${inputArg}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_output.cmake)
endforeach()
