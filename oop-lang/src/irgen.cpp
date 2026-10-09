#include "lang.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <stdexcept>
#include <map>
#include <optional>

using namespace llvm;

namespace {

struct ClassInfo {
  const ClassDecl* ast = nullptr;
  std::string name, base;
  ClassInfo* baseInfo = nullptr;
  std::vector<std::pair<std::string,std::string>> allFields;
  std::map<std::string,int> fieldIndex;
  std::map<std::string,int> slot;        // 方法名 -> 虚表槽位
  std::map<int,std::string> slotName;    // 槽位 -> 方法名
  std::map<std::string,std::pair<std::string,std::vector<std::string>>> sig;
  std::map<std::string, Function*> ownFn;
  StructType* structTy = nullptr;
  GlobalVariable* vtable = nullptr;
  int numSlots = 0;
};

class CodeGen {
public:
  explicit CodeGen(const Program& p) : prog_(p), mod_("oop", ctx_) {
    i64_ = b_.getInt64Ty(); i32_ = b_.getInt32Ty(); i1_ = b_.getInt1Ty();
    ptr_ = PointerType::get(ctx_, 0); voidTy_ = b_.getVoidTy();
  }

  void run(const std::string& outIr, const std::string& outObj) {
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
    std::string tripleStr = sys::getDefaultTargetTriple();
    Triple triple(tripleStr);
    mod_.setTargetTriple(triple);
    std::string err;
    auto* target = TargetRegistry::lookupTarget(triple, err);
    if (!target) throw std::runtime_error("无法找到目标平台: " + err);
    auto* tm = target->createTargetMachine(triple, "generic", "", {}, std::optional<Reloc::Model>());
    mod_.setDataLayout(tm->createDataLayout());
    collect();
    declareRuntime();
    declareMethods();
    emitVtables();
    emitMethods();
    emitMain();
    std::string verr;
    raw_string_ostream vs(verr);
    if (verifyModule(mod_, &vs)) throw std::runtime_error("IR 校验失败:\n" + verr);
    if (!outIr.empty()) {
      std::error_code ec;
      raw_fd_ostream os(outIr, ec);
      mod_.print(os, nullptr);
    }
    if (!outObj.empty()) {
      std::error_code ec;
      raw_fd_ostream dest(outObj, ec, sys::fs::OF_None);
      legacy::PassManager pm;
      if (tm->addPassesToEmitFile(pm, dest, nullptr, CodeGenFileType::ObjectFile))
        throw std::runtime_error("无法生成目标文件");
      pm.run(mod_);
    }
  }

private:
  const Program& prog_;
  LLVMContext ctx_;
  Module mod_;
  IRBuilder<> b_{ctx_};
  Type *i64_, *i32_, *i1_, *ptr_, *voidTy_;
  std::map<std::string, ClassInfo> classes_;
  Function* curFn_ = nullptr;
  ClassInfo* curClass_ = nullptr;
  Value* thisVal_ = nullptr;
  std::vector<std::map<std::string,std::pair<std::string,Value*>>> scopes_;

  Function *fnCalloc_, *fnVecNew_, *fnVecPush_, *fnVecGet_, *fnVecSet_, *fnVecLen_;
  Function *fnBoxInt_, *fnUnboxInt_, *fnPrintInt_, *fnPrintBool_, *fnPrintStr_, *fnPrintObj_;
  Function *fnStrConcat_, *fnStrEq_;

  Function* decl(const char* name, Type* ret, std::vector<Type*> params) {
    auto* ft = FunctionType::get(ret, params, false);
    return Function::Create(ft, GlobalValue::ExternalLinkage, name, mod_);
  }

  Type* llvmType(const std::string& t) {
    if (t == "void") return voidTy_;
    if (t == "int" || t == "bool") return i64_;
    return ptr_;
  }
  Type* fieldType(const std::string& t) { return (t == "int" || t == "bool") ? i64_ : ptr_; }
  bool isScalar(const std::string& t) { return t == "int" || t == "bool"; }

