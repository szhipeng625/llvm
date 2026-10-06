// Stage 0 门槛之一:证明 C++(GCC)与运行时 DLL 之间的 PyValue ABI 自洽。
//
// 这个测试不做任何 LLVM 的事 —— 它只回答一个问题:
// "16 字节的 PyValue 按值跨 DLL 边界传递,能不能正常工作?"
// 如果这个都过不了,后面 LLVM 生成的代码更不可能过。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void check(bool cond, const char *what) {
  if (cond) {
    std::printf("  [ok]   %s\n", what);
  } else {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

}  // namespace

int main() {
  std::printf("=== PyValue ABI test ===\n");

  // 布局契约
  check(sizeof(PyValue) == 16, "sizeof(PyValue) == 16");
  check(alignof(PyValue) == 8, "alignof(PyValue) == 8");
  check(offsetof(PyValue, as) == 8, "payload 位于偏移 8");

  // tag 值必须与 IRGen 里硬编码的常量一致
  check(PY_NULL == 0 && PY_BOOL == 1 && PY_INT == 2 && PY_FLOAT == 3 &&
            PY_STR == 4 && PY_LIST == 5 && PY_DICT == 6 && PY_TUPLE == 7,
        "PyTag 取值与 IRGen 常量一致");

  // 构造 + 取值往返
  check(py_as_int(py_int(42)) == 42, "py_int 往返");
  check(py_as_float(py_float(2.5)) == 2.5, "py_float 往返");
  check(py_as_bool(py_bool(true)), "py_bool(true) 往返");
  check(!py_as_bool(py_bool(false)), "py_bool(false) 往返");

  // 跨 DLL 调用的按值传参/返回 —— 这是本测试的真正目的
  PyValue s = py_add(py_int(1), py_int(2));
  check(s.tag == PY_INT && s.as.i == 3, "py_add(int 1, int 2) == 3 (跨 DLL 按值传递)");

  PyValue f = py_add(py_float(1.5), py_float(2.25));
  check(f.tag == PY_FLOAT && f.as.f == 3.75, "py_add(1.5, 2.25) == 3.75");

  PyValue mixed = py_add(py_int(1), py_float(0.5));
  check(mixed.tag == PY_FLOAT && mixed.as.f == 1.5, "int+float 提升为 float");

  PyValue neg = py_sub(py_int(10), py_int(4));
  check(neg.as.i == 6, "py_sub(10, 4) == 6");

  PyValue prod = py_mul(py_int(6), py_int(7));
  check(prod.as.i == 42, "py_mul(6, 7) == 42");

  // 谓词返回 i32
  check(py_truthy(py_int(1)) == 1, "py_truthy(1) == 1 (返回 i32)");
  check(py_truthy(py_int(0)) == 0, "py_truthy(0) == 0");
  check(py_truthy(py_none()) == 0, "py_truthy(None) == 0");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
