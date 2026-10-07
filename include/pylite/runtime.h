// PyLite 运行时库的 C ABI。
//
// IRGen 会对这里每个函数生成 call;用户手写的 C++ 也可以直接调用它们。
// 所有函数都从 libpylite_runtime.dll 导出(靠 MinGW 的 -shared 默认自动导出,
// 刻意不加 __declspec(dllexport) —— 那会触发 MinGW "遇到第一个 dllexport
// 就停止自动导出其余符号"的行为,反而让 JIT 找不到别的符号)。
//
// ⚠️ 跨边界约定 —— 改动前务必先读 docs/llvm-notes.md 第 2 节
//
// 下面这些函数在 C++ 源码里是按值的 `PyValue f(PyValue, PyValue)`,
// 但 IRGen 在 IR 里必须把它们声明成:
//
//     void @f(ptr sret(%PyValue), ptr, ptr)
//
// 即参数是"指向 PyValue 的指针"、返回值写进 sret 指针。
// 两者在机器层面一致,因为 Win64 ABI 规定 16 字节聚合体按引用传递、
// 返回值走隐藏 sret 寄存器(RCX = sret,RDX/R8/... = 各参数)。
//
// 若在 IR 里按值声明(`%PyValue @f(%PyValue, %PyValue)`),LLVM 会选另一套
// 降低方式,与 GCC 不匹配 —— 链接通过、lookup 成功,但一执行就段错误。
//
// 其它约定:
//   * 谓词一律返回 int32_t 而非 bool(i1 的 ABI 标注 GCC 与 Clang 不一致)
//   * 数组参数(如 py_print 的 PyValue*)是普通指针,不受上面限制
#pragma once

#include "pylite/value.h"