  void collect() {
    for (auto& c : prog_.classes) {
      ClassInfo ci;
      ci.ast = &c;
      ci.name = c.name;
      ci.base = c.base;
      if (c.hasBase) {
        auto it = classes_.find(c.base);
        if (it == classes_.end()) throw std::runtime_error("找不到父类: " + c.base);
        ci.baseInfo = &it->second;
        ci.allFields = ci.baseInfo->allFields;
        ci.slot = ci.baseInfo->slot;
        ci.slotName = ci.baseInfo->slotName;
        ci.sig = ci.baseInfo->sig;
        ci.numSlots = ci.baseInfo->numSlots;
      }
      for (auto& f : c.ownFields) ci.allFields.push_back({f.ty, f.name});
      for (size_t i = 0; i < ci.allFields.size(); i++) ci.fieldIndex[ci.allFields[i].second] = (int)i;
      for (auto& m : c.methods) {
        if (!ci.slot.count(m.name)) {
          ci.slot[m.name] = ci.numSlots;
          ci.slotName[ci.numSlots] = m.name;
          ci.numSlots++;
        }
        std::vector<std::string> pts;
        for (auto& p : m.params) pts.push_back(p.first);
        ci.sig[m.name] = {m.retTy, pts};
      }
      classes_[c.name] = std::move(ci);
    }
    for (auto& [name, ci] : classes_) {
      std::vector<Type*> elems;
      elems.push_back(ptr_);
      for (auto& f : ci.allFields) elems.push_back(fieldType(f.first));
      ci.structTy = StructType::create(ctx_, elems, name + "_t");
    }
  }

  void declareRuntime() {
    fnCalloc_ = decl("calloc", ptr_, {i64_, i64_});
    fnVecNew_ = decl("rt_vec_new", ptr_, {});
    fnVecPush_ = decl("rt_vec_push", voidTy_, {ptr_, ptr_});
    fnVecGet_ = decl("rt_vec_get", ptr_, {ptr_, i64_});
    fnVecSet_ = decl("rt_vec_set", voidTy_, {ptr_, i64_, ptr_});
    fnVecLen_ = decl("rt_vec_len", i64_, {ptr_});
    fnBoxInt_ = decl("rt_box_int", ptr_, {i64_});
    fnUnboxInt_ = decl("rt_unbox_int", i64_, {ptr_});
    fnPrintInt_ = decl("rt_print_int", voidTy_, {i64_});
    fnPrintBool_ = decl("rt_print_bool", voidTy_, {i64_});
    fnPrintStr_ = decl("rt_print_str", voidTy_, {ptr_});
    fnPrintObj_ = decl("rt_print_obj", voidTy_, {ptr_});
    fnStrConcat_ = decl("rt_str_concat", ptr_, {ptr_, ptr_});
    fnStrEq_ = decl("rt_str_eq", i64_, {ptr_, ptr_});
  }

  void declareMethods() {
    for (auto& [name, ci] : classes_) {
      for (auto& m : ci.ast->methods) {
        std::string fnName = name + "_" + m.name;
        std::vector<Type*> params; params.push_back(ptr_);
        for (auto& p : m.params) params.push_back(llvmType(p.first));
        auto* ft = FunctionType::get(llvmType(m.retTy), params, false);
        ci.ownFn[m.name] = Function::Create(ft, GlobalValue::ExternalLinkage, fnName, mod_);
      }
    }
  }

  Function* resolveFn(ClassInfo* ci, const std::string& mname) {
    for (ClassInfo* c = ci; c; c = c->baseInfo)
      if (c->ownFn.count(mname)) return c->ownFn[mname];
    return nullptr;
  }

