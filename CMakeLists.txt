cmake_minimum_required(VERSION 3.20)
project(PyLite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Linux 上 ld 默认 --as-needed，会把 .o 之前出现的 .so 跳过，
# 导致 pylitec 产出的 .o 找不到运行时符号。强制 --no-as-needed。
if(UNIX AND NOT APPLE)
  add_link_options(-Wl,--no-as-needed)
endif()

# Linux 适配：可执行文件和 .so 都放到同一个 bin/ 下
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# LLVM
# ---------------------------------------------------------------------------
# Linux 上 LLVM 22 也是 shared 模式，LLVMConfig.cmake 导出 `LLVM` target
# 指向 libLLVM-22.so。链接它就够了。
find_package(LLVM REQUIRED CONFIG HINTS /usr/lib/llvm-22/lib/cmake/llvm)
message(STATUS "找到 LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")

# LLVM_DEFINITIONS 是带空格的字符串,含 -D 前缀,要用 add_compile_options
# 而不是 target_compile_definitions(后者不接受 -D 前缀)。
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_compile_options(${LLVM_DEFINITIONS_LIST})

# ---------------------------------------------------------------------------
# 运行时库
# ---------------------------------------------------------------------------
# Linux 上使用 SHARED(.so)，JIT 生成的代码通过 dlsym 解析运行时符号。
# GCC/Clang 对 -shared 默认导出全部符号，无需额外标注。
# ---------------------------------------------------------------------------
# 内置分布式项目 adviskv（作为子项目接入构建）
# ---------------------------------------------------------------------------
set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/adviskv/.adviskv_deps/vcpkg/installed" CACHE PATH "adviskv vcpkg installed dir")
list(PREPEND CMAKE_PREFIX_PATH "${VCPKG_INSTALLED_DIR}/x64-linux")
set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "Disable vcpkg manifest install" FORCE)
add_subdirectory(third_party/adviskv)

add_library(pylite_runtime SHARED
  runtime/error.cpp
  runtime/gc.cpp
  runtime/arith.cpp
  runtime/string.cpp
  runtime/tuple.cpp
  runtime/list.cpp
  runtime/dict.cpp
  runtime/index.cpp
  runtime/method.cpp
  runtime/io.cpp
  runtime/docker.cpp
  runtime/llm.cpp
  runtime/cuda.cpp
  runtime/fusion.cpp
  runtime/inference.cpp
  runtime/tokenizer.cpp
  runtime/server.cpp
  runtime/quantize.cpp
  runtime/engine.cpp
  runtime/concurrency.cpp
  runtime/object.cpp
  runtime/trace.cpp
  runtime/sandbox.cpp
  runtime/cgroup.cpp
  runtime/sandbox_api.cpp
  runtime/distributed_adapter.cpp
  runtime/cgroup_api.cpp
  runtime/sysmon.cpp
  runtime/sysmon_api.cpp
  runtime/raft.cpp
  runtime/raft_api.cpp
  runtime/cluster.cpp
  runtime/cluster_api.cpp
  runtime/kv_admin.cpp
  runtime/kv_admin_api.cpp
)
target_include_directories(pylite_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# Linux 上 GCC 默认隐藏符号，需要显式设置默认可见性以匹配 MinGW 行为
target_compile_options(pylite_runtime PRIVATE -fvisibility=default)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECCOMP REQUIRED libseccomp)
target_include_directories(pylite_runtime PRIVATE ${SECCOMP_INCLUDE_DIRS})
target_link_libraries(pylite_runtime PRIVATE pthread dl ${SECCOMP_LIBRARIES} adviskv_storage_lib adviskv_catalog_lib adviskv_topo_lib adviskv_sdk)

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

# 运行时语义的直接测试(不经过 IRGen)。放在这一层是为了让 e2e 失败时
# 能立刻排除"运行时算错"这一整类原因。
add_executable(runtime_test tests/runtime_test.cpp)
target_link_libraries(runtime_test PRIVATE pylite_runtime)

