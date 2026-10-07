// 下标、切片与长度 —— 按 tag 分派到各容器的访问器。
//
// 把这些集中在一处,是为了让"XX 不支持下标""XX 不支持 len()"这类错误只有
// 一个产生点:错误信息统一,而且新增容器类型时不会漏掉某一条路径。
//
// 注意本文件**看不见** PyList / PyDict / PyStr 的内部布局 —— 那些结构体是各
// 自翻译单元局部的(与 tuple.cpp 同一模式),这里只能组合 py_list_len 这类
// 导出的访问器。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstring>

namespace {

// 报错主语放在类型上:"'int' 不支持下标访问" 比 "下标访问 不支持 'int'" 好读
[[noreturn]] void badType(const char *what, const PyValue *v) {
  static thread_local char buf[160];
  std::snprintf(buf, sizeof(buf), "'%s' 不支持%s", py_tag_name(v->tag), what);
  py_runtime_error(buf);
}

// 切片边界:省略(None)时按方向取默认值,给了就归一化并夹到合法区间。
// 这一段是 Python 的 PySlice_GetIndicesEx 语义,负步长下默认值恰好相反 ——
// 这是最容易写错的地方。
void sliceBounds(int64_t len, const PyValue *lo, const PyValue *hi,
                 const PyValue *step, int64_t *start, int64_t *count) {
  int64_t st = (step->tag == PY_NULL) ? 1 : py_to_int(*step);
  if (st == 0) py_runtime_error("切片的步长不能为 0");

  const int64_t lower = (st > 0) ? 0 : -1;
  const int64_t upper = (st > 0) ? len : len - 1;

  int64_t a;
  if (lo->tag == PY_NULL) {
    a = (st > 0) ? lower : upper;
  } else {
    a = py_to_int(*lo);
    if (a < 0) a += len;
    if (a < lower) a = lower;
    if (a > upper) a = upper;
  }

  int64_t b;
  if (hi->tag == PY_NULL) {
    b = (st > 0) ? upper : lower;
  } else {
    b = py_to_int(*hi);
    if (b < 0) b += len;
    if (b < lower) b = lower;
    if (b > upper) b = upper;
  }

  // 数出会走到多少个元素。用算式而不是循环,免得 [:] 在大容器上退化成 O(n)
  // 的额外一趟。
  int64_t n = 0;
  if (st > 0) {
    if (b > a) n = (b - a + st - 1) / st;
  } else {
    if (a > b) n = (a - b + (-st) - 1) / (-st);
  }

  *start = a;
  *count = n;
}

// 按下标取元素。切片与遍历都要用,所以单独抽出来 —— 但它**不是**对外的
// py_index:那一个还要处理字典的按键查找。
PyValue elementAt(const PyValue *obj, int64_t i) {
  switch (obj->tag) {
    case PY_LIST:  return py_list_get(obj, i);
    case PY_TUPLE: return py_tuple_get(obj, i);
    case PY_STR:   return py_str_get(obj, i);
    default:       badType("下标访问", obj);
  }
}

}  // namespace

// 长度。返回 i64 而非 PyValue:for 循环的条件要直接拿它比,
// 而 `len(x)` 的调用点自己装箱(见 irgen.cpp)。
extern "C" int64_t py_len(const PyValue *v) {
  switch (v->tag) {
    case PY_LIST:  return py_list_len(v);
    case PY_DICT:  return py_dict_len(v);
    case PY_TUPLE: return py_tuple_len(v);
    case PY_STR:   return py_str_size(v);
    default:       badType("len()", v);
  }
}

extern "C" PyValue py_index(const PyValue *obj, const PyValue *idx) {
  switch (obj->tag) {
    case PY_LIST:
    case PY_TUPLE:
    case PY_STR:
      return elementAt(obj, py_to_int(*idx));
    case PY_DICT:
      // 字典是按键查找,不是"第 i 个" —— 遍历字典要的是键,走 py_iter_at
      return py_dict_get(obj, idx);
    default:
      badType("下标访问", obj);
  }
}

extern "C" void py_setindex(const PyValue *obj, const PyValue *idx,
                            const PyValue *val) {
  switch (obj->tag) {
    case PY_LIST:
      py_list_set(obj, py_to_int(*idx), val);
      return;
    case PY_DICT:
      py_dict_set(obj, idx, val);
      return;
    default:
      // 字符串/元组不可变 —— 与 Python 报 TypeError 的判定一致
      badType("下标赋值", obj);
  }
}

extern "C" PyValue py_iter_at(const PyValue *obj, int64_t i) {
  // 遍历字典产出的是**键**,不能走 py_index(那是按键查找,语义完全不同)
  if (obj->tag == PY_DICT) return py_dict_key_at(obj, i);
  return elementAt(obj, i);
}

extern "C" PyValue py_slice(const PyValue *obj, const PyValue *lo,
                            const PyValue *hi, const PyValue *step) {
  const int64_t len = py_len(obj);

  int64_t start = 0, count = 0;
  sliceBounds(len, lo, hi, step, &start, &count);

  const int64_t st = (step->tag == PY_NULL) ? 1 : py_to_int(*step);

  if (obj->tag == PY_STR) {
    // 步长为 1 时是连续区间,直接一次拷贝;否则逐字符拼。
    const char *data = py_str_data(obj);
    if (st == 1) return py_str_new(data + start, count);
    // ⚠️ 显式根:buf 要在随后的 py_str_new 分配期间活下来
    char *buf = nullptr;
    py_gc_root_push(reinterpret_cast<void **>(&buf));
    buf = static_cast<char *>(
        py_gc_alloc(static_cast<size_t>(count) + 1, PY_GC_SCAN_NONE));
    for (int64_t k = 0; k < count; ++k) buf[k] = data[start + k * st];
    PyValue out = py_str_new(buf, count);
    py_gc_root_pop(1);
    return out;
  }

  if (obj->tag == PY_LIST || obj->tag == PY_TUPLE) {
    PyValue *elems = count > 0 ? static_cast<PyValue *>(py_gc_alloc_values(
                                     static_cast<size_t>(count) * sizeof(PyValue), 0))
                               : nullptr;
    for (int64_t k = 0; k < count; ++k) elems[k] = elementAt(obj, start + k * st);
    // 切片的类型跟着原对象:列表切出列表,元组切出元组(与 Python 一致)
    return obj->tag == PY_LIST ? py_list_new(elems, count)
                               : py_tuple_new(elems, count);
  }

  badType("切片", obj);
}
