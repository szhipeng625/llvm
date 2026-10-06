// 输入输出。
//
// py_print 收的是 PyValue 数组的指针 —— 聚合体本身不跨 ABI 边界按值传,
// 但"指向数组的指针"没问题,元素在内存里按 PyValue 布局排列。
#include "pylite/runtime.h"

#include <cstdio>
#include <cstdlib>  // strtod
#include <cstring>

namespace {

// 模仿 Python 的浮点显示:用能唯一还原该值的最短表示。
// 直接写 %.17g 会打出 0.10000000000000001 这种噪声,%.6g 又会丢精度,
// 所以从一个很短的精度开始试,直到字符串能精确往返为止。
void printDouble(std::FILE *f, double d) {
  char buf[64];
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
    if (std::strtod(buf, nullptr) == d) break;
  }
  // 整数值要显示成 3.0 而不是 3,否则类型看起来像 int
  if (std::strpbrk(buf, ".eEnN") == nullptr) {
    std::strncat(buf, ".0", sizeof(buf) - std::strlen(buf) - 1);
  }
  std::fputs(buf, f);
}

void printOne(std::FILE *f, PyValue v);

void printTuple(std::FILE *f, PyValue v) {
  const int64_t n = py_tuple_len(&v);
  std::fputc('(', f);
  for (int64_t i = 0; i < n; ++i) {
    if (i) std::fputs(", ", f);
    printOne(f, py_tuple_get(&v, i));
  }
  // 单元素元组要写成 (x,),与 Python 一致
  if (n == 1) std::fputc(',', f);
  std::fputc(')', f);
}

void printOne(std::FILE *f, PyValue v) {
  switch (v.tag) {
    case PY_NULL:
      std::fputs("None", f);
      break;
    case PY_BOOL:
      std::fputs(v.as.i ? "True" : "False", f);
      break;
    case PY_INT:
      std::fprintf(f, "%lld", static_cast<long long>(v.as.i));
      break;
    case PY_FLOAT:
      printDouble(f, v.as.f);
      break;
    case PY_STR:
      std::fwrite(py_str_data(&v), 1, static_cast<size_t>(py_str_size(&v)), f);
      break;
    case PY_TUPLE:
      printTuple(f, v);
      break;
    default:
      std::fprintf(f, "<%s>", py_tag_name(v.tag));
      break;
  }
}

// 读一整行。
// Windows 上 stdin 是文本模式,会给出行尾的 \r\n,统一裁掉;
// 读之前先冲 stdout,否则提示语会卡在缓冲区里,用户看不到就被阻塞住。
bool readLine(char *buf, size_t cap) {
  std::fflush(stdout);
  if (std::fgets(buf, static_cast<int>(cap), stdin) == nullptr) return false;
  size_t len = std::strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[--len] = '\0';
  }
  return true;
}

[[noreturn]] void badNumber(const char *what, const char *text) {
  static thread_local char buf[256];
  std::snprintf(buf, sizeof(buf), "输入的不是合法的 %s: '%s'", what, text);
  py_runtime_error(buf);
}

}  // namespace

// print(a, b, c) —— 以空格分隔,末尾换行。与 Python 的 print 一致。
extern "C" void py_print(PyValue *vals, int64_t n) {
  for (int64_t i = 0; i < n; ++i) {
    if (i) std::fputc(' ', stdout);
    printOne(stdout, vals[i]);
  }
  std::fputc('\n', stdout);
}

// ---------------------------------------------------------------------------
// input(...)
//
// input(int, int) 在 IRGen 里被拆成若干个这里的具体读取函数,
// 结果再打包成元组交给多目标赋值去解包。
// ---------------------------------------------------------------------------

extern "C" PyValue py_read_int() {
  char buf[256];
  if (!readLine(buf, sizeof(buf))) py_runtime_error("读取输入失败(输入已结束)");
  char *end = nullptr;
  const long long v = std::strtoll(buf, &end, 10);
  if (end == buf) badNumber("整数", buf);
  return py_int(v);
}

extern "C" PyValue py_read_float() {
  char buf[256];
  if (!readLine(buf, sizeof(buf))) py_runtime_error("读取输入失败(输入已结束)");
  char *end = nullptr;
  const double v = std::strtod(buf, &end);
  if (end == buf) badNumber("浮点数", buf);
  return py_float(v);
}

extern "C" PyValue py_read_bool() {
  char buf[256];
  if (!readLine(buf, sizeof(buf))) py_runtime_error("读取输入失败(输入已结束)");
  if (std::strcmp(buf, "True") == 0 || std::strcmp(buf, "true") == 0 ||
      std::strcmp(buf, "1") == 0) {
    return py_bool(true);
  }
  if (std::strcmp(buf, "False") == 0 || std::strcmp(buf, "false") == 0 ||
      std::strcmp(buf, "0") == 0) {
    return py_bool(false);
  }
  badNumber("布尔值(应为 True/False)", buf);
}

extern "C" PyValue py_read_str() {
  char buf[1024];
  if (!readLine(buf, sizeof(buf))) py_runtime_error("读取输入失败(输入已结束)");
  return py_str_new(buf, static_cast<int64_t>(std::strlen(buf)));
}
