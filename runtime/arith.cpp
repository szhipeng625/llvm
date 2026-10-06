// 算术、比较与逻辑运算。
//
// 全部走 C ABI、收发 PyValue。数值类型提升规则与 Python 一致:
// int 与 float 混算时提升为 float。
//
// 注意:C++ 侧的签名是按值的 `PyValue f(PyValue, PyValue)`,而 IRGen 在 IR 里
// 把它声明成 `void @f(ptr sret(%PyValue), ptr, ptr)`。两者在机器层面是一致的 ——
// Win64 ABI 规定 16 字节聚合体按引用传递,返回值走隐藏 sret 指针。
// tests/jit_spike.cpp 与 tests/abi_test.cpp 就是为了钉死这个约定。
#include "pylite/runtime.h"

#include <cmath>
#include <cstdio>

namespace {

  bool isNumeric(int32_t t) { return t == PY_INT || t == PY_FLOAT || t == PY_BOOL; }

  bool wantFloat(PyValue a, PyValue b) {
    return a.tag == PY_FLOAT || b.tag == PY_FLOAT;
  }

  double toDouble(PyValue v) {
    return v.tag == PY_FLOAT ? v.as.f : static_cast<double>(v.as.i);
  }

  [[noreturn]] void badOperands(const char *op, PyValue a, PyValue b) {
    static thread_local char buf[192];
    std::snprintf(buf, sizeof(buf), "不支持的操作数类型: %s 用于 '%s' 和 '%s'",
                  op, py_tag_name(a.tag), py_tag_name(b.tag));
    py_runtime_error(buf);
  }

  void needNumeric(const char *op, PyValue a, PyValue b) {
    if (!isNumeric(a.tag) || !isNumeric(b.tag)) badOperands(op, a, b);
  }

  // 比较前先检查可比性。数值之间可比;类型不同一律不可比(与 Python 一致,
  // 除了 == / != 会返回 False / True 而不是报错)。
  void needComparable(const char *op, PyValue a, PyValue b) {
    if (!isNumeric(a.tag) || !isNumeric(b.tag)) badOperands(op, a, b);
  }

  PyValue boolOf(bool b) { return py_bool(b); }

}  // namespace

// ---------------------------------------------------------------------------
// 算术
// ---------------------------------------------------------------------------

extern "C" PyValue py_add(PyValue a, PyValue b) {
  needNumeric("+", a, b);
  if (wantFloat(a, b)) return py_float(toDouble(a) + toDouble(b));
  return py_int(a.as.i + b.as.i);
}

extern "C" PyValue py_sub(PyValue a, PyValue b) {
  needNumeric("-", a, b);
  if (wantFloat(a, b)) return py_float(toDouble(a) - toDouble(b));
  return py_int(a.as.i - b.as.i);
}

extern "C" PyValue py_mul(PyValue a, PyValue b) {
  needNumeric("*", a, b);
  if (wantFloat(a, b)) return py_float(toDouble(a) * toDouble(b));
  return py_int(a.as.i * b.as.i);
}

// Python 的 / 是真除法,结果总是 float(7 / 2 == 3.5)
extern "C" PyValue py_div(PyValue a, PyValue b) {
  needNumeric("/", a, b);
  const double d = toDouble(b);
  if (d == 0.0) py_runtime_error("除以零");
  return py_float(toDouble(a) / d);
}

// // 是向下取整除法:整数相除结果仍是整数,浮点相除结果是浮点数
extern "C" PyValue py_floordiv(PyValue a, PyValue b) {
  needNumeric("//", a, b);
  if (wantFloat(a, b)) {
    const double d = toDouble(b);
    if (d == 0.0) py_runtime_error("除以零");
    return py_float(std::floor(toDouble(a) / d));
  }
  if (b.as.i == 0) py_runtime_error("整数除以零");
  // C++ 的 / 向零取整,Python 的 // 向下取整,负数时结果不同:
  //   -7 // 2 == -4(Python)而 C++ 给 -3。这里按 Python 语义修正。
  int64_t q = a.as.i / b.as.i;
  const int64_t r = a.as.i % b.as.i;
  if (r != 0 && ((r < 0) != (b.as.i < 0))) --q;
  return py_int(q);
}

// 取模。Python 的结果符号跟随除数(与 C++ 的 % 不同)。
extern "C" PyValue py_mod(PyValue a, PyValue b) {
  needNumeric("%", a, b);
  if (wantFloat(a, b)) {
    const double d = toDouble(b);
    if (d == 0.0) py_runtime_error("取模的除数为零");
    double r = std::fmod(toDouble(a), d);
    if (r != 0.0 && ((r < 0.0) != (d < 0.0))) r += d;
    return py_float(r);
  }
  if (b.as.i == 0) py_runtime_error("整数取模的除数为零");
  int64_t r = a.as.i % b.as.i;
  if (r != 0 && ((r < 0) != (b.as.i < 0))) r += b.as.i;
  return py_int(r);
}

