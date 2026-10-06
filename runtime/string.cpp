// 字符串。
//
// v1 只做最小集合:构造、取长度、取值、拼接。方法(upper/split/...)与
// 切片留到后续。字符串不可变 —— 拼接总是新分配。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstring>

// 与 value.h 里的前置声明 `struct PyStr;` 对应,所以必须是全局作用域 ——
// 放进匿名命名空间会与那个声明构成歧义(编译期就报 ambiguous)。
// 定义只在本翻译单元内可见,外部一律通过 py_str_* 函数访问。
struct PyStr {
  int64_t len;
  char data[1];  // 变长,末尾保证有 '\0'
};

extern "C" PyValue py_str_new(const char *bytes, int64_t len) {
  auto *s = static_cast<PyStr *>(py_alloc(sizeof(int64_t) + static_cast<size_t>(len) + 1));
  s->len = len;
  if (len > 0) std::memcpy(s->data, bytes, static_cast<size_t>(len));
  s->data[len] = '\0';
  return py_ptr(PY_STR, s);
}

extern "C" int64_t py_str_size(const PyValue *v) {
  if (v->tag != PY_STR) py_runtime_error("需要一个字符串");
  return reinterpret_cast<PyStr *>(v->as.ptr)->len;
}

extern "C" const char *py_str_data(const PyValue *v) {
  if (v->tag != PY_STR) py_runtime_error("需要一个字符串");
  return reinterpret_cast<PyStr *>(v->as.ptr)->data;
}

extern "C" PyValue py_str_concat(const PyValue *a, const PyValue *b) {
  if (a->tag != PY_STR || b->tag != PY_STR) {
    py_runtime_error("只能拼接两个字符串");
  }
  auto *sa = reinterpret_cast<PyStr *>(a->as.ptr);
  auto *sb = reinterpret_cast<PyStr *>(b->as.ptr);
  auto *r = static_cast<PyStr *>(
      py_alloc(sizeof(int64_t) + static_cast<size_t>(sa->len + sb->len) + 1));
  r->len = sa->len + sb->len;
  std::memcpy(r->data, sa->data, static_cast<size_t>(sa->len));
  std::memcpy(r->data + sa->len, sb->data, static_cast<size_t>(sb->len));
  r->data[r->len] = '\0';
  return py_ptr(PY_STR, r);
}