  void emitVtables() {
    for (auto& [name, ci] : classes_) {
      std::vector<Constant*> entries;
      for (int s = 0; s < ci.numSlots; s++) {
        auto it = ci.slotName.find(s);
        Function* f = it != ci.slotName.end() ? resolveFn(&ci, it->second) : nullptr;
        entries.push_back(f ? (Constant*)f : ConstantPointerNull::get(cast<PointerType>(ptr_)));
      }
      if (entries.empty()) entries.push_back(ConstantPointerNull::get(cast<PointerType>(ptr_)));
      auto* arrTy = ArrayType::get(ptr_, entries.size());
      auto* init = ConstantArray::get(arrTy, entries);
      ci.vtable = new GlobalVariable(mod_, arrTy, true, GlobalValue::PrivateLinkage, init, name + "_vtable");
    }
  }

  void emitMethods() {
    for (auto& [name, ci] : classes_) {
      for (auto& m : ci.ast->methods) {
        curClass_ = &ci;
        curFn_ = ci.ownFn[m.name];
        scopes_.clear();
        scopes_.push_back({});
        thisVal_ = curFn_->getArg(0);
        thisVal_->setName("this");
        auto* entry = BasicBlock::Create(ctx_, "entry", curFn_);
        b_.SetInsertPoint(entry);
        for (size_t i = 0; i < m.params.size(); i++) {
          Value* a = curFn_->getArg(i + 1);
          std::string pn = m.params[i].second;
          auto* slot = b_.CreateAlloca(llvmType(m.params[i].first), nullptr, pn);
          Type* want = slot->getAllocatedType();
          Value* st = a;
          if (a->getType() != want) {
            if (want->isIntegerTy() && a->getType()->isPointerTy()) st = b_.CreatePtrToInt(a, i64_);
            else if (want->isPointerTy() && a->getType()->isIntegerTy()) st = b_.CreateIntToPtr(a, ptr_);
          }
          b_.CreateStore(st, slot);
          scopes_.back()[pn] = {m.params[i].first, slot};
        }
        for (auto& s : m.body) genStmt(s.get());
        if (m.retTy == "void") {
          if (!b_.GetInsertBlock()->getTerminator()) b_.CreateRetVoid();
        } else {
          if (!b_.GetInsertBlock()->getTerminator())
            b_.CreateRet(Constant::getNullValue(llvmType(m.retTy)));
        }
      }
    }
  }

  void emitMain() {
    curClass_ = nullptr;
    auto* ft = FunctionType::get(i32_, {}, false);
    auto* fn = Function::Create(ft, GlobalValue::ExternalLinkage, "main", mod_);
    curFn_ = fn;
    scopes_.clear();
    scopes_.push_back({});
    auto* entry = BasicBlock::Create(ctx_, "entry", fn);
    b_.SetInsertPoint(entry);
    for (auto& s : prog_.mainBody) genStmt(s.get());
    if (!b_.GetInsertBlock()->getTerminator()) b_.CreateRet(ConstantInt::get(i32_, 0));
  }

