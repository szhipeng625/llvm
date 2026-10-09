// OOP 语言的运行时支撑库
// 为编译生成的可执行程序提供:动态容器、对象打印、整数装箱、字符串操作等能力。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Vec {
  std::vector<void*> items;
};

extern "C" {

void* rt_vec_new() { return new Vec(); }

void rt_vec_push(void* v, void* val) {
  if (!v) return;
  static_cast<Vec*>(v)->items.push_back(val);
}

void* rt_vec_get(void* v, int64_t idx) {
  if (!v) return nullptr;
  auto& xs = static_cast<Vec*>(v)->items;
  if (idx < 0 || (size_t)idx >= xs.size()) {
    fprintf(stderr, "运行时错误: 容器下标越界 %lld (长度 %zu)\n", (long long)idx, xs.size());
    exit(1);
  }
  return xs[(size_t)idx];
}

void rt_vec_set(void* v, int64_t idx, void* val) {
  if (!v) return;
  auto& xs = static_cast<Vec*>(v)->items;
  if (idx < 0 || (size_t)idx >= xs.size()) {
    fprintf(stderr, "运行时错误: 容器下标越界 %lld\n", (long long)idx);
    exit(1);
  }
  xs[(size_t)idx] = val;
}

int64_t rt_vec_len(void* v) {
  if (!v) return 0;
  return (int64_t)static_cast<Vec*>(v)->items.size();
}

void* rt_box_int(int64_t x) {
  int64_t* p = (int64_t*)malloc(sizeof(int64_t));
  *p = x;
  return p;
}

int64_t rt_unbox_int(void* p) {
  if (!p) return 0;
  return *(int64_t*)p;
}

void rt_print_int(int64_t x) { printf("%lld\n", (long long)x); }

void rt_print_bool(int64_t x) { printf("%s\n", x ? "true" : "false"); }

void rt_print_str(const char* s) { printf("%s\n", s ? s : "(null)"); }

void* rt_print_obj(void* p) {
  printf("<对象 @ %p>\n", p);
  return p;
}

void* rt_str_concat(const char* a, const char* b) {
  std::string s = std::string(a ? a : "") + std::string(b ? b : "");
  char* out = (char*)malloc(s.size() + 1);
  memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

int64_t rt_str_eq(const char* a, const char* b) {
  if (!a && !b) return 1;
  if (!a || !b) return 0;
  return strcmp(a, b) == 0 ? 1 : 0;
}

}  // extern "C"
