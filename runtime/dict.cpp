// 字典。
//
// **插入有序 + 线性扫描**,刻意不用哈希表:
//
//   * 线性扫描保住了 Python 3.7 起可观测的插入序 —— 遍历 dict 按插入顺序、
//     `==` 比较与 repr 的输出稳定,这些行为不依赖哈希实现也不需要额外维护
//     一条顺序链。
//   * 哈希表要为每种键类型(int/float/str/bool/tuple)写一份 hash,还得处理
//     冲突与再散列;而本项目的用途下字典都不大。
//
// 代价是查找 O(n),README 与 docs/language.md 里都写明了。
//
// 键的相等判定复用 py_eq —— 单一事实来源。特别注意 py_eq 对字符串必须按
// **值**比较(见 arith.cpp),否则这里会退化成按指针认键,`d["a"]` 永远查不到。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstring>

// 与 value.h 里的前置声明对应,必须是全局作用域。
struct PyDict {
  int64_t len;
  int64_t cap;
  PyValue *keys;
  PyValue *vals;
};

namespace {

PyDict *asDict(const PyValue *v) { return static_cast<PyDict *>(v->as.ptr); }

void needDict(const PyValue *v, const char *what) {
  if (v->tag != PY_DICT) {
    static thread_local char buf[160];
    std::snprintf(buf, sizeof(buf), "%s 需要字典,实际得到 '%s'", what,
                  py_tag_name(v->tag));
    py_runtime_error(buf);
  }
}

void reserve(PyDict *d, int64_t need) {
  if (need <= d->cap) return;
  int64_t newCap = d->cap ? d->cap : 8;
  while (newCap < need) newCap *= 2;

  // ⚠️ 显式根:`keys` 必须在 `vals` 的分配期间活下来 —— 那两次分配之间它只被这个
  // C++ 局部持有,没有任何容器指得到它(理由同 list.cpp 的 newList)。
  PyValue *keys = nullptr;
  py_gc_root_push(reinterpret_cast<void **>(&keys));
  keys = static_cast<PyValue *>(py_gc_alloc_values(
      static_cast<size_t>(newCap) * sizeof(PyValue), 0));

  auto *vals = static_cast<PyValue *>(py_gc_alloc_values(
      static_cast<size_t>(newCap) * sizeof(PyValue), 0));
  py_gc_root_pop(1);
  if (d->len > 0) {
    std::memcpy(keys, d->keys, static_cast<size_t>(d->len) * sizeof(PyValue));
    std::memcpy(vals, d->vals, static_cast<size_t>(d->len) * sizeof(PyValue));
  }
  d->keys = keys;
  d->vals = vals;
  d->cap = newCap;
}

PyDict *newDict(int64_t cap) {
  // ⚠️ 显式根:`d` 跨过 reserve 里的两次分配(理由同 list.cpp 的 newList)
  PyDict *d = nullptr;
  py_gc_root_push(reinterpret_cast<void **>(&d));

  // SCAN_WORDS:PyDict 的后两个字是 keys / vals 指针
  d = static_cast<PyDict *>(py_gc_alloc(sizeof(PyDict), PY_GC_SCAN_WORDS));
  d->len = 0;
  d->cap = 0;
  d->keys = nullptr;
  d->vals = nullptr;
  reserve(d, cap);

  py_gc_root_pop(1);
  return d;
}

// 按键查找,返回下标;找不到返回 -1
int64_t findIndex(PyDict *d, const PyValue *key) {
  for (int64_t i = 0; i < d->len; ++i) {
    if (py_eq(d->keys[i], *key).as.i != 0) return i;
  }
  return -1;
}

void needIndex(int64_t i, int64_t len) {
  if (i < 0 || i >= len) {
    static thread_local char buf[128];
    std::snprintf(buf, sizeof(buf), "字典下标越界: %lld(长度 %lld)",
                  static_cast<long long>(i), static_cast<long long>(len));
    py_runtime_error(buf);
  }
}

}  // namespace