  void genStmt(Stmt* s) {
    switch (s->k) {
      case Stmt::VarDecl: {
        Value* init = nullptr;
        if (s->val) {
          auto r = genExpr(s->val.get());
          init = convert(r.first, r.second, s->ty);
        }
        auto* slot = b_.CreateAlloca(llvmType(s->ty), nullptr, s->name);
        if (init) b_.CreateStore(init, slot);
        else if (s->ty == "int" || s->ty == "bool") b_.CreateStore(ConstantInt::get(i64_, 0), slot);
        else b_.CreateStore(ConstantPointerNull::get(cast<PointerType>(ptr_)), slot);
        scopes_.back()[s->name] = {s->ty, slot};
        break;
      }
      case Stmt::Assign: {
        auto* addr = genLValue(s->target.get());
        auto r = genExpr(s->val.get());
        Value* c = convert(r.first, r.second, addr->second);
        b_.CreateStore(c, addr->first);
        break;
      }
      case Stmt::ExprStmt: genExpr(s->val.get()); break;
      case Stmt::IfS: {
        auto r = genExpr(s->val.get());
        Value* cond = toBool(r.first, r.second);
        auto* thenBB = BasicBlock::Create(ctx_, "then", curFn_);
        auto* elseBB = BasicBlock::Create(ctx_, "else", curFn_);
        auto* mergeBB = BasicBlock::Create(ctx_, "merge", curFn_);
        b_.CreateCondBr(cond, thenBB, elseBB);
        b_.SetInsertPoint(thenBB);
        for (auto& x : s->body) genStmt(x.get());
        if (!b_.GetInsertBlock()->getTerminator()) b_.CreateBr(mergeBB);
        b_.SetInsertPoint(elseBB);
        for (auto& x : s->alt) genStmt(x.get());
        if (!b_.GetInsertBlock()->getTerminator()) b_.CreateBr(mergeBB);
        b_.SetInsertPoint(mergeBB);
        break;
      }
      case Stmt::WhileS: {
        auto* condBB = BasicBlock::Create(ctx_, "wcond", curFn_);
        auto* bodyBB = BasicBlock::Create(ctx_, "wbody", curFn_);
        auto* endBB = BasicBlock::Create(ctx_, "wend", curFn_);
        b_.CreateBr(condBB);
        b_.SetInsertPoint(condBB);
        auto r = genExpr(s->val.get());
        b_.CreateCondBr(toBool(r.first, r.second), bodyBB, endBB);
        b_.SetInsertPoint(bodyBB);
        for (auto& x : s->body) genStmt(x.get());
        if (!b_.GetInsertBlock()->getTerminator()) b_.CreateBr(condBB);
        b_.SetInsertPoint(endBB);
        break;
      }
      case Stmt::ForS: {
        if (s->init) genStmt(s->init.get());
        auto* condBB = BasicBlock::Create(ctx_, "fcond", curFn_);
        auto* bodyBB = BasicBlock::Create(ctx_, "fbody", curFn_);
        auto* stepBB = BasicBlock::Create(ctx_, "fstep", curFn_);
        auto* endBB = BasicBlock::Create(ctx_, "fend", curFn_);
        b_.CreateBr(condBB);
        b_.SetInsertPoint(condBB);
        auto r = genExpr(s->val.get());
        b_.CreateCondBr(toBool(r.first, r.second), bodyBB, endBB);
        b_.SetInsertPoint(bodyBB);
        for (auto& x : s->body) genStmt(x.get());
        if (!b_.GetInsertBlock()->getTerminator()) b_.CreateBr(stepBB);
        b_.SetInsertPoint(stepBB);
        if (s->step) genExpr(s->step.get());
        b_.CreateBr(condBB);
        b_.SetInsertPoint(endBB);
        break;
      }
      case Stmt::ReturnS: {
        if (s->val) {
          auto r = genExpr(s->val.get());
          Type* rt = curFn_->getReturnType();
          if (rt->isVoidTy()) b_.CreateRetVoid();
          else b_.CreateRet(convert(r.first, r.second, rt->isPointerTy() ? std::string("ptr") : std::string("int")));
        } else b_.CreateRetVoid();
        auto* dead = BasicBlock::Create(ctx_, "dead", curFn_);
        b_.SetInsertPoint(dead);
        break;
      }
      case Stmt::BlockS:
        for (auto& x : s->body) genStmt(x.get());
        break;
    }
  }

