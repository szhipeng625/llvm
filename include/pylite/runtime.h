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
PyValue py_add(PyValue a, PyValue b);
PyValue py_sub(PyValue a, PyValue b);
PyValue py_mul(PyValue a, PyValue b);
PyValue py_div(PyValue a, PyValue b);       // 真除法,恒为 float
PyValue py_floordiv(PyValue a, PyValue b);  // 向下取整除法
PyValue py_mod(PyValue a, PyValue b);       // 取模,符号跟随除数
PyValue py_pow(PyValue a, PyValue b);
PyValue py_neg(PyValue a);                  // 一元负号

// --- 比较(返回 bool 值的 PyValue)----------------------------------------
PyValue py_eq(PyValue a, PyValue b);
PyValue py_ne(PyValue a, PyValue b);
PyValue py_lt(PyValue a, PyValue b);
PyValue py_le(PyValue a, PyValue b);
PyValue py_gt(PyValue a, PyValue b);
PyValue py_ge(PyValue a, PyValue b);

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

// --- 输入 ---------------------------------------------------------------
// input(int, int) 在 IRGen 里被拆成若干个具体读取调用,结果打包成元组
// 再交给多目标赋值解包。全部返回 PyValue,在 IR 里声明成 void(ptr sret)。
PyValue py_read_int();
PyValue py_read_float();
PyValue py_read_bool();
PyValue py_read_str();

// --- 内存 ---------------------------------------------------------------
// v1 是只增不还的 bump 分配器(见 runtime/arena.cpp 的说明)
void *py_alloc(size_t n);
void py_arena_reset();

// --- 元组 ---------------------------------------------------------------
// 全部按指针收发,避免聚合体按值跨 ABI 边界
PyValue py_tuple_new(PyValue *elems, int64_t n);
int64_t py_tuple_len(const PyValue *t);
PyValue py_tuple_get(const PyValue *t, int64_t i);
void py_unpack(const PyValue *t, PyValue *out, int64_t n);

// --- 字符串 -------------------------------------------------------------
PyValue py_str_new(const char *bytes, int64_t len);
int64_t py_str_size(const PyValue *s);
const char *py_str_data(const PyValue *s);
PyValue py_str_concat(const PyValue *a, const PyValue *b);

}  // extern "C"