extern "C" {

// --- 错误 ---------------------------------------------------------------
// [[noreturn]] 不只是给编译器看的优化提示:没有它,GCC 会认为
// badOperands 这类同样标了 [[noreturn]] 的辅助函数"会返回",从而报警告。
[[noreturn]] void py_runtime_error(const char *msg);
const char *py_tag_name(int32_t tag);

// --- 构造 ---------------------------------------------------------------
// 刻意没有 py_int/py_float/py_bool 的运行时版本 —— 它们作为 inline 函数
// 定义在 value.h 里。IRGen 用 insertvalue 指令内联构造 PyValue,不发起调用;
// 若在这里再声明一份 extern "C" 版本,会与 value.h 的 C++ 链接版本构成
// 同名不同语言链接的非法重声明。

// --- 算术 ---------------------------------------------------------------
PyValue py_add(PyValue a, PyValue b);       // 数值相加 / 字符串拼接 / 列表拼接
PyValue py_sub(PyValue a, PyValue b);
PyValue py_mul(PyValue a, PyValue b);       // 数值相乘 / 字符串与列表重复
PyValue py_div(PyValue a, PyValue b);       // 真除法,恒为 float
PyValue py_floordiv(PyValue a, PyValue b);  // 向下取整除法
PyValue py_mod(PyValue a, PyValue b);       // 取模,符号跟随除数
PyValue py_pow(PyValue a, PyValue b);
PyValue py_neg(PyValue a);                  // 一元负号

// a += b。与 py_add 的区别**只在列表上**:Python 的 `a += [x]` 是就地 extend,
// 别名看得见改动;而 `a = a + [x]` 是重新绑定,别名不受影响。
// 第一个参数因此是可写的 —— 列表就地改,其余类型(数值、字符串不可变)
// 一律返回新值,由调用点写回自己的槽位。
PyValue py_iadd(PyValue *a, const PyValue *b);

// --- 比较(返回 bool 值的 PyValue)----------------------------------------
PyValue py_eq(PyValue a, PyValue b);
PyValue py_ne(PyValue a, PyValue b);
PyValue py_lt(PyValue a, PyValue b);
PyValue py_le(PyValue a, PyValue b);
PyValue py_gt(PyValue a, PyValue b);
PyValue py_ge(PyValue a, PyValue b);

// 三路比较,返回 -1 / 0 / 1。数值之间比大小、两个字符串按字典序比;
// 类型不可比时报运行时错误(供 < <= > >= 与 list.sort 共用)。
int32_t py_compare(const PyValue *a, const PyValue *b);

// --- 逻辑 ---------------------------------------------------------------
// and / or 的短路由 IRGen 用 PHI 实现(且必须返回操作数本身而非布尔值),
// 所以运行时只提供 not。
PyValue py_not(PyValue a);

// --- 真值判定 -----------------------------------------------------------
int32_t py_truthy(PyValue v);

// --- 转换 ---------------------------------------------------------------
// 取出整数负载;非整数时报运行时错误(用于 range 的边界)
int64_t py_to_int(PyValue v);

// --- 输出 ---------------------------------------------------------------
// 打印 n 个值,空格分隔,末尾换行。
void py_print(PyValue *vals, int64_t n);

// 按 repr 规则渲染成一个新字符串:容器里的字符串带引号并转义,
// 与 print 的顶层 str 规则不同 —— print("hi") 打 hi,print(["hi"]) 打 ['hi']。
// 目前供字典的 KeyError 类错误信息使用。
PyValue py_repr(const PyValue *v);

// --- 输入 ---------------------------------------------------------------
// input(int, int) 在 IRGen 里被拆成若干个具体读取调用,结果打包成元组
// 再交给多目标赋值解包。全部返回 PyValue,在 IR 里声明成 void(ptr sret)。
PyValue py_read_int();
PyValue py_read_float();
PyValue py_read_bool();
PyValue py_read_str();

// --- 内存与垃圾回收 ------------------------------------------------------
// 保守式标记-清除(见 runtime/gc.cpp 的说明)。只有 str/list/dict/tuple 是堆
// 对象,int/float/bool/None 是内联标量、根本不分配。
//
// 根 = 保守扫描本线程的栈与寄存器 + 显式根栈。**根必须放在栈上** —— 全局变量、
// 静态变量、thread_local 里放 PyValue 不会被认为是根,对象会被回收掉。
//
// 下面这些是运行时内部用的;用户手写 C++ 只需要 py_alloc。
typedef enum {
  PY_GC_SCAN_NONE = 0,    // 纯字节缓冲:字符串内容、临时的 char 缓冲
  PY_GC_SCAN_WORDS = 1,   // 保守逐字扫描:容器对象头(含 items/keys/vals 指针)
  PY_GC_SCAN_VALUES = 2,  // 按 PyValue 单元格扫描,由 tag 决定是否跟随 payload
} PyGcScan;

typedef struct {
  int64_t alloc_bytes_total;
  int64_t freed_bytes_total;
  int64_t live_bytes;
  int64_t live_blocks;
  int64_t collections;
  // 当前模式(来自环境变量)。dry_run 下被判定为死的块只下毒、**不归还内存**,
  // 所以"存活块数下降"这类断言在干跑模式里不成立 —— 测试要据此调整。
  int32_t dry_run;
  int32_t disabled;
} PyGcStats;

// 分配 n 字节并登记为一个 GC 块。n 会向上取整到 16 的倍数。
void *py_gc_alloc(size_t n, PyGcScan kind);
// SCAN_VALUES 的专用入口:从 firstCellOffset 起是若干 16 字节的 PyValue 单元格。
// ⚠️ 元组传 8(PyTuple 是 {int64 len; PyValue items[]}),传 0 会把 len 当 tag、
// 整条链错位,那是会丢根的。
void *py_gc_alloc_values(size_t n, size_t firstCellOffset);

void py_gc_collect();          // 手动触发一次回收
void py_gc_stats(PyGcStats *out);
void py_gc_init();             // 可选:提前读环境变量并注册 atexit 统计

// 显式根栈。用于运行时内部的 C++ 局部变量 —— 见 gc.cpp 里"最可能出错的一处"。
// slot 必须指向一个局部指针变量;取地址这个动作会迫使该值留在内存里,
// 于是不再依赖编译器恰好在 -O2 下把它 spill 到栈上。
void py_gc_root_push(void **slot);
void py_gc_root_pop(size_t n);

// 历史符号,保留以维持 ABI(用户手写 C++ 会直接调 py_alloc)。
// py_alloc 按最保守的方式扫描 —— 宁可多保留,不可漏根。
void *py_alloc(size_t n);
void py_arena_reset();         // 语义已改为"强制回收一次"

// --- 元组 ---------------------------------------------------------------
// 全部按指针收发,避免聚合体按值跨 ABI 边界
PyValue py_tuple_new(PyValue *elems, int64_t n);
int64_t py_tuple_len(const PyValue *t);
PyValue py_tuple_get(const PyValue *t, int64_t i);   // 支持负索引
void py_unpack(const PyValue *t, PyValue *out, int64_t n);

// --- 字符串 -------------------------------------------------------------
PyValue py_str_new(const char *bytes, int64_t len);
int64_t py_str_size(const PyValue *s);
const char *py_str_data(const PyValue *s);
PyValue py_str_concat(const PyValue *a, const PyValue *b);
PyValue py_str_get(const PyValue *s, int64_t i);          // 取单个字符,支持负索引
PyValue py_str_repeat(const PyValue *s, int64_t times);
// 按字节序比较,返回 -1 / 0 / 1
int32_t py_str_compare(const PyValue *a, const PyValue *b);
int32_t py_str_find(const PyValue *s, const PyValue *needle, int64_t from);
int32_t py_str_starts_with(const PyValue *s, const PyValue *prefix);
int32_t py_str_ends_with(const PyValue *s, const PyValue *suffix);

// --- 列表 ---------------------------------------------------------------
// PyList 是可变对象:append/insert/pop/... 就地改,别名的调用方看得见改动。
// 增长靠"新分配 + 拷贝",因为 bump 分配器没有 realloc(旧数组就地泄漏,
// 与项目其它部分一致,见 runtime/arena.cpp)。
PyValue py_list_new(PyValue *elems, int64_t n);
int64_t py_list_len(const PyValue *v);
PyValue py_list_get(const PyValue *v, int64_t i);          // 支持负索引
void py_list_set(const PyValue *v, int64_t i, const PyValue *val);
PyValue py_list_concat(const PyValue *a, const PyValue *b);  // 新列表
PyValue py_list_repeat(const PyValue *a, int64_t times);     // 新列表

void py_list_append(const PyValue *v, const PyValue *item);
void py_list_extend(const PyValue *v, const PyValue *other);
void py_list_insert(const PyValue *v, int64_t i, const PyValue *item);
void py_list_remove(const PyValue *v, const PyValue *item);
// idxOrNone 传的是一个 **None 值**(py_none()),不是空指针 —— 长度为 0 的
// 下标在语言里表达不出来,所以借 None 当"没给下标"的哨兵。传 nullptr 会崩。
PyValue py_list_pop(const PyValue *v, const PyValue *idxOrNone);
int64_t py_list_index(const PyValue *v, const PyValue *item);     // 找不到报错
int64_t py_list_count(const PyValue *v, const PyValue *item);
void py_list_reverse(const PyValue *v);
void py_list_clear(const PyValue *v);
void py_list_sort(const PyValue *v);

// --- 字典 ---------------------------------------------------------------
// 插入有序 + 线性扫描。不用哈希表:维护 Python 3.7 的可观测插入序,
// 且省掉为每种键类型写 hash 的面上。代价是查找 O(n),见 docs/language.md。
PyValue py_dict_new(PyValue *keys, PyValue *vals, int64_t n);
int64_t py_dict_len(const PyValue *d);
PyValue py_dict_get(const PyValue *d, const PyValue *key);   // 键不存在则报错
PyValue py_dict_get_default(const PyValue *d, const PyValue *key,
                            const PyValue *def);
int32_t py_dict_has(const PyValue *d, const PyValue *key);
void py_dict_set(const PyValue *d, const PyValue *key, const PyValue *val);
PyValue py_dict_key_at(const PyValue *d, int64_t i);   // 按插入序取第 i 个键
PyValue py_dict_val_at(const PyValue *d, int64_t i);
// pop 必须给键。这里刻意不用 None 当"没给键"的哨兵 —— None 本身是合法的键,
// 两者混淆会让 `d.pop(None)` 变得无法表达。
PyValue py_dict_pop(const PyValue *d, const PyValue *key);
void py_dict_clear(const PyValue *d);

// --- 下标 / 切片 / 长度 --------------------------------------------------
// "不支持下标"这类错误的唯一产生点。按 tag 分派到上面各类型的访问器。
int64_t py_len(const PyValue *v);       // str / list / dict / tuple
PyValue py_index(const PyValue *obj, const PyValue *idx);
// obj[idx] = val。列表按位置改,字典按键插入或更新(新增键追加在末尾)。
void py_setindex(const PyValue *obj, const PyValue *idx, const PyValue *val);
// lo / hi / step 省略时传 None(即 PY_NULL),各自按默认值处理
PyValue py_slice(const PyValue *obj, const PyValue *lo, const PyValue *hi,
                 const PyValue *step);

// for 循环用的"第 i 个元素"。**不能**拿 py_index 代替:遍历字典要的是第 i 个
// 键,而下标 d[k] 是按键查找,两者语义不同。
PyValue py_iter_at(const PyValue *obj, int64_t i);

// --- 方法调用 -----------------------------------------------------------
// obj.print(...) 形式统一走这一个入口,按 tag + 方法名分派。
// 方法表在运行时里,所以拼错方法名是**运行时**报错 —— 本项目的类型注解
// 本来就不做检查(见 docs/language.md),这是动态语言的必然。
PyValue py_call_method(const PyValue *obj, const char *name, int64_t nameLen,
                       PyValue *args, int64_t nargs);

}  // extern "C"
