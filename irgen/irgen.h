// AST -> LLVM IR。
//
// 值的表示:每个 PyValue 都住在栈上的一个 alloca 槽位里,genExpr 返回的是
// "指向槽位的指针"而非聚合体本身。这样传给运行时函数时天然就是指针,
// 不需要在每个调用点临时 spill —— 也和 docs/llvm-notes.md 里那条
// "聚合体不跨 ABI 边界按值传"的硬性要求天然吻合。
//
// 生成的符号约定:
//   每个 def  ->  void @pylite_<name>(ptr sret(%PyValue), ptr arg0, ptr arg1, ...)
//   顶层语句  ->  void @pylite_module_main()   (无参数,无返回值)
// 调用方(手写 C++)拿到的是 PyValue*,头文件由 aot/header_emit 生成。
#pragma once

#include "abi.h"
#include "ast.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace pylite {

class IRGen {
 public:
  IRGen(llvm::Module &M, std::string moduleName);

  // 生成整个模块
  void generate(const Module &ast);

  Abi &abi() { return abi_; }
  const llvm::Module &module() const { return M_; }

  // 已生成的用户函数,供头文件生成使用
  const std::vector<const FuncDef *> &userFunctions() const { return funcs_; }

  // 符号名:pylite_<模块名>_<函数名>,模块名做了清洗
  std::string symbolFor(const std::string &funcName) const;

 private:
  // --- 作用域 ---
  llvm::AllocaInst *lookupVar(const std::string &name) const;
  llvm::AllocaInst *declareVar(const std::string &name);

  // --- 表达式:一律返回指向 PyValue 槽位的指针 ---
  llvm::Value *genExpr(const Expr *e);
  llvm::Value *genBinOp(const BinOp *e);
  llvm::Value *genAndOr(const BinOp *e, bool isAnd);   // 短路,带 PHI
  llvm::Value *genUnaryOp(const UnaryOp *e);
  llvm::Value *genCall(const Call *e);
  llvm::Value *genPrint(const Call *e);                // print(...) 内建
  llvm::Value *genLen(const Call *e);                  // len(...) 内建
  llvm::Value *genGcCollect(const Call *e);            // gc_collect() 内建
  llvm::Value *genMethodCall(const Call *e, const Attribute *attr);  // obj.m(...)
  llvm::Value *genInput(const InputExpr *e);           // input(int, int) 特殊形式
  llvm::Value *genListLit(const ListLit *e);
  llvm::Value *genDictLit(const DictLit *e);
  llvm::Value *genTupleLit(const TupleLit *e);
  llvm::Value *genSubscript(const Subscript *e);
  llvm::Value *genSlice(const SliceExpr *e);

  // 把一组表达式依次求值,塞进一块连续的 %PyValue 数组,返回数组指针。
  // genTupleLit / genListLit / genDictLit / genPrint / 方法调用共用。
  //
  // 用一个**数组**而不是逐个传参,是因为数组是内存布局,不受
  // "聚合体不按值跨 ABI 边界"那条限制(见 docs/llvm-notes.md 第 2 节)。
  // 元素个数为 0 时返回空指针,运行时不读它。
  llvm::Value *evalToArray(const std::vector<ExprPtr> &items, const llvm::Twine &name);
  void storeElement(llvm::Value *array, size_t i, llvm::Value *slot);

  // 一个装着 None 的槽位。切片省略边界时用它当"没给"的哨兵。
  llvm::Value *noneSlot();

  // --- 语句 ---
  void genStmt(const Stmt *s);
  void genAssign(const Assign *s);
  void genAugAssign(const AugAssign *s);
  void genIf(const If *s);
  void genWhile(const While *s);
  void genFor(const For *s);
  void genForRange(const For *s, const RangeExpr *r);
  void genForIterable(const For *s);   // 遍历 list / str / dict
  void genReturn(const Return *s);
  void genBlock(const std::vector<StmtPtr> &body);

  // --- 函数 ---
  void genFunction(const FuncDef *fd);
  void genTopLevel(const Module &ast);

  // 先全部求值到临时槽位,再统一写目标。
  // `a, b = b, a` 必须这样,否则第二次求值读到的是已被覆盖的 a。
  std::vector<llvm::Value *> evalAll(const std::vector<ExprPtr> &items);

  // 把若干已知的值按顺序写进赋值目标(目前目标只能是变量)
  void assignToTargets(const Assign *s, const std::vector<llvm::Value *> &vals);

  std::string mangle(const std::string &module, const std::string &fn);

  struct LoopTarget {
    llvm::BasicBlock *contBB = nullptr;
    llvm::BasicBlock *breakBB = nullptr;
  };

  llvm::Module &M_;
  llvm::LLVMContext &Ctx_;
  Abi abi_;
  std::string moduleName_;

  llvm::IRBuilder<> B_;
  llvm::Function *curFn_ = nullptr;
  llvm::Value *retSlot_ = nullptr;  // 当前函数的 sret 槽位

  std::vector<std::map<std::string, llvm::AllocaInst *>> scopes_;
  std::vector<LoopTarget> loops_;
  std::vector<const FuncDef *> funcs_;
  std::set<std::string> declaredSyms_;   // 已生成过声明的用户函数
};

}  // namespace pylite