# GC 专项测试:守恒律、真的回收了、不误回收、环。
add_executable(gc_test tests/gc_test.cpp)
target_link_libraries(gc_test PRIVATE pylite_runtime)

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
add_test(NAME runtime_test COMMAND runtime_test)
add_test(NAME gc_test COMMAND gc_test)

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
  containers
  dicts
  fib
  for_step
  gc_stress
  if_else
  inplace_add
  input_types
  lists
  logic
  long_loop
  loop_sum
  multiassign
  range_print
  slices
  strings
  unpack_call
  while_ctl
)

# 手写 C++ 链接 pylitec 产物的集成测试。tests/linktest/ 一直都在,但之前只有
# 手工步骤;接进 CTest 之后,项目的核心诉求 —— "把 PyLite 编译进 C++ 程序" ——
# 每次构建都会被真正验证一次。
set(LINKTEST_OBJ ${CMAKE_CURRENT_BINARY_DIR}/linktest/arith.o)
add_custom_command(
  OUTPUT ${LINKTEST_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/linktest
  COMMAND pylitec ${CMAKE_CURRENT_SOURCE_DIR}/tests/linktest/arith.pys
          -o ${LINKTEST_OBJ}
  DEPENDS pylitec ${CMAKE_CURRENT_SOURCE_DIR}/tests/linktest/arith.pys
  COMMENT "pylitec linktest/arith.pys")

add_executable(linktest tests/linktest/main.cpp ${LINKTEST_OBJ})
target_link_libraries(linktest PRIVATE pylite_runtime)

add_test(NAME linktest
         COMMAND ${CMAKE_COMMAND}
                 -DEXE=$<TARGET_FILE:linktest>
                 -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/linktest/arith.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_output.cmake)

# ---------------------------------------------------------------------------
# 混合语言示例:C++ 宿主 + PyLite 业务逻辑
# ---------------------------------------------------------------------------
# examples/mixed/ 是给人看的一个完整例子(C++ 造数据、PyLite 算、C++ 收结果),
# 文档在同目录的 README.md。这里把它接进构建,保证它随时可编译可链接。
#
# 只断言"退出码为 0",不比对 stdout:它的输出是给人看的中文,
# 而 tests/compare_output.cmake 在 Windows 上只能比 ASCII。
set(MIXED_OBJ ${CMAKE_CURRENT_BINARY_DIR}/mixed/scores.o)
add_custom_command(
  OUTPUT ${MIXED_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/mixed
  COMMAND pylitec ${CMAKE_CURRENT_SOURCE_DIR}/examples/mixed/scores.pys
          -o ${MIXED_OBJ}
  DEPENDS pylitec ${CMAKE_CURRENT_SOURCE_DIR}/examples/mixed/scores.pys
  COMMENT "pylitec mixed/scores.pys")

add_executable(mixed_demo examples/mixed/main.cpp ${MIXED_OBJ})
target_link_libraries(mixed_demo PRIVATE pylite_runtime)

add_test(NAME mixed_demo COMMAND mixed_demo)

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

  # 同一份程序再跑一轮"每次分配都回收"。默认配置下这些用例只分配几 KB,
  # 离任何回收阈值都差着数量级 —— 也就是说 GC 的回收路径**根本没被走到**,
  # 全绿说明不了任何问题。加上这一轮之后,丢根与误回收会立刻暴露,
  # 而输出必须与默认配置逐字节相同(这本身就证明"回收不影响可观测行为")。
  add_test(NAME e2e_${t}_gc
           COMMAND ${CMAKE_COMMAND}
                   -DEXE=$<TARGET_FILE:e2e_${t}>
                   -DEXPECTED=${exp}
                   ${inputArg}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_output.cmake)
  set_tests_properties(e2e_${t}_gc PROPERTIES
                       ENVIRONMENT "PYLITE_GC_STRESS=1")
endforeach()