// 幂。整数底数与非负整数指数时结果保持整数,否则退化为浮点。
// 这里用朴素的累乘实现;大指数会慢,但对 v1 的用途足够了。
extern "C" PyValue py_pow(PyValue a, PyValue b) {
  needNumeric("**", a, b);
  if (!wantFloat(a, b) && b.as.i >= 0) {
    int64_t base = a.as.i;
    int64_t exp = b.as.i;
    int64_t result = 1;
    while (exp > 0) {
      if (exp & 1) result *= base;
      base *= base;
      exp >>= 1;
    }
    return py_int(result);
  }
  return py_float(std::pow(toDouble(a), toDouble(b)));
}

// 取整数负载。range 的边界用它 —— 顺便在这里做类型检查,
// 让 `range("a")` 之类能在运行时给出像样的报错,而不是把指针当数字用。
extern "C" int64_t py_to_int(PyValue v) {
  if (v.tag == PY_INT || v.tag == PY_BOOL) return v.as.i;
  static thread_local char buf[128];
  std::snprintf(buf, sizeof(buf), "需要整数,实际得到 '%s'", py_tag_name(v.tag));
  py_runtime_error(buf);
}

extern "C" PyValue py_neg(PyValue a) {
  if (!isNumeric(a.tag)) {
    static thread_local char buf[128];
    std::snprintf(buf, sizeof(buf), "不支持的操作数类型: 一元 '-' 用于 '%s'",
                  py_tag_name(a.tag));
    py_runtime_error(buf);
  }
  if (a.tag == PY_FLOAT) return py_float(-a.as.f);
  return py_int(-a.as.i);
}

// ---------------------------------------------------------------------------
// 比较
//
// 全部返回 bool 值的 PyValue。类型不匹配时 ==/!= 返回 False/True,
// 其余比较报错 —— 与 Python 一致。
// ---------------------------------------------------------------------------

extern "C" PyValue py_eq(PyValue a, PyValue b) {
  if (a.tag != b.tag) return boolOf(false);
  switch (a.tag) {
    case PY_NULL:  return boolOf(true);
    case PY_FLOAT: return boolOf(a.as.f == b.as.f);
    default:       return boolOf(a.as.i == b.as.i);
  }
}

extern "C" PyValue py_ne(PyValue a, PyValue b) {
  PyValue e = py_eq(a, b);
  return boolOf(e.as.i == 0);
}

extern "C" PyValue py_lt(PyValue a, PyValue b) {
  needComparable("<", a, b);
  return boolOf(wantFloat(a, b) ? toDouble(a) < toDouble(b) : a.as.i < b.as.i);
}

extern "C" PyValue py_le(PyValue a, PyValue b) {
  needComparable("<=", a, b);
  return boolOf(wantFloat(a, b) ? toDouble(a) <= toDouble(b) : a.as.i <= b.as.i);
}

extern "C" PyValue py_gt(PyValue a, PyValue b) {
  needComparable(">", a, b);
  return boolOf(wantFloat(a, b) ? toDouble(a) > toDouble(b) : a.as.i > b.as.i);
}

extern "C" PyValue py_ge(PyValue a, PyValue b) {
  needComparable(">=", a, b);
  return boolOf(wantFloat(a, b) ? toDouble(a) >= toDouble(b) : a.as.i >= b.as.i);
}

// ---------------------------------------------------------------------------
// 逻辑
//
// and / or 的短路由 IRGen 用 PHI 实现(且要返回操作数本身,不是布尔值),
// 所以这里只有 not。
// ---------------------------------------------------------------------------

extern "C" PyValue py_not(PyValue a) { return boolOf(py_truthy(a) == 0); }

// 返回 i32 而非 i1:见 value.h 的说明,GCC 与 Clang 对 bool 返回值的
// ABI 标注不一致,跨 DLL 边界不可靠。
extern "C" int32_t py_truthy(PyValue v) {
  switch (v.tag) {
    case PY_NULL:  return 0;
    case PY_BOOL:
    case PY_INT:   return v.as.i != 0;
    case PY_FLOAT: return v.as.f != 0.0;
    // TODO(Stage 7):字符串实现后,空串应为假。PY_STR/PY_LIST/PY_DICT 目前
    // 一律按"非空指针即真"处理,容器落地时需要改成按元素个数判断。
    default:       return v.as.ptr != nullptr;
  }
}