  std::pair<Value*,std::string>* genLValue(Expr* e) {
    if (e->k == Expr::Var) {
      std::string t;
      Value* a = lookupVar(e->text, t);
      if (!a && curClass_) {
        auto fi = curClass_->fieldIndex.find(e->text);
        if (fi != curClass_->fieldIndex.end()) {
          const std::string& ft = curClass_->allFields[fi->second].first;
          Value* gep = b_.CreateStructGEP(curClass_->structTy, thisVal_, fi->second + 1);
          return new std::pair<Value*,std::string>(gep, ft);
        }
      }
      if (!a) throw std::runtime_error("未定义的变量: " + e->text);
      return new std::pair<Value*,std::string>(a, t);
    }
    if (e->k == Expr::FieldE) {
      auto r = genExpr(e->a.get());
      auto it = classes_.find(r.second);
      if (it == classes_.end()) throw std::runtime_error("类型 " + r.second + " 没有字段 " + e->text);
      auto fi = it->second.fieldIndex.find(e->text);
      if (fi == it->second.fieldIndex.end()) throw std::runtime_error("类型 " + r.second + " 没有字段 " + e->text);
      const std::string& ft = it->second.allFields[fi->second].first;
      Value* gep = b_.CreateStructGEP(it->second.structTy, r.first, fi->second + 1);
      return new std::pair<Value*,std::string>(gep, ft);
    }
    throw std::runtime_error("非法的赋值目标");
  }

  std::pair<Value*,std::string> genExpr(Expr* e) {
    switch (e->k) {
      case Expr::LitInt: return {ConstantInt::get(i64_, e->num), "int"};
      case Expr::LitBool: return {ConstantInt::get(i64_, e->bval ? 1 : 0), "bool"};
      case Expr::LitNull: return {ConstantPointerNull::get(cast<PointerType>(ptr_)), "null"};
      case Expr::LitStr: {
        Value* s = b_.CreateGlobalString(e->text, ".str");
        return {s, "string"};
      }
      case Expr::ThisE: return {thisVal_, curClass_ ? curClass_->name : std::string("null")};
      case Expr::Var: {
        std::string t;
        Value* a = lookupVar(e->text, t);
        if (!a && curClass_) {
          auto fi = curClass_->fieldIndex.find(e->text);
          if (fi != curClass_->fieldIndex.end()) {
            const std::string& ft = curClass_->allFields[fi->second].first;
            Value* gep = b_.CreateStructGEP(curClass_->structTy, thisVal_, fi->second + 1);
            return {b_.CreateLoad(fieldType(ft), gep), ft};
          }
        }
        if (!a) throw std::runtime_error("未定义的变量: " + e->text);
        return {b_.CreateLoad(llvmType(t), a), t};
      }
      case Expr::NewE: {
        auto it = classes_.find(e->ty);
        if (it == classes_.end()) throw std::runtime_error("未知的类: " + e->ty);
        ClassInfo& ci = it->second;
        Value* size = ConstantInt::get(i64_, (int64_t)(ci.allFields.size() + 1) * 8);
        Value* mem = b_.CreateCall(fnCalloc_, {size, ConstantInt::get(i64_, 1)});
        b_.CreateStore(ci.vtable, b_.CreateStructGEP(ci.structTy, mem, 0));
        return {mem, ci.name};
      }
      case Expr::NewVector: {
        Value* v = b_.CreateCall(fnVecNew_, {});
        return {v, "vector<" + e->ty + ">"};
      }
      case Expr::FieldE: {
        auto* p = genLValue(e);
        Value* v = b_.CreateLoad(fieldType(p->second), p->first);
        return {v, p->second};
      }
      case Expr::MethodE: return genMethod(e);
      case Expr::IndexE: return genIndex(e);
      case Expr::CallE: return genCall(e);
      case Expr::Un: {
        auto r = genExpr(e->a.get());
        if (e->text == "-") return {b_.CreateNeg(r.first), r.second};
        return {b_.CreateNot(toBool(r.first, r.second)), "bool"};
      }
      case Expr::Bin: return genBin(e);
    }
    throw std::runtime_error("未知的表达式");
  }

