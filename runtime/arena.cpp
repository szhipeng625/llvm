// 极简 bump 分配器。
//
// v1 不回收内存 —— 这是刻意接受的限制。字符串拼接、列表追加在循环里会
// 无界增长,README 与 docs/language.md 里都写明了。
//
// 为什么不上引用计数:它与 and/or 返回操作数本身的语义(同一个值可能被
// 多个槽位共享却不增加计数)、以及与"运行时错误直接 exit 不展开栈"的策略
// 都冲突。留到 v2 再说。
#include "pylite/runtime.h"

#include <cstddef>
#include <cstdlib>

namespace {

constexpr size_t kChunkSize = 64 * 1024;

struct Chunk {
  Chunk *next;
  size_t used;
  size_t cap;
  char *data() { return reinterpret_cast<char *>(this + 1); }
};

Chunk *g_head = nullptr;

}  // namespace

extern "C" void *py_alloc(size_t n) {
  // 16 字节对齐,保证能放得下 PyValue 这类含 i64/double 的结构
  n = (n + 15) & ~static_cast<size_t>(15);

  if (g_head == nullptr || g_head->used + n > g_head->cap) {
    const size_t cap = n > kChunkSize ? n : kChunkSize;
    Chunk *c = static_cast<Chunk *>(std::malloc(sizeof(Chunk) + cap));
    if (c == nullptr) py_runtime_error("内存耗尽");
    c->next = g_head;
    c->used = 0;
    c->cap = cap;
    g_head = c;
  }

  void *p = g_head->data() + g_head->used;
  g_head->used += n;
  return p;
}

// 供 REPL 在每条语句之间回收。注意会一次性丢弃全部内存,
// 调用方必须保证没有活着的值仍被引用。
extern "C" void py_arena_reset() {
  Chunk *c = g_head;
  while (c != nullptr) {
    Chunk *next = c->next;
    std::free(c);
    c = next;
  }
  g_head = nullptr;
}
