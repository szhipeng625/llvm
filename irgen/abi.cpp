#include "abi.h"

#include "pylite/value.h"  // PY_INT 等 tag 常量,保证与 C++ 侧同源

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

#include <bit>  // std::bit_cast

using namespace llvm;

namespace pylite {

Abi::Abi(Module &M) : M_(M), Ctx_(M.getContext()) {
  i32Ty_ = Type::getInt32Ty(Ctx_);
  i64Ty_ = Type::getInt64Ty(Ctx_);
  f64Ty_ = Type::getDoubleTy(Ctx_);
  ptrTy_ = PointerType::getUnqual(Ctx_);
  voidTy_ = Type::getVoidTy(Ctx_);

  // 同一个 context 里可能建多个 Module(REPL 每次输入一个),
  // 所以先按名字查已有类型,避免反复 create 出 PyValue.1 / PyValue.2。
  valueTy_ = StructType::getTypeByName(Ctx_, "PyValue");
  if (!valueTy_) {
    valueTy_ = StructType::create(Ctx_, {i32Ty_, i64Ty_}, "PyValue");
  }

  sretAttr_ = Attribute::getWithStructRetType(Ctx_, valueTy_);
}

Value *Abi::newSlot(IRBuilder<> &B, const Twine &name) {
  return B.CreateAlloca(valueTy_, nullptr, name);
}

Value *Abi::packAggregate(IRBuilder<> &B, Value *tag, Value *payloadI64) {
  Value *agg = UndefValue::get(valueTy_);
  agg = B.CreateInsertValue(agg, tag, 0);
  agg = B.CreateInsertValue(agg, payloadI64, 1);
  return agg;
}

void Abi::storeInt(IRBuilder<> &B, Value *slot, int64_t v) {
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, PY_INT),
                              ConstantInt::get(i64Ty_, v)),
                slot);
}

void Abi::storeFloat(IRBuilder<> &B, Value *slot, double v) {
  // payload 字段在 LLVM 侧是 i64,所以要按位重新解释 double
  const uint64_t bits = std::bit_cast<uint64_t>(v);
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, PY_FLOAT),
                              ConstantInt::get(i64Ty_, bits)),
                slot);
}

void Abi::storeBool(IRBuilder<> &B, Value *slot, bool v) {
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, PY_BOOL),
                              ConstantInt::get(i64Ty_, v ? 1 : 0)),
                slot);
}

void Abi::storeNone(IRBuilder<> &B, Value *slot) {
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, PY_NULL),
                              ConstantInt::get(i64Ty_, 0)),
                slot);
}

void Abi::storeI64AsInt(IRBuilder<> &B, Value *slot, Value *i64v) {
  B.CreateStore(packAggregate(B, ConstantInt::get(i32Ty_, PY_INT), i64v), slot);
}

void Abi::copySlot(IRBuilder<> &B, Value *dst, Value *src) {
  B.CreateStore(B.CreateLoad(valueTy_, src), dst);
}

Value *Abi::loadTag(IRBuilder<> &B, Value *slot) {
  return B.CreateLoad(i32Ty_, B.CreateStructGEP(valueTy_, slot, 0), "tag");
}

Value *Abi::loadInt(IRBuilder<> &B, Value *slot) {
  return B.CreateLoad(i64Ty_, B.CreateStructGEP(valueTy_, slot, 1), "int");
}

Value *Abi::loadFloat(IRBuilder<> &B, Value *slot) {
  Value *bits = B.CreateLoad(i64Ty_, B.CreateStructGEP(valueTy_, slot, 1), "fbits");
  return B.CreateBitCast(bits, f64Ty_, "flt");
}

// ---------------------------------------------------------------------------
// 运行时函数声明
//
// 三个 declare* 是"聚合体按引用 + sret"这条约定的唯一落地点。
// 注意每个函数只声明一次并缓存 —— 重复创建同名函数会让 LLVM 把后来的
// 改名成 py_add.1,符号就找不到了。
// ---------------------------------------------------------------------------

Function *Abi::declareBinRet(StringRef name) {
  auto it = cache_.find(name.str());
  if (it != cache_.end()) return it->second;

  // void @name(ptr sret(%PyValue), ptr %a, ptr %b)
  auto *ft = FunctionType::get(voidTy_, {ptrTy_, ptrTy_, ptrTy_}, false);
  auto *f = Function::Create(ft, Function::ExternalLinkage, name, M_);
  f->addParamAttr(0, sretAttr_);
  cache_[name.str()] = f;
  return f;
}

Function *Abi::declareUnaryRet(StringRef name) {
  auto it = cache_.find(name.str());
  if (it != cache_.end()) return it->second;

  // void @name(ptr sret(%PyValue), ptr %a)
  auto *ft = FunctionType::get(voidTy_, {ptrTy_, ptrTy_}, false);
  auto *f = Function::Create(ft, Function::ExternalLinkage, name, M_);
  f->addParamAttr(0, sretAttr_);
  cache_[name.str()] = f;
  return f;
}

Function *Abi::declareUnaryPred(StringRef name) {
  auto it = cache_.find(name.str());
  if (it != cache_.end()) return it->second;

  // i32 @name(ptr %a)  —— 谓词返回 i32 而非 i1,见 value.h
  auto *ft = FunctionType::get(i32Ty_, {ptrTy_}, false);
  auto *f = Function::Create(ft, Function::ExternalLinkage, name, M_);
  cache_[name.str()] = f;
  return f;
}

