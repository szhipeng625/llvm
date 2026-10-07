// 方法调用:obj.method(args) 统一走 py_call_method。
//
// 为什么把方法表放在运行时、按名字字符串分派,而不是在 IRGen 里按静态类型
// 解析成直接调用:
//
//   * 本项目的类型注解**不做检查**(docs/language.md),变量没有静态类型,
//     `x.upper()` 里的 x 到编译期根本不知道是什么。Python 自己也是运行时查找。
//   * 代价是拼错方法名变成**运行时**错误而不是编译期错误。这一点在
//     docs/language.md 里写明了,不是遗漏。
//
// 三个 tag 各有一张表,用 string_view 比名字。方法集合刻意只取常用子集,
// 文档里列了清单;表里没有的一律给出"XX 没有方法 'yy'"的明确报错。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

std::string_view nm(const char *p, int64_t n) {
  return std::string_view(p, static_cast<size_t>(n));
}

bool isSpace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

[[noreturn]] void noSuchMethod(const PyValue *obj, std::string_view name) {
  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%s' 没有方法 '%.*s'", py_tag_name(obj->tag),
                static_cast<int>(name.size()), name.data());
  py_runtime_error(buf);
}

// 参数个数检查。min == max 时只报一个数字,别写成 "需要 1..1 个"。
void needArgs(std::string_view method, int64_t got, int64_t min, int64_t max) {
  if (got >= min && got <= max) return;
  static thread_local char buf[192];
  if (min == max) {
    std::snprintf(buf, sizeof(buf), "'%.*s' 需要 %lld 个参数,给了 %lld 个",
                  static_cast<int>(method.size()), method.data(),
                  static_cast<long long>(min), static_cast<long long>(got));
  } else {
    std::snprintf(buf, sizeof(buf),
                  "'%.*s' 需要 %lld 到 %lld 个参数,给了 %lld 个",
                  static_cast<int>(method.size()), method.data(),
                  static_cast<long long>(min), static_cast<long long>(max),
                  static_cast<long long>(got));
  }
  py_runtime_error(buf);
}

void needStrArg(std::string_view method, const PyValue &v, const char *what) {
  if (v.tag == PY_STR) return;
  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%.*s' 的%s必须是字符串,实际得到 '%s'",
                static_cast<int>(method.size()), method.data(), what,
                py_tag_name(v.tag));
  py_runtime_error(buf);
}

void needListArg(std::string_view method, const PyValue &v, const char *what) {
  if (v.tag == PY_LIST) return;
  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%.*s' 的%s必须是列表,实际得到 '%s'",
                static_cast<int>(method.size()), method.data(), what,
                py_tag_name(v.tag));
  py_runtime_error(buf);
}

void needDictArg(std::string_view method, const PyValue &v) {
  if (v.tag == PY_DICT) return;
  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%.*s' 的参数必须是字典,实际得到 '%s'",
                static_cast<int>(method.size()), method.data(),
                py_tag_name(v.tag));
  py_runtime_error(buf);
}

// ---------------------------------------------------------------------------
// 字符串方法
// ---------------------------------------------------------------------------

