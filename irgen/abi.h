// LLVM 侧 PyValue 的 ABI 封装。
//
// 全项目**只有这一处**把"聚合体按引用传参 + sret 返回"这条约定写下来。
// 每个调用点自己记这件事迟早会漏,而这个错误的表现形式是"链接通过、lookup
// 成功、一执行就段错误",极难定位。详见 docs/llvm-notes.md 第 2 节。
//
// 值的表示约定:IRGen 里每个 PyValue 都住在栈上的一个 alloca 槽位里,
// genExpr 返回的是"指向槽位的指针"而不是聚合体本身。
// 这样做的好处是传给运行时函数时天然就是指针,不需要在每个调用点临时 spill;
// 代价是多了一些栈流量,靠 LLVM 的优化去消。
#pragma once

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <map>
#include <string>

namespace pylite {

class Abi {
 public:
  explicit Abi(llvm::Module &M);

  llvm::StructType *valueTy() const { return valueTy_; }
  llvm::Type *i32Ty() const { return i32Ty_; }
  llvm::Type *i64Ty() const { return i64Ty_; }
  llvm::Type *f64Ty() const { return f64Ty_; }
  llvm::Type *ptrTy() const { return ptrTy_; }
  llvm::Type *voidTy() const { return voidTy_; }
  llvm::Attribute sretAttr() const { return sretAttr_; }

  // 分配一个 PyValue 槽位
  llvm::Value *newSlot(llvm::IRBuilder<> &B, const llvm::Twine &name = "v");

  // 往槽位里写装箱的常量
  void storeInt(llvm::IRBuilder<> &B, llvm::Value *slot, int64_t v);
  void storeFloat(llvm::IRBuilder<> &B, llvm::Value *slot, double v);
  void storeBool(llvm::IRBuilder<> &B, llvm::Value *slot, bool v);
  void storeNone(llvm::IRBuilder<> &B, llvm::Value *slot);

  // 把一个未装箱的 i64 SSA 值装箱后写入槽位(循环变量每轮要重新装箱)
  void storeI64AsInt(llvm::IRBuilder<> &B, llvm::Value *slot, llvm::Value *i64v);

  // 把槽位里的值原样复制到另一个槽位
  void copySlot(llvm::IRBuilder<> &B, llvm::Value *dst, llvm::Value *src);

  // 从槽位里读
  llvm::Value *loadTag(llvm::IRBuilder<> &B, llvm::Value *slot);
  llvm::Value *loadInt(llvm::IRBuilder<> &B, llvm::Value *slot);
  llvm::Value *loadFloat(llvm::IRBuilder<> &B, llvm::Value *slot);

  // --- 运行时调用 ---
  // 自动按正确 ABI 声明函数、投入槽位指针、把结果放进新槽位。

  // (PyValue, PyValue) -> PyValue
  llvm::Value *callBinary(llvm::IRBuilder<> &B, llvm::StringRef fn,
                          llvm::Value *a, llvm::Value *b);
  // (PyValue) -> PyValue
  llvm::Value *callUnary(llvm::IRBuilder<> &B, llvm::StringRef fn, llvm::Value *a);
  // () -> PyValue   —— 如 py_read_int()
  llvm::Value *callNullary(llvm::IRBuilder<> &B, llvm::StringRef fn);
  // (ptr, i64) -> PyValue —— 如 py_tuple_new(elems, n)、py_str_new(bytes, len)
  llvm::Value *callPtrI64(llvm::IRBuilder<> &B, llvm::StringRef fn,
                          llvm::Value *p, llvm::Value *i);
  // (PyValue) -> i32
  llvm::Value *callPredicate(llvm::IRBuilder<> &B, llvm::StringRef fn, llvm::Value *a);

  // py_print(PyValue* vals, i64 n) —— 数组参数是普通指针,不受聚合体规则影响
  void callPrint(llvm::IRBuilder<> &B, llvm::Value *slotArray, int64_t n);

  // py_unpack(const PyValue* t, PyValue* out, i64 n) —— 把元组解包到 out
  void callUnpack(llvm::IRBuilder<> &B, llvm::Value *tupleSlot,
                  llvm::Value *outArray, int64_t n);

  // (PyValue) -> i64。取出整数负载(供 range 的边界用);非整数会报运行时错误。
  llvm::Value *callUnaryI64(llvm::IRBuilder<> &B, llvm::StringRef fn, llvm::Value *a);

  // 把槽位里的值当布尔用,返回 i1
  llvm::Value *truthyAsI1(llvm::IRBuilder<> &B, llvm::Value *slot);

  // 生成 py_runtime_error(msg) 并接 unreachable。
  // 调用方负责先建好错误分支的基本块。
  void emitRuntimeError(llvm::IRBuilder<> &B, const llvm::Twine &msg);

 private:
  llvm::Function *declareBinRet(llvm::StringRef name);     // (PyValue,PyValue)->PyValue
  llvm::Function *declareUnaryRet(llvm::StringRef name);   // (PyValue)->PyValue
  llvm::Function *declareNullaryRet(llvm::StringRef name); // ()->PyValue
  llvm::Function *declarePtrI64Ret(llvm::StringRef name);  // (ptr,i64)->PyValue
  llvm::Function *declareUnaryPred(llvm::StringRef name);  // (PyValue)->i32
  llvm::Function *declareUnaryI64(llvm::StringRef name);   // (PyValue)->i64

  llvm::Value *packAggregate(llvm::IRBuilder<> &B, llvm::Value *tag, llvm::Value *payloadI64);

  llvm::Module &M_;
  llvm::LLVMContext &Ctx_;
  llvm::StructType *valueTy_ = nullptr;
  llvm::Type *i32Ty_ = nullptr;
  llvm::Type *i64Ty_ = nullptr;
  llvm::Type *f64Ty_ = nullptr;
  llvm::Type *ptrTy_ = nullptr;
  llvm::Type *voidTy_ = nullptr;
  llvm::Attribute sretAttr_;
  std::map<std::string, llvm::Function *> cache_;
};

}  // namespace pylite
