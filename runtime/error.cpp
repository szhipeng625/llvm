// 运行时错误处理。
//
// v1 没有异常、没有栈展开 —— IRGen 不会生成 landing pad,运行时报错直接
// 打印并 exit(1)。这是刻意的简化:异常会要求 JIT 生成的代码带 SEH 表,
// 而 ORC 的 MinGW 支持目前很不完整(见 docs/llvm-notes.md 的 R1)。
#include "pylite/value.h"

#include <cstdio>
#include <cstdlib>

extern "C" {

[[noreturn]] void py_runtime_error(const char *msg) {
  // 先冲 stdout:报错信息不能插在用户程序输出中间
  std::fflush(stdout);
  std::fprintf(stderr, "PyLite error: %s\n", msg);
  std::fflush(stderr);
  std::exit(1);
}

// 供 IRGen 在无法静态判定类型时使用
const char *py_tag_name(int32_t tag) {
  switch (tag) {
    case PY_NULL:  return "None";
    case PY_BOOL:  return "bool";
    case PY_INT:   return "int";
    case PY_FLOAT: return "float";
    case PY_STR:   return "str";
    case PY_LIST:  return "list";
    case PY_DICT:  return "dict";
    case PY_TUPLE: return "tuple";
    default:       return "<unknown>";
  }
}

}  // extern "C"
