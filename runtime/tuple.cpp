// 元组。
//
// 存在的意义主要是承载"多值":input(int, int) 的返回值、函数的多返回值,
// 以及 a, b = ... 的右侧。IRGen 对多目标赋值统一走 py_unpack。
//
// 按指针收发(参数是 PyValue*),避免聚合体跨 ABI 边界按值传递。
#include "pylite/runtime.h"

#include <cstdio>

// 与 value.h 里的前置声明 `struct PyTuple;` 对应,必须是全局作用域 ——
// 放进匿名命名空间会与那个声明构成歧义。定义只在本翻译单元可见。
struct PyTuple {
  int64_t len;
  PyValue items[1];  // 变长数组,实际分配 sizeof(int64_t) + len*sizeof(PyValue)
};

namespace {
PyTuple *asTuple(const PyValue *v) { return static_cast<PyTuple *>(v->as.ptr); }
}  // namespace

extern "C" PyValue py_tuple_new(PyValue *elems, int64_t n) {
  auto *t = static_cast<PyTuple *>(py_alloc(sizeof(int64_t) + sizeof(PyValue) * static_cast<size_t>(n)));
  t->len = n;
  for (int64_t i = 0; i < n; ++i) t->items[i] = elems[i];
  return py_ptr(PY_TUPLE, t);
}

extern "C" int64_t py_tuple_len(const PyValue *v) {
  if (v->tag != PY_TUPLE) {
    static thread_local char buf[128];
    std::snprintf(buf, sizeof(buf), "len() 不支持 '%s'", py_tag_name(v->tag));
    py_runtime_error(buf);
  }
  return asTuple(v)->len;
}

extern "C" PyValue py_tuple_get(const PyValue *v, int64_t i) {
  PyTuple *t = asTuple(v);
  if (i < 0 || i >= t->len) {
    py_runtime_error("元组下标越界");
  }
  return t->items[i];
}

// 把元组解包到 out 指向的 n 个连续槽位。
// 元素个数不匹配是常见错误(比如 a, b = 只有两个值的东西),这里给出
// 明确的报错而不是静默出错。
extern "C" void py_unpack(const PyValue *v, PyValue *out, int64_t n) {
  if (v->tag != PY_TUPLE) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "无法解包:'%s' 不是元组", py_tag_name(v->tag));
    py_runtime_error(buf);
  }
  PyTuple *t = asTuple(v);
  if (t->len != n) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "解包元素个数不匹配:左侧要 %lld 个,右侧有 %lld 个",
                  static_cast<long long>(n), static_cast<long long>(t->len));
    py_runtime_error(buf);
  }
  for (int64_t i = 0; i < n; ++i) out[i] = t->items[i];
}