  std::pair<Value*,std::string> genBin(Expr* e) {
    auto ra = genExpr(e->a.get());
    auto rc = genExpr(e->b.get());
    Value* a = ra.first; Value* c = rc.first;
    const std::string& ta = ra.second;
    const std::string& tc = rc.second;
    const std::string& op = e->text;
    if (ta == "string" && tc == "string") {
      if (op == "+") return {b_.CreateCall(fnStrConcat_, {a, c}), "string"};
      if (op == "==") return {b_.CreateICmpNE(b_.CreateCall(fnStrEq_, {a, c}), ConstantInt::get(i64_, 0)), "bool"};
      if (op == "!=") return {b_.CreateICmpEQ(b_.CreateCall(fnStrEq_, {a, c}), ConstantInt::get(i64_, 0)), "bool"};
    }
    if (op == "+") return {b_.CreateAdd(a, c), "int"};
    if (op == "-") return {b_.CreateSub(a, c), "int"};
    if (op == "*") return {b_.CreateMul(a, c), "int"};
    if (op == "/") return {b_.CreateSDiv(a, c), "int"};
    if (op == "%") return {b_.CreateSRem(a, c), "int"};
    if (op == "<") return {b_.CreateICmpSLT(a, c), "bool"};
    if (op == ">") return {b_.CreateICmpSGT(a, c), "bool"};
    if (op == "<=") return {b_.CreateICmpSLE(a, c), "bool"};
    if (op == ">=") return {b_.CreateICmpSGE(a, c), "bool"};
    if (op == "==") return {b_.CreateICmpEQ(a, c), "bool"};
    if (op == "!=") return {b_.CreateICmpNE(a, c), "bool"};
    if (op == "&&") return {b_.CreateAnd(toBool(a, ta), toBool(c, tc)), "bool"};
    if (op == "||") return {b_.CreateOr(toBool(a, ta), toBool(c, tc)), "bool"};
    throw std::runtime_error("不支持的运算符: " + op);
  }

  std::pair<Value*,std::string> genMethod(Expr* e) {
    auto r = genExpr(e->a.get());
    Value* obj = r.first;
    const std::string& ot = r.second;
    if (ot.rfind("vector<", 0) == 0) {
      std::string elem = ot.substr(7, ot.size() - 8);
      if (e->text == "push") {
        auto rv = genExpr(e->args[0].get());
        b_.CreateCall(fnVecPush_, {obj, boxIfScalar(rv.first, rv.second)});
        return {Constant::getNullValue(ptr_), "void"};
      }
      if (e->text == "get" || e->text == "at") {
        auto ri = genExpr(e->args[0].get());
        Value* v = b_.CreateCall(fnVecGet_, {obj, castToI64(ri.first, ri.second)});
        return {unboxIfScalar(v, elem), elem};
      }
      if (e->text == "set") {
        auto ri = genExpr(e->args[0].get());
        auto rv = genExpr(e->args[1].get());
        b_.CreateCall(fnVecSet_, {obj, castToI64(ri.first, ri.second), boxIfScalar(rv.first, rv.second)});
        return {Constant::getNullValue(ptr_), "void"};
      }
      if (e->text == "len" || e->text == "size") return {b_.CreateCall(fnVecLen_, {obj}), "int"};
      throw std::runtime_error("vector 没有方法: " + e->text);
    }
    auto it = classes_.find(ot);
    if (it == classes_.end()) throw std::runtime_error("类型 " + ot + " 没有方法 " + e->text);
    ClassInfo& ci = it->second;
    auto sit = ci.slot.find(e->text);
    if (sit == ci.slot.end()) throw std::runtime_error("类 " + ot + " 没有方法 " + e->text);
    auto& sig = ci.sig[e->text];
    Value* vt = b_.CreateLoad(ptr_, b_.CreateStructGEP(ci.structTy, obj, 0));
    Value* fp = b_.CreateLoad(ptr_, b_.CreateGEP(ptr_, vt, {ConstantInt::get(i32_, sit->second)}));
    std::vector<Type*> ptypes; ptypes.push_back(ptr_);
    for (auto& pt : sig.second) ptypes.push_back(llvmType(pt));
    auto* ft = FunctionType::get(llvmType(sig.first), ptypes, false);
    std::vector<Value*> args; args.push_back(obj);
    for (size_t i = 0; i < e->args.size(); i++) {
      auto rv = genExpr(e->args[i].get());
      args.push_back(convert(rv.first, rv.second, sig.second[i]));
    }
    Value* rr = b_.CreateCall(ft, fp, args);
    return {rr, sig.first};
  }