Function *Abi::declareUnaryI64(StringRef name) {
  auto it = cache_.find(name.str());
  if (it != cache_.end()) return it->second;

  // i64 @name(ptr %a)
  auto *ft = FunctionType::get(i64Ty_, {ptrTy_}, false);
  auto *f = Function::Create(ft, Function::ExternalLinkage, name, M_);
  cache_[name.str()] = f;
  return f;
}

Function *Abi::declareNullaryRet(StringRef name) {
  auto it = cache_.find(name.str());
  if (it != cache_.end()) return it->second;

  // void @name(ptr sret(%PyValue))
  auto *ft = FunctionType::get(voidTy_, {ptrTy_}, false);
  auto *f = Function::Create(ft, Function::ExternalLinkage, name, M_);
  f->addParamAttr(0, sretAttr_);
  cache_[name.str()] = f;
  return f;
}

Function *Abi::declarePtrI64Ret(StringRef name) {
  auto it = cache_.find(name.str());
  if (it != cache_.end()) return it->second;

  // void @name(ptr sret(%PyValue), ptr, i64)
  auto *ft = FunctionType::get(voidTy_, {ptrTy_, ptrTy_, i64Ty_}, false);
  auto *f = Function::Create(ft, Function::ExternalLinkage, name, M_);
  f->addParamAttr(0, sretAttr_);
  cache_[name.str()] = f;
  return f;
}

Value *Abi::callBinary(IRBuilder<> &B, StringRef fn, Value *a, Value *b) {
  Function *f = declareBinRet(fn);
  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
  CallInst *call = B.CreateCall(f, {ret, a, b});
  // 属性不会自动从声明传播到调用点,必须再标一次
  call->addParamAttr(0, sretAttr_);
  return ret;
}

Value *Abi::callUnary(IRBuilder<> &B, StringRef fn, Value *a) {
  Function *f = declareUnaryRet(fn);
  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
  CallInst *call = B.CreateCall(f, {ret, a});
  call->addParamAttr(0, sretAttr_);
  return ret;
}

Value *Abi::callPredicate(IRBuilder<> &B, StringRef fn, Value *a) {
  Function *f = declareUnaryPred(fn);
  return B.CreateCall(f, {a}, (Twine(fn) + ".r").str());
}

Value *Abi::callNullary(IRBuilder<> &B, StringRef fn) {
  Function *f = declareNullaryRet(fn);
  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
  CallInst *call = B.CreateCall(f, {ret});
  call->addParamAttr(0, sretAttr_);
  return ret;
}

Value *Abi::callPtrI64(IRBuilder<> &B, StringRef fn, Value *p, Value *i) {
  Function *f = declarePtrI64Ret(fn);
  Value *ret = newSlot(B, (Twine(fn) + ".ret").str());
  CallInst *call = B.CreateCall(f, {ret, p, i});
  call->addParamAttr(0, sretAttr_);
  return ret;
}

void Abi::callPrint(IRBuilder<> &B, Value *slotArray, int64_t n) {
  auto *ft = FunctionType::get(voidTy_, {ptrTy_, i64Ty_}, false);
  Function *f = M_.getFunction("py_print");
  if (!f) {
    f = Function::Create(ft, Function::ExternalLinkage, "py_print", M_);
  }
  B.CreateCall(f, {slotArray, ConstantInt::get(i64Ty_, n)});
}

void Abi::callUnpack(IRBuilder<> &B, Value *tupleSlot, Value *outArray, int64_t n) {
  // void @py_unpack(ptr %tuple, ptr %out, i64 %n)
  // 三个参数都是普通标量/指针,不涉及聚合体按值传递
  auto *ft = FunctionType::get(voidTy_, {ptrTy_, ptrTy_, i64Ty_}, false);
  Function *f = M_.getFunction("py_unpack");
  if (!f) {
    f = Function::Create(ft, Function::ExternalLinkage, "py_unpack", M_);
  }
  B.CreateCall(f, {tupleSlot, outArray, ConstantInt::get(i64Ty_, n)});
}

Value *Abi::truthyAsI1(IRBuilder<> &B, Value *slot) {
  Value *t = callPredicate(B, "py_truthy", slot);
  return B.CreateICmpNE(t, ConstantInt::get(i32Ty_, 0), "truthy");
}

Value *Abi::callUnaryI64(IRBuilder<> &B, StringRef fn, Value *a) {
  Function *f = declareUnaryI64(fn);
  return B.CreateCall(f, {a}, (Twine(fn) + ".r").str());
}

void Abi::emitRuntimeError(IRBuilder<> &B, const Twine &msg) {
  Function *f = M_.getFunction("py_runtime_error");
  if (!f) {
    auto *ft = FunctionType::get(voidTy_, {ptrTy_}, false);
    f = Function::Create(ft, Function::ExternalLinkage, "py_runtime_error", M_);
    f->addFnAttr(Attribute::NoReturn);
  }
  Value *str = B.CreateGlobalString(msg.str(), ".errmsg");
  B.CreateCall(f, {str});
  B.CreateUnreachable();
}

}  // namespace pylite