PyValue strUpper(const PyValue &s) {
  const int64_t n = py_str_size(&s);
  const char *data = py_str_data(&s);
  // ⚠️ 显式根:buf 要在随后的 py_str_new 分配期间活下来
  // (它只被这个局部持有,没有任何容器指得到它)
  char *buf = nullptr;
  py_gc_root_push(reinterpret_cast<void **>(&buf));
  buf = static_cast<char *>(
      py_gc_alloc(static_cast<size_t>(n) + 1, PY_GC_SCAN_NONE));
  for (int64_t i = 0; i < n; ++i) {
    const char c = data[i];
    buf[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  // 先把结果串造出来(这一步可能触发回收,buf 仍被根保护),再退根。
  // 反过来先退根的话,根就白加了。
  PyValue out = py_str_new(buf, n);
  py_gc_root_pop(1);
  return out;
}

PyValue strLower(const PyValue &s) {
  const int64_t n = py_str_size(&s);
  const char *data = py_str_data(&s);
  // ⚠️ 显式根:buf 要在随后的 py_str_new 分配期间活下来
  // (它只被这个局部持有,没有任何容器指得到它)
  char *buf = nullptr;
  py_gc_root_push(reinterpret_cast<void **>(&buf));
  buf = static_cast<char *>(
      py_gc_alloc(static_cast<size_t>(n) + 1, PY_GC_SCAN_NONE));
  for (int64_t i = 0; i < n; ++i) {
    const char c = data[i];
    buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  // 先把结果串造出来(这一步可能触发回收,buf 仍被根保护),再退根。
  // 反过来先退根的话,根就白加了。
  PyValue out = py_str_new(buf, n);
  py_gc_root_pop(1);
  return out;
}

// strip() 去空白,strip(chars) 去掉两端出现在 chars 里的任意字符
PyValue strStrip(const PyValue &s, const PyValue *chars) {
  const int64_t n = py_str_size(&s);
  const char *data = py_str_data(&s);

  const char *set = nullptr;
  int64_t setLen = 0;
  if (chars) {
    set = py_str_data(chars);
    setLen = py_str_size(chars);
  }

  auto inSet = [&](char c) {
    if (!set) return isSpace(static_cast<unsigned char>(c));
    for (int64_t i = 0; i < setLen; ++i) {
      if (set[i] == c) return true;
    }
    return false;
  };

  int64_t lo = 0, hi = n;
  while (lo < hi && inSet(data[lo])) ++lo;
  while (hi > lo && inSet(data[hi - 1])) --hi;
  return py_str_new(data + lo, hi - lo);
}

// 在 sep 的每次出现处切开。sep 为空串是 Python 里的 ValueError,这里直接报错;
// 空串作为分隔符在我们这套 C 实现的循环里会死循环,不拦下来很危险。
PyValue strSplit(const PyValue &s, const PyValue *sep) {
  const char *data = py_str_data(&s);
  const int64_t n = py_str_size(&s);

  PyValue out = py_list_new(nullptr, 0);

  if (!sep) {
    // 不给分隔符:按空白切,并丢掉空片段(与 Python 一致)
    int64_t i = 0;
    while (i < n) {
      while (i < n && isSpace(static_cast<unsigned char>(data[i]))) ++i;
      if (i >= n) break;
      const int64_t start = i;
      while (i < n && !isSpace(static_cast<unsigned char>(data[i]))) ++i;
      PyValue piece = py_str_new(data + start, i - start);
      py_list_append(&out, &piece);
    }
    return out;
  }

  const char *sd = py_str_data(sep);
  const int64_t sn = py_str_size(sep);
  if (sn == 0) py_runtime_error("split() 的分隔符不能是空字符串");

  int64_t start = 0;
  for (int64_t i = 0; i + sn <= n;) {
    if (std::memcmp(data + i, sd, static_cast<size_t>(sn)) == 0) {
      PyValue piece = py_str_new(data + start, i - start);
      py_list_append(&out, &piece);
      i += sn;
      start = i;
    } else {
      ++i;
    }
  }
  PyValue tail = py_str_new(data + start, n - start);
  py_list_append(&out, &tail);
  return out;
}

PyValue strJoin(const PyValue &sep, const PyValue &items) {
  needListArg("join", items, "参数");
  const int64_t n = py_list_len(&items);

  // 先量一遍总长,一次分配,避免逐个 concat 造成 O(n²) 的拷贝
  int64_t total = 0;
  for (int64_t i = 0; i < n; ++i) {
    PyValue e = py_list_get(&items, i);
    needStrArg("join", e, "列表元素");
    total += py_str_size(&e);
  }
  const int64_t sepLen = py_str_size(&sep);
  if (n > 1) total += sepLen * (n - 1);

  // ⚠️ 显式根:buf 要在随后的 py_str_new 分配期间活下来
  // (它只被这个局部持有,没有任何容器指得到它)
  char *buf = nullptr;
  py_gc_root_push(reinterpret_cast<void **>(&buf));
  buf = static_cast<char *>(
      py_gc_alloc(static_cast<size_t>(total) + 1, PY_GC_SCAN_NONE));
  int64_t at = 0;
  for (int64_t i = 0; i < n; ++i) {
    if (i) {
      std::memcpy(buf + at, py_str_data(&sep), static_cast<size_t>(sepLen));
      at += sepLen;
    }
    PyValue e = py_list_get(&items, i);
    const int64_t en = py_str_size(&e);
    std::memcpy(buf + at, py_str_data(&e), static_cast<size_t>(en));
    at += en;
  }
  // 先把结果串造出来(这一步可能触发回收,buf 仍被根保护),再退根。
  // 反过来先退根的话,根就白加了。
  PyValue out = py_str_new(buf, total);
  py_gc_root_pop(1);
  return out;
}

PyValue strReplace(const PyValue &s, const PyValue &oldS, const PyValue &newS) {
  const char *data = py_str_data(&s);
  const int64_t n = py_str_size(&s);
  const char *od = py_str_data(&oldS);
  const int64_t on = py_str_size(&oldS);
  const char *nd = py_str_data(&newS);
  const int64_t nn = py_str_size(&newS);

  // Python 里 "abc".replace("", "x") 是在每个字符前后插入,规则很绕。
  // 这里把空旧串当成"没什么可替换"直接返回原串 —— 明确、可预期,
  // 也在 docs/language.md 里注明了与 Python 的差异。
  if (on == 0) return py_str_new(data, n);

  const int64_t cap = n + (nn > on ? (nn - on) * n : 0) + 1;
  // ⚠️ 显式根:buf 要在随后的 py_str_new 分配期间活下来
  // (它只被这个局部持有,没有任何容器指得到它)
  char *buf = nullptr;
  py_gc_root_push(reinterpret_cast<void **>(&buf));
  buf = static_cast<char *>(
      py_gc_alloc(static_cast<size_t>(cap) + 1, PY_GC_SCAN_NONE));
  int64_t at = 0;
  for (int64_t i = 0; i < n;) {
    if (i + on <= n && std::memcmp(data + i, od, static_cast<size_t>(on)) == 0) {
      std::memcpy(buf + at, nd, static_cast<size_t>(nn));
      at += nn;
      i += on;
    } else {
      buf[at++] = data[i++];
    }
  }
  // 先把结果串造出来(这一步可能触发回收,buf 仍被根保护),再退根。
  // 反过来先退根的话,根就白加了。
  PyValue out = py_str_new(buf, at);
  py_gc_root_pop(1);
  return out;
}

PyValue strCount(const PyValue &s, const PyValue &sub) {
  const char *data = py_str_data(&s);
  const int64_t n = py_str_size(&s);
  const char *sd = py_str_data(&sub);
  const int64_t sn = py_str_size(&sub);
  if (sn == 0) py_runtime_error("count(): 子串不能为空");

  int64_t cnt = 0;
  for (int64_t i = 0; i + sn <= n;) {
    if (std::memcmp(data + i, sd, static_cast<size_t>(sn)) == 0) {
      ++cnt;
      i += sn;  // 不重叠地往后走,与 Python 一致
    } else {
      ++i;
    }
  }
  return py_int(cnt);
}

// ---------------------------------------------------------------------------
// 列表方法
// ---------------------------------------------------------------------------

PyValue listPop(const PyValue &l, const PyValue *idx) {
  PyValue none = py_none();
  return py_list_pop(&l, idx ? idx : &none);
}

PyValue dictGet(const PyValue &d, const PyValue &key, const PyValue *def) {
  if (def) return py_dict_get_default(&d, &key, def);
  PyValue none = py_none();
  return py_dict_get_default(&d, &key, &none);
}

// keys()/values()/items() 都由"按下标取第 i 项"拼出来
PyValue dictProjection(const PyValue &d, bool wantKeys, bool wantVals) {
  const int64_t n = py_dict_len(&d);
  // 元素数组里装的是键/值/新元组 —— 标成 SCAN_NONE 会把它们全放死。
  // 这里是 method.cpp 里唯一需要扫描的缓冲,其余 4 处 char 缓冲都是 SCAN_NONE。
  //
  // ⚠️ 显式根:`items` 里已经填好的那些元组/字符串,在下一轮 py_tuple_new
  // 触发回收时只被这个局部数组引用着 —— 不登记就会被收掉。
  PyValue *items = nullptr;
  py_gc_root_push(reinterpret_cast<void **>(&items));
  items = n > 0 ? static_cast<PyValue *>(
                      py_gc_alloc_values(static_cast<size_t>(n) * sizeof(PyValue), 0))
                : nullptr;

  for (int64_t i = 0; i < n; ++i) {
    if (wantKeys && wantVals) {
      PyValue pair[2] = {py_dict_key_at(&d, i), py_dict_val_at(&d, i)};
      items[i] = py_tuple_new(pair, 2);
    } else if (wantKeys) {
      items[i] = py_dict_key_at(&d, i);
    } else {
      items[i] = py_dict_val_at(&d, i);
    }
  }

  PyValue out = py_list_new(items, n);   // 此时元素已经在结果列表里了
  py_gc_root_pop(1);
  return out;
}

}  // namespace

extern "C" PyValue py_call_method(const PyValue *obj, const char *name,
                                  int64_t nameLen, PyValue *args, int64_t nargs) {
  const std::string_view m = nm(name, nameLen);

  // 便于书写:下标访问比 args[0] 清楚
  auto arg = [&](int64_t i) -> const PyValue & { return args[i]; };

  // ------------------------------------------------------------------ str
  if (obj->tag == PY_STR) {
    if (m == "upper") {
      needArgs(m, nargs, 0, 0);
      return strUpper(*obj);
    }
    if (m == "lower") {
      needArgs(m, nargs, 0, 0);
      return strLower(*obj);
    }
    if (m == "strip") {
      needArgs(m, nargs, 0, 1);
      if (nargs == 1) needStrArg(m, arg(0), "参数");
      return strStrip(*obj, nargs == 1 ? &arg(0) : nullptr);
    }
    if (m == "split") {
      needArgs(m, nargs, 0, 1);
      if (nargs == 1) needStrArg(m, arg(0), "分隔符");
      return strSplit(*obj, nargs == 1 ? &arg(0) : nullptr);
    }
    if (m == "join") {
      needArgs(m, nargs, 1, 1);
      return strJoin(*obj, arg(0));
    }
    if (m == "startswith") {
      needArgs(m, nargs, 1, 1);
      needStrArg(m, arg(0), "参数");
      return py_bool(py_str_starts_with(obj, &arg(0)) != 0);
    }
    if (m == "endswith") {
      needArgs(m, nargs, 1, 1);
      needStrArg(m, arg(0), "参数");
      return py_bool(py_str_ends_with(obj, &arg(0)) != 0);
    }
    if (m == "replace") {
      needArgs(m, nargs, 2, 2);
      needStrArg(m, arg(0), "第一个参数");
      needStrArg(m, arg(1), "第二个参数");
      return strReplace(*obj, arg(0), arg(1));
    }
    if (m == "find") {
      needArgs(m, nargs, 1, 2);
      needStrArg(m, arg(0), "参数");
      const int64_t from = nargs == 2 ? py_to_int(arg(1)) : 0;
      return py_int(py_str_find(obj, &arg(0), from));
    }
    if (m == "count") {
      needArgs(m, nargs, 1, 1);
      needStrArg(m, arg(0), "参数");
      return strCount(*obj, arg(0));
    }
    noSuchMethod(obj, m);
  }

  // ----------------------------------------------------------------- list
  if (obj->tag == PY_LIST) {
    if (m == "append") {
      needArgs(m, nargs, 1, 1);
      py_list_append(obj, &arg(0));
      return py_none();
    }
    if (m == "extend") {
      needArgs(m, nargs, 1, 1);
      needListArg(m, arg(0), "参数");
      py_list_extend(obj, &arg(0));
      return py_none();
    }
    if (m == "insert") {
      needArgs(m, nargs, 2, 2);
      py_list_insert(obj, py_to_int(arg(0)), &arg(1));
      return py_none();
    }
    if (m == "remove") {
      needArgs(m, nargs, 1, 1);
      py_list_remove(obj, &arg(0));
      return py_none();
    }
    if (m == "pop") {
      needArgs(m, nargs, 0, 1);
      return listPop(*obj, nargs == 1 ? &arg(0) : nullptr);
    }
    if (m == "index") {
      needArgs(m, nargs, 1, 1);
      return py_int(py_list_index(obj, &arg(0)));
    }
    if (m == "count") {
      needArgs(m, nargs, 1, 1);
      return py_int(py_list_count(obj, &arg(0)));
    }
    if (m == "reverse") {
      needArgs(m, nargs, 0, 0);
      py_list_reverse(obj);
      return py_none();
    }
    if (m == "sort") {
      needArgs(m, nargs, 0, 0);
      py_list_sort(obj);
      return py_none();
    }
    if (m == "clear") {
      needArgs(m, nargs, 0, 0);
      py_list_clear(obj);
      return py_none();
    }
    noSuchMethod(obj, m);
  }

  // ----------------------------------------------------------------- dict
  if (obj->tag == PY_DICT) {
    if (m == "get") {
      needArgs(m, nargs, 1, 2);
      return dictGet(*obj, arg(0), nargs == 2 ? &arg(1) : nullptr);
    }
    if (m == "keys") {
      needArgs(m, nargs, 0, 0);
      return dictProjection(*obj, true, false);
    }
    if (m == "values") {
      needArgs(m, nargs, 0, 0);
      return dictProjection(*obj, false, true);
    }
    if (m == "items") {
      needArgs(m, nargs, 0, 0);
      return dictProjection(*obj, true, true);
    }
    if (m == "pop") {
      needArgs(m, nargs, 1, 1);
      return py_dict_pop(obj, &arg(0));
    }
    if (m == "update") {
      needArgs(m, nargs, 1, 1);
      needDictArg(m, arg(0));
      const int64_t n = py_dict_len(&arg(0));
      for (int64_t i = 0; i < n; ++i) {
        PyValue k = py_dict_key_at(&arg(0), i);
        PyValue v = py_dict_val_at(&arg(0), i);
        py_dict_set(obj, &k, &v);
      }
      return py_none();
    }
    if (m == "clear") {
      needArgs(m, nargs, 0, 0);
      py_dict_clear(obj);
      return py_none();
    }
    noSuchMethod(obj, m);
  }

  static thread_local char buf[192];
  std::snprintf(buf, sizeof(buf), "'%s' 类型的值没有方法(方法调用只支持 str/list/dict)",
                py_tag_name(obj->tag));
  py_runtime_error(buf);
}