  std::pair<Value*,std::string> genIndex(Expr* e) {
    auto r = genExpr(e->a.get());
    auto ri = genExpr(e->b.get());
    if (r.second.rfind("vector<", 0) == 0) {
      std::string elem = r.second.substr(7, r.second.size() - 8);
      Value* v = b_.CreateCall(fnVecGet_, {r.first, castToI64(ri.first, ri.second)});
      return {unboxIfScalar(v, elem), elem};
    }
    throw std::runtime_error("类型 " + r.second + " 不支持下标");
  }

  std::pair<Value*,std::string> genCall(Expr* e) {
    if (e->a->k == Expr::Var && e->a->text == "print") {
      for (auto& arg : e->args) {
        auto r = genExpr(arg.get());
        if (r.second == "int") b_.CreateCall(fnPrintInt_, {r.first});
        else if (r.second == "bool") b_.CreateCall(fnPrintBool_, {r.first});
        else if (r.second == "string") b_.CreateCall(fnPrintStr_, {r.first});
        else b_.CreateCall(fnPrintObj_, {r.first});
      }
      return {Constant::getNullValue(ptr_), "void"};
    }
    throw std::runtime_error("不支持的函数调用");
  }

  Value* lookupVar(const std::string& name, std::string& type) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) { type = f->second.first; return f->second.second; }
    }
    return nullptr;
  }

  Value* toBool(Value* v, const std::string&) {
    if (v->getType()->isPointerTy())
      return b_.CreateICmpNE(v, ConstantPointerNull::get(cast<PointerType>(ptr_)));
    return b_.CreateICmpNE(v, ConstantInt::get(i64_, 0));
  }

  Value* castToI64(Value* v, const std::string&) {
    if (v->getType()->isPointerTy()) return b_.CreateCall(fnUnboxInt_, {v});
    return v;
  }

  Value* boxIfScalar(Value* v, const std::string& t) {
    if (isScalar(t) || v->getType()->isIntegerTy()) {
      Value* x = v;
      if (x->getType()->isIntegerTy(1)) x = b_.CreateZExt(x, i64_);
      return b_.CreateCall(fnBoxInt_, {x});
    }
    return v;
  }

  Value* unboxIfScalar(Value* r, const std::string& elem) {
    if (elem == "int" || elem == "bool") return b_.CreateCall(fnUnboxInt_, {r});
    return r;
  }

  Value* convert(Value* v, const std::string&, const std::string& to) {
    if (to == "ptr") return v;
    Type* want = llvmType(to);
    if (v->getType() == want) return v;
    if (v->getType()->isPointerTy() && want->isIntegerTy()) return b_.CreatePtrToInt(v, i64_);
    if (v->getType()->isIntegerTy() && want->isPointerTy()) return b_.CreateIntToPtr(v, ptr_);
    if (v->getType()->isIntegerTy(1) && want->isIntegerTy(64)) return b_.CreateZExt(v, i64_);
    if (v->getType()->isIntegerTy(64) && want->isIntegerTy(1)) return b_.CreateICmpNE(v, ConstantInt::get(i64_, 0));
    return v;
  }
};

}  // namespace

int runCodeGen(const Program& prog, const std::string& outIr, const std::string& outObj) {
  try {
    CodeGen cg(prog);
    cg.run(outIr, outObj);
    return 0;
  } catch (const std::exception& e) {
    llvm::errs() << "编译错误: " << e.what() << "\n";
    return 1;
  }
}