extern "C" PyValue py_dict_new(PyValue *keys, PyValue *vals, int64_t n) {
  // ⚠️ 显式根:循环里每一次 reserve 都可能触发回收,而 `d` 只被这个局部持有
  PyDict *d = nullptr;
  py_gc_root_push(reinterpret_cast<void **>(&d));

  d = newDict(n);
  // 字面量里出现重复键时:值取**后**到的,位置取**先**到的 —— 与 Python 一致
  // (`{"a": 1, "b": 2, "a": 3}` 得到 `{'a': 3, 'b': 2}`,'a' 仍在首位)。
  for (int64_t i = 0; i < n; ++i) {
    const int64_t at = findIndex(d, &keys[i]);
    if (at >= 0) {
      d->vals[at] = vals[i];
    } else {
      reserve(d, d->len + 1);
      d->keys[d->len] = keys[i];
      d->vals[d->len] = vals[i];
      ++d->len;
    }
  }

  py_gc_root_pop(1);
  return py_ptr(PY_DICT, d);
}

extern "C" int64_t py_dict_len(const PyValue *v) {
  needDict(v, "len()");
  return asDict(v)->len;
}

extern "C" int32_t py_dict_has(const PyValue *v, const PyValue *key) {
  needDict(v, "'in'");
  return findIndex(asDict(v), key) >= 0 ? 1 : 0;
}

extern "C" PyValue py_dict_get(const PyValue *v, const PyValue *key) {
  needDict(v, "下标访问");
  PyDict *d = asDict(v);
  const int64_t at = findIndex(d, key);
  if (at < 0) {
    // 用 repr 显示键:字符串键能带出引号,数字键与字符串键不会被混为一谈
    PyValue shown = py_repr(key);
    static thread_local char buf[256];
    std::snprintf(buf, sizeof(buf), "字典里没有键 %.*s",
                  static_cast<int>(py_str_size(&shown)), py_str_data(&shown));
    py_runtime_error(buf);
  }
  return d->vals[at];
}

extern "C" PyValue py_dict_get_default(const PyValue *v, const PyValue *key,
                                       const PyValue *def) {
  needDict(v, "get()");
  PyDict *d = asDict(v);
  const int64_t at = findIndex(d, key);
  return at < 0 ? *def : d->vals[at];
}

extern "C" void py_dict_set(const PyValue *v, const PyValue *key,
                            const PyValue *val) {
  needDict(v, "下标赋值");
  PyDict *d = asDict(v);
  const int64_t at = findIndex(d, key);
  if (at >= 0) {
    d->vals[at] = *val;
    return;
  }
  reserve(d, d->len + 1);
  d->keys[d->len] = *key;
  d->vals[d->len] = *val;
  ++d->len;
}

extern "C" PyValue py_dict_key_at(const PyValue *v, int64_t i) {
  needDict(v, "遍历");
  PyDict *d = asDict(v);
  needIndex(i, d->len);
  return d->keys[i];
}

extern "C" PyValue py_dict_val_at(const PyValue *v, int64_t i) {
  needDict(v, "遍历");
  PyDict *d = asDict(v);
  needIndex(i, d->len);
  return d->vals[i];
}

extern "C" PyValue py_dict_pop(const PyValue *v, const PyValue *key) {
  needDict(v, "pop()");
  PyDict *d = asDict(v);
  const int64_t at = findIndex(d, key);
  if (at < 0) py_runtime_error("pop(): 字典里没有这个键");
  PyValue r = d->vals[at];
  // 后面的元素整体前移,保持插入序
  std::memmove(d->keys + at, d->keys + at + 1,
               static_cast<size_t>(d->len - at - 1) * sizeof(PyValue));
  std::memmove(d->vals + at, d->vals + at + 1,
               static_cast<size_t>(d->len - at - 1) * sizeof(PyValue));
  --d->len;
  return r;
}

extern "C" void py_dict_clear(const PyValue *v) {
  needDict(v, "clear()");
  asDict(v)->len = 0;
}
