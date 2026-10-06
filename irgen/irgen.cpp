#include "irgen.h"

#include "frontend/lexer.h"  // SourceError

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

#include <cctype>
#include <string>

using namespace llvm;

namespace pylite {

namespace {

// 把运算符映射到运行时函数名。返回 nullptr 表示不是简单的一元/二元运算。
const char *runtimeForBinOp(const std::string &op) {
  if (op == "+")  return "py_add";
  if (op == "-")  return "py_sub";
  if (op == "*")  return "py_mul";
  if (op == "/")  return "py_div";
  if (op == "//") return "py_floordiv";
  if (op == "%")  return "py_mod";
  if (op == "**") return "py_pow";
  if (op == "==") return "py_eq";
  if (op == "!=") return "py_ne";
  if (op == "<")  return "py_lt";
  if (op == "<=") return "py_le";
  if (op == ">")  return "py_gt";
  if (op == ">=") return "py_ge";
  return nullptr;
}

// 模块名/函数名里的非字母数字字符换成下划线,保证生成合法的 C 符号
std::string sanitize(const std::string &s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    r += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
  }
  if (r.empty() || std::isdigit(static_cast<unsigned char>(r[0]))) r.insert(r.begin(), '_');
  return r;
}

}  // namespace

// 注意这里必须写 llvm::Module。在 namespace pylite 里,不带限定的 `Module`
// 指的是 pylite::Module(AST 根节点),而不是 LLVM 的模块。
IRGen::IRGen(llvm::Module &M, std::string moduleName)
    : M_(M), Ctx_(M.getContext()), abi_(M), moduleName_(std::move(moduleName)),
      B_(M.getContext()) {}

std::string IRGen::symbolFor(const std::string &funcName) const {
  return "pylite_" + sanitize(moduleName_) + "_" + sanitize(funcName);
}

[[noreturn]] static void irError(int line, int col, const std::string &msg) {
  throw SourceError(line, col, "<irgen>:" + std::to_string(line) + ":" +
                                  std::to_string(col) + ": " + msg);
}

// ---------------------------------------------------------------------------
// 作用域
//
// Python 的变量是**函数作用域**的,不是块作用域 —— if/for/while 里赋值的
// 变量在外面也能看见。所以这里一个函数只有一个作用域,不随块进出压栈。
// 这既是正确语义,也省掉一堆边界情况。
// ---------------------------------------------------------------------------

AllocaInst *IRGen::lookupVar(const std::string &name) const {
  if (scopes_.empty()) return nullptr;
  auto it = scopes_.back().find(name);
  return it == scopes_.back().end() ? nullptr : it->second;
}

AllocaInst *IRGen::declareVar(const std::string &name) {
  // alloca 统一放在入口块的最前面。放在当前插入点会出问题:
  // 循环体里声明的变量每轮都会重新 alloca 一次,栈会一直涨。
  BasicBlock &entry = curFn_->getEntryBlock();
  IRBuilder<> tmp(&entry, entry.begin());
  auto *slot = tmp.CreateAlloca(abi_.valueTy(), nullptr, name);
  abi_.storeNone(tmp, slot);  // 默认 None,避免读到未初始化内存
  scopes_.back()[name] = slot;
  return slot;
}

// ---------------------------------------------------------------------------
// 表达式
//
// 全部返回"指向 PyValue 槽位的指针"。
// ---------------------------------------------------------------------------

Value *IRGen::genExpr(const Expr *e) {
  // --- 字面量 ---
  if (auto *x = dynamic_cast<const IntLit *>(e)) {
    Value *s = abi_.newSlot(B_, "int");
    abi_.storeInt(B_, s, x->value);
    return s;
  }
  if (auto *x = dynamic_cast<const FloatLit *>(e)) {
    Value *s = abi_.newSlot(B_, "flt");
    abi_.storeFloat(B_, s, x->value);
    return s;
  }
  if (auto *x = dynamic_cast<const BoolLit *>(e)) {
    Value *s = abi_.newSlot(B_, "bool");
    abi_.storeBool(B_, s, x->value);
    return s;
  }
  if (auto *x = dynamic_cast<const NoneLit *>(e)) {
    Value *s = abi_.newSlot(B_, "none");
    abi_.storeNone(B_, s);
    return s;
  }
  if (auto *x = dynamic_cast<const StringLit *>(e)) {
    // 字符串要等 Stage 7 的运行时支持;先给出明确的报错而不是静默出错
    irError(x->line, x->col, "字符串字面量尚未支持(计划在 Stage 7 实现)");
  }

  // --- 名字 ---
  if (auto *x = dynamic_cast<const Name *>(e)) {
    AllocaInst *slot = lookupVar(x->id);
    if (!slot) {
      // 也可能是"把一个函数当值用",这里同样不支持
      irError(x->line, x->col, "使用了未定义的变量 '" + x->id + "'");
    }
    return slot;
  }

  // --- 运算 ---
  if (auto *x = dynamic_cast<const BinOp *>(e)) return genBinOp(x);
  if (auto *x = dynamic_cast<const UnaryOp *>(e)) return genUnaryOp(x);
  if (auto *x = dynamic_cast<const Call *>(e)) return genCall(x);
  if (auto *x = dynamic_cast<const TupleLit *>(e)) return genTupleLit(x);
  if (auto *x = dynamic_cast<const ListLit *>(e)) return genListLit(x);

  if (auto *x = dynamic_cast<const RangeExpr *>(e)) {
    irError(x->line, x->col, "range() 只能直接用在 for 的迭代对象位置");
  }
  if (auto *x = dynamic_cast<const InputExpr *>(e)) return genInput(x);
  if (auto *x = dynamic_cast<const DictLit *>(e)) {
    irError(x->line, x->col, "字典字面量尚未支持(计划在 Stage 6 实现)");
  }
  if (auto *x = dynamic_cast<const Subscript *>(e)) {
    irError(x->line, x->col, "下标访问尚未支持(计划在 Stage 6 实现)");
  }
  if (auto *x = dynamic_cast<const SliceExpr *>(e)) {
    irError(x->line, x->col, "切片尚未支持(计划在 Stage 6 实现)");
  }
  if (auto *x = dynamic_cast<const Attribute *>(e)) {
    irError(x->line, x->col, "属性访问尚未支持(计划在 Stage 7 实现)");
  }

  irError(e->line, e->col, "无法生成代码:不认识的表达式节点");
}

// 构造一个真正的元组值(堆上分配)。用于 `return a, b` 这类场景。
// 这里逐个求值后立即写入新数组是安全的 —— 数组是全新的,不存在
// `a, b = b, a` 那种"写目标会覆盖读取来源"的问题。
Value *IRGen::genTupleLit(const TupleLit *e) {
  const size_t n = e->items.size();
  if (n == 0) {
    // 空元组:传个空指针,运行时不读它
    return abi_.callPtrI64(B_, "py_tuple_new",
                           ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy())),
                           B_.getInt64(0));
  }

  Value *arr = B_.CreateAlloca(abi_.valueTy(), B_.getInt32(static_cast<uint32_t>(n)),
                               "tuple.vals");
  for (size_t i = 0; i < n; ++i) {
    Value *v = genExpr(e->items[i].get());
    Value *elem = B_.CreateGEP(abi_.valueTy(), arr, B_.getInt64(static_cast<uint64_t>(i)));
    B_.CreateStore(B_.CreateLoad(abi_.valueTy(), v), elem);
  }
  return abi_.callPtrI64(B_, "py_tuple_new", arr, B_.getInt64(static_cast<uint64_t>(n)));
}

Value *IRGen::genListLit(const ListLit *e) {
  irError(e->line, e->col, "列表字面量尚未支持(计划在 Stage 6 实现)");
}

Value *IRGen::genBinOp(const BinOp *e) {
  // and / or 需要短路,单独处理
  if (e->op == "and") return genAndOr(e, true);
  if (e->op == "or")  return genAndOr(e, false);

  const char *fn = runtimeForBinOp(e->op);
  if (!fn) irError(e->line, e->col, "无法生成代码:不支持的运算符 '" + e->op + "'");

  // 左右两侧都求值到槽位。注意求值顺序:先左后右。
  Value *a = genExpr(e->lhs.get());
  Value *b = genExpr(e->rhs.get());
  return abi_.callBinary(B_, fn, a, b);
}

// and / or 必须短路,并且返回的是**操作数本身**而不是布尔值
// (`1 and 2` 是 2,不是 True)。用 PHI 合并两个分支上产生的槽位指针。
Value *IRGen::genAndOr(const BinOp *e, bool isAnd) {
  Function *fn = curFn_;
  Value *a = genExpr(e->lhs.get());
  Value *aTruth = abi_.truthyAsI1(B_, a);
  BasicBlock *entryBB = B_.GetInsertBlock();

  BasicBlock *rhsBB = BasicBlock::Create(Ctx_, isAnd ? "and.rhs" : "or.rhs", fn);
  BasicBlock *endBB = BasicBlock::Create(Ctx_, isAnd ? "and.end" : "or.end", fn);

  // and:左侧为真才求右侧;or:左侧为假才求右侧
  if (isAnd) B_.CreateCondBr(aTruth, rhsBB, endBB);
  else       B_.CreateCondBr(aTruth, endBB, rhsBB);

  B_.SetInsertPoint(rhsBB);
  Value *b = genExpr(e->rhs.get());
  BasicBlock *rhsEndBB = B_.GetInsertBlock();
  B_.CreateBr(endBB);

  B_.SetInsertPoint(endBB);
  PHINode *phi = B_.CreatePHI(abi_.ptrTy(), 2, isAnd ? "and.val" : "or.val");
  phi->addIncoming(a, entryBB);   // 短路时结果就是左侧
  phi->addIncoming(b, rhsEndBB);  // 未短路时结果取右侧
  return phi;
}

Value *IRGen::genUnaryOp(const UnaryOp *e) {
  if (e->op == "-") {
    return abi_.callUnary(B_, "py_neg", genExpr(e->operand.get()));
  }
  if (e->op == "not") {
    return abi_.callUnary(B_, "py_not", genExpr(e->operand.get()));
  }
  irError(e->line, e->col, "无法生成代码:不支持的一元运算符 '" + e->op + "'");
}

Value *IRGen::genCall(const Call *e) {
  auto *name = dynamic_cast<const Name *>(e->callee.get());
  if (!name) {
    irError(e->line, e->col, "只支持直接调用具名函数(方法调用计划在 Stage 7 实现)");
  }

  if (name->id == "print") return genPrint(e);

  const std::string sym = symbolFor(name->id);
  Function *f = M_.getFunction(sym);
  if (!f) {
    irError(e->line, e->col, "调用了未定义的函数 '" + name->id + "'");
  }

  // 参数个数:第一个是 sret 指针,其余与用户参数一一对应
  const size_t expected = f->arg_size() - 1;
  if (e->args.size() != expected) {
    irError(e->line, e->col,
            "函数 '" + name->id + "' 需要 " + std::to_string(expected) +
                " 个参数,给了 " + std::to_string(e->args.size()) + " 个");
  }

  Value *ret = abi_.newSlot(B_, name->id + ".ret");
  std::vector<Value *> callArgs;
  callArgs.reserve(e->args.size() + 1);
  callArgs.push_back(ret);
  for (const auto &a : e->args) callArgs.push_back(genExpr(a.get()));

  CallInst *call = B_.CreateCall(f, callArgs);
  call->addParamAttr(0, abi_.sretAttr());
  return ret;
}

// input(int, int) —— 特殊形式。
//
// 按给定的类型列表逐个从 stdin 读一个值,打包成元组返回。
// 之所以做成"返回元组"而不是别的形式:`a, b = input(int, int)` 于是和
// `a, b = f()`、`a, b = b, a` 走完全同一条解包路径,不必为它单开特例。
Value *IRGen::genInput(const InputExpr *e) {
  const size_t n = e->kinds.size();

  // 只读一个值时直接返回它本身,不包成 1 元组。
  // 否则 `x = input(float)` 会让 x 拿到一个元组而不是那个浮点数。
  auto readerFor = [&](TypeName t) -> const char * {
    switch (t) {
      case TypeName::Int:   return "py_read_int";
      case TypeName::Float: return "py_read_float";
      case TypeName::Bool:  return "py_read_bool";
      case TypeName::Str:   return "py_read_str";
      default: return nullptr;
    }
  };
  if (n == 1) {
    const char *fn = readerFor(e->kinds[0]);
    if (!fn) irError(e->line, e->col, "input() 的类型参数必须是 int/float/bool/str");
    return abi_.callNullary(B_, fn);
  }

  // 把各次读取的结果堆进一块连续内存,再交给 py_tuple_new
  Value *arr = B_.CreateAlloca(abi_.valueTy(), B_.getInt32(static_cast<uint32_t>(n)),
                               "input.vals");

  for (size_t i = 0; i < n; ++i) {
    const char *fn = readerFor(e->kinds[i]);
    if (!fn) irError(e->line, e->col, "input() 的类型参数必须是 int/float/bool/str");
    Value *v = abi_.callNullary(B_, fn);
    // 单索引 GEP,语义是"数组第 i 个元素"(见 genPrint 里的说明)
    Value *elem = B_.CreateGEP(abi_.valueTy(), arr, B_.getInt64(static_cast<uint64_t>(i)));
    B_.CreateStore(B_.CreateLoad(abi_.valueTy(), v), elem);
  }

  return abi_.callPtrI64(B_, "py_tuple_new", arr, B_.getInt64(static_cast<uint64_t>(n)));
}

// print(a, b, ...) —— 内建。把各参数求值后放进一个 PyValue 数组,
// 把数组指针交给运行时。数组是内存布局,不受"聚合体不按值传"的限制。
Value *IRGen::genPrint(const Call *e) {
  const size_t n = e->args.size();

  Value *arr = nullptr;
  if (n == 0) {
    arr = ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy()));
  } else {
    arr = B_.CreateAlloca(abi_.valueTy(), B_.getInt32(static_cast<uint32_t>(n)),
                          "print.args");
    for (size_t i = 0; i < n; ++i) {
      Value *v = genExpr(e->args[i].get());
      // 只能用**单索引**的 GEP。写成 {0, i} 是错的:源类型 %PyValue 是结构体,
      // 第二个索引会被解释成"结构体的第几个字段",而不是数组的第几个元素 ——
      // i=1 时算出来是 arr+8(写进了 args[0] 的 payload),i=2 直接字段越界。
      // 单索引形式按 sizeof(%PyValue) 步进,正是数组取址要的语义。
      Value *elem = B_.CreateGEP(abi_.valueTy(), arr,
                                 B_.getInt64(static_cast<uint64_t>(i)), "arg.ptr");
      B_.CreateStore(B_.CreateLoad(abi_.valueTy(), v), elem);
    }
  }

  abi_.callPrint(B_, arr, static_cast<int64_t>(n));

  // print 的返回值是 None,这样 `x = print(...)` 也能有个确定的结果
  Value *res = abi_.newSlot(B_, "print.ret");
  abi_.storeNone(B_, res);
  return res;
}

// ---------------------------------------------------------------------------
// 语句
// ---------------------------------------------------------------------------

void IRGen::genBlock(const std::vector<StmtPtr> &body) {
  for (const auto &s : body) {
    // 前面的语句已经终止了当前块(return/break/continue),后面的代码不可达。
    // 仍然要生成 —— 否则变量声明会缺失,但要让它们落在新的死块里。
    if (B_.GetInsertBlock()->getTerminator()) {
      BasicBlock *dead = BasicBlock::Create(Ctx_, "dead", curFn_);
      B_.SetInsertPoint(dead);
    }
    genStmt(s.get());
  }
}

void IRGen::genStmt(const Stmt *s) {
  if (auto *x = dynamic_cast<const Assign *>(s))    return genAssign(x);
  if (auto *x = dynamic_cast<const AugAssign *>(s)) return genAugAssign(x);
  if (auto *x = dynamic_cast<const If *>(s))        return genIf(x);
  if (auto *x = dynamic_cast<const While *>(s))     return genWhile(x);
  if (auto *x = dynamic_cast<const For *>(s))       return genFor(x);
  if (auto *x = dynamic_cast<const Return *>(s))    return genReturn(x);

  if (dynamic_cast<const Break *>(s)) {
    if (loops_.empty()) irError(s->line, s->col, "break 只能出现在循环里");
    B_.CreateBr(loops_.back().breakBB);
    return;
  }
  if (dynamic_cast<const Continue *>(s)) {
    if (loops_.empty()) irError(s->line, s->col, "continue 只能出现在循环里");
    B_.CreateBr(loops_.back().contBB);
    return;
  }

  if (auto *x = dynamic_cast<const ExprStmt *>(s)) {
    genExpr(x->expr.get());  // 求值即可,丢弃结果
    return;
  }
  if (auto *x = dynamic_cast<const FuncDef *>(s)) {
    // 函数定义在 generate() 的两趟流程里已经处理过了
    (void)x;
    return;
  }

  irError(s->line, s->col, "无法生成代码:不认识的语句节点");
}

void IRGen::genAssign(const Assign *s) {
  const size_t n = s->targets.size();

  // 单目标:`a = expr`
  if (n == 1) {
    Value *v = genExpr(s->value.get());
    auto *name = dynamic_cast<const Name *>(s->targets[0].get());
    if (!name) {
      irError(s->line, s->col, "目前只支持给变量赋值");
    }
    AllocaInst *slot = lookupVar(name->id);
    if (!slot) slot = declareVar(name->id);
    abi_.copySlot(B_, slot, v);
    return;
  }

  // --- 右侧是元组字面量:`a, b = b, a` ---
  //
  // 必须先全部求值并复制进独立临时槽位,再写目标。原因是 genExpr 遇到
  // 变量名时返回的是**变量自己的槽位指针**,不是拷贝 —— 直接拿这些指针
  // 去写目标的话,先写 a 就会把 vals[1] 的来源改掉,交换退化成两次自赋值。
  if (auto *tup = dynamic_cast<const TupleLit *>(s->value.get())) {
    if (tup->items.size() != n) {
      irError(s->line, s->col,
              "赋值目标有 " + std::to_string(n) + " 个,右侧元组有 " +
                  std::to_string(tup->items.size()) + " 个");
    }
    auto vals = evalAll(tup->items);
    std::vector<Value *> temps;
    temps.reserve(vals.size());
    for (Value *v : vals) {
      Value *t = abi_.newSlot(B_, "rhs.tmp");
      abi_.copySlot(B_, t, v);
      temps.push_back(t);
    }
    assignToTargets(s, temps);
    return;
  }

  // --- 其余情况:右侧求值成一个元组,运行时解包 ---
  //
  // 覆盖 `a, b = f()`(函数多返回值)和 `a, b = input(int, int)`。
  // 元素个数不匹配由 py_unpack 在运行时给出明确报错。
  Value *tup = genExpr(s->value.get());
  Value *out = B_.CreateAlloca(abi_.valueTy(), B_.getInt32(static_cast<uint32_t>(n)),
                               "unpack.vals");
  abi_.callUnpack(B_, tup, out, static_cast<int64_t>(n));

  std::vector<Value *> slots;
  slots.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    // out 是 %PyValue 数组,用单索引 GEP 取第 i 个元素
    slots.push_back(
        B_.CreateGEP(abi_.valueTy(), out, B_.getInt64(static_cast<uint64_t>(i))));
  }
  assignToTargets(s, slots);
}

void IRGen::assignToTargets(const Assign *s, const std::vector<Value *> &vals) {
  for (size_t i = 0; i < s->targets.size(); ++i) {
    auto *name = dynamic_cast<const Name *>(s->targets[i].get());
    if (!name) irError(s->line, s->col, "目前只支持给变量赋值");
    AllocaInst *slot = lookupVar(name->id);
    if (!slot) slot = declareVar(name->id);
    abi_.copySlot(B_, slot, vals[i]);
  }
}

std::vector<Value *> IRGen::evalAll(const std::vector<ExprPtr> &items) {
  std::vector<Value *> out;
  out.reserve(items.size());
  for (const auto &it : items) out.push_back(genExpr(it.get()));
  return out;
}

void IRGen::genAugAssign(const AugAssign *s) {
  auto *name = dynamic_cast<const Name *>(s->target.get());
  if (!name) irError(s->line, s->col, "复合赋值目前只支持变量");

  AllocaInst *slot = lookupVar(name->id);
  if (!slot) irError(s->line, s->col, "使用了未定义的变量 '" + name->id + "'");

  const char *fn = runtimeForBinOp(s->op);
  if (!fn) irError(s->line, s->col, "复合赋值不支持运算符 '" + s->op + "'");

  Value *rhs = genExpr(s->value.get());
  Value *result = abi_.callBinary(B_, fn, slot, rhs);
  abi_.copySlot(B_, slot, result);  // 结果写回原变量
}

void IRGen::genIf(const If *s) {
  Function *fn = curFn_;
  BasicBlock *endBB = BasicBlock::Create(Ctx_, "if.end", fn);

  struct Branch {
    const Expr *cond;
    const std::vector<StmtPtr> *body;
  };
  std::vector<Branch> branches;
  branches.push_back({s->cond.get(), &s->body});
  for (const auto &e : s->elifs) branches.push_back({e.first.get(), &e.second});

  for (size_t i = 0; i < branches.size(); ++i) {
    Value *c = abi_.truthyAsI1(B_, genExpr(branches[i].cond));
    BasicBlock *thenBB = BasicBlock::Create(Ctx_, "if.then", fn);

    // 后面还有 elif 或 else 才需要单独的"假"分支;否则假分支直接去 end
    const bool hasMore = (i + 1 < branches.size()) || !s->orelse.empty();
    BasicBlock *falseBB = hasMore ? BasicBlock::Create(Ctx_, "if.next", fn) : endBB;

    B_.CreateCondBr(c, thenBB, falseBB);

    B_.SetInsertPoint(thenBB);
    genBlock(*branches[i].body);
    if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(endBB);

    if (hasMore) B_.SetInsertPoint(falseBB);
  }

  if (!s->orelse.empty()) {
    genBlock(s->orelse);
    if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(endBB);
  }

  B_.SetInsertPoint(endBB);
}

void IRGen::genWhile(const While *s) {
  Function *fn = curFn_;
  BasicBlock *condBB = BasicBlock::Create(Ctx_, "while.cond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(Ctx_, "while.body", fn);
  BasicBlock *exitBB = BasicBlock::Create(Ctx_, "while.exit", fn);

  B_.CreateBr(condBB);

  B_.SetInsertPoint(condBB);
  Value *c = abi_.truthyAsI1(B_, genExpr(s->cond.get()));
  B_.CreateCondBr(c, bodyBB, exitBB);

  B_.SetInsertPoint(bodyBB);
  // continue 在 while 里跳回条件判断(而不是循环末),这里正好是 condBB
  loops_.push_back({condBB, exitBB});
  genBlock(s->body);
  loops_.pop_back();
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(condBB);

  B_.SetInsertPoint(exitBB);
}

void IRGen::genFor(const For *s) {
  // range(...) 直接降成计数循环;其余迭代对象(列表)走通用路径,
  // 计划在 Stage 6 实现。
  if (auto *r = dynamic_cast<const RangeExpr *>(s->iterable.get())) {
    return genForRange(s, r);
  }
  irError(s->line, s->col,
          "只支持 for ... in range(...);遍历列表计划在 Stage 6 实现");
}

// for i in range(a, b, step) 降成带 PHI 的计数循环,不构造任何迭代器对象。
//
//   stepOk: 检查 step != 0
//   cond:   %i = phi [start, stepOk], [next, latch]
//           按 step 的正负选择 i < stop 或 i > stop
//   body:   每轮把 %i 装箱写进循环变量的槽位
//   latch:  %next = i + step, 跳回 cond
void IRGen::genForRange(const For *s, const RangeExpr *r) {
  Function *fn = curFn_;

  // 边界在循环外只求值一次 —— 与 Python 一致(循环途中改 a 不影响次数)
  Value *startSlot = r->start ? genExpr(r->start.get()) : nullptr;
  Value *stopSlot = genExpr(r->stop.get());
  Value *stepSlot = r->step ? genExpr(r->step.get()) : nullptr;

  Value *startI = startSlot ? abi_.callUnaryI64(B_, "py_to_int", startSlot)
                            : ConstantInt::get(abi_.i64Ty(), 0);
  Value *stopI = abi_.callUnaryI64(B_, "py_to_int", stopSlot);
  Value *stepI = stepSlot ? abi_.callUnaryI64(B_, "py_to_int", stepSlot)
                          : ConstantInt::get(abi_.i64Ty(), 1);

  // 步长为 0 会死循环,先拦下来
  BasicBlock *stepOkBB = BasicBlock::Create(Ctx_, "range.stepok", fn);
  BasicBlock *badStepBB = BasicBlock::Create(Ctx_, "range.badstep", fn);
  B_.CreateCondBr(B_.CreateICmpEQ(stepI, ConstantInt::get(abi_.i64Ty(), 0)),
                  badStepBB, stepOkBB);

  B_.SetInsertPoint(badStepBB);
  abi_.emitRuntimeError(B_, "range() 的步长不能为 0");

  B_.SetInsertPoint(stepOkBB);
  BasicBlock *condBB = BasicBlock::Create(Ctx_, "range.cond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(Ctx_, "range.body", fn);
  BasicBlock *latchBB = BasicBlock::Create(Ctx_, "range.latch", fn);
  BasicBlock *exitBB = BasicBlock::Create(Ctx_, "range.exit", fn);
  B_.CreateBr(condBB);

  B_.SetInsertPoint(condBB);
  PHINode *i = B_.CreatePHI(abi_.i64Ty(), 2, "i");
  i->addIncoming(startI, stepOkBB);

  Value *stepPositive = B_.CreateICmpSGT(stepI, ConstantInt::get(abi_.i64Ty(), 0), "steppos");
  Value *lessThan = B_.CreateICmpSLT(i, stopI, "lt");
  Value *greaterThan = B_.CreateICmpSGT(i, stopI, "gt");
  Value *keepGoing = B_.CreateSelect(stepPositive, lessThan, greaterThan, "keep");
  B_.CreateCondBr(keepGoing, bodyBB, exitBB);

  B_.SetInsertPoint(bodyBB);
  AllocaInst *ivar = lookupVar(s->var);
  if (!ivar) ivar = declareVar(s->var);
  abi_.storeI64AsInt(B_, ivar, i);  // 每轮把计数装箱写回循环变量
  loops_.push_back({latchBB, exitBB});
  genBlock(s->body);
  loops_.pop_back();
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(latchBB);

  B_.SetInsertPoint(latchBB);
  Value *next = B_.CreateAdd(i, stepI, "next");
  i->addIncoming(next, latchBB);
  B_.CreateBr(condBB);

  B_.SetInsertPoint(exitBB);
}

void IRGen::genReturn(const Return *s) {
  if (s->value) {
    Value *v = genExpr(s->value.get());
    abi_.copySlot(B_, retSlot_, v);
  } else {
    abi_.storeNone(B_, retSlot_);
  }
  B_.CreateRetVoid();
}

// ---------------------------------------------------------------------------
// 函数
// ---------------------------------------------------------------------------

void IRGen::genFunction(const FuncDef *fd) {
  const std::string sym = symbolFor(fd->name);
  Function *fn = M_.getFunction(sym);
  // generate() 的第一趟已经建好声明

  curFn_ = fn;
  retSlot_ = fn->getArg(0);
  scopes_.emplace_back();

  BasicBlock *entry = BasicBlock::Create(Ctx_, "entry", fn);
  B_.SetInsertPoint(entry);

  // 参数复制进本地的槽位。不能直接用传进来的指针 —— 那是调用方的槽位,
  // 函数体内给参数赋值会改到调用方的变量。
  for (size_t i = 0; i < fd->params.size(); ++i) {
    AllocaInst *slot = declareVar(fd->params[i].first);
    abi_.copySlot(B_, slot, fn->getArg(static_cast<unsigned>(i + 1)));
  }

  // Python 的函数走到末尾会隐式返回 None。入口处先写一次 None,
  // 这样没有 return 的路径也有确定的结果。
  abi_.storeNone(B_, retSlot_);

  genBlock(fd->body);
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateRetVoid();

  scopes_.pop_back();
  curFn_ = nullptr;
  retSlot_ = nullptr;
}

void IRGen::genTopLevel(const Module &ast) {
  const std::string sym = symbolFor("main");
  auto *ft = FunctionType::get(abi_.voidTy(), {}, false);
  auto *fn = Function::Create(ft, Function::ExternalLinkage, sym, M_);

  curFn_ = fn;
  retSlot_ = nullptr;
  scopes_.emplace_back();

  BasicBlock *entry = BasicBlock::Create(Ctx_, "entry", fn);
  B_.SetInsertPoint(entry);

  for (const auto &s : ast.body) {
    if (dynamic_cast<const FuncDef *>(s.get())) continue;  // 已单独生成
    if (B_.GetInsertBlock()->getTerminator()) {
      BasicBlock *dead = BasicBlock::Create(Ctx_, "dead", fn);
      B_.SetInsertPoint(dead);
    }
    genStmt(s.get());
  }
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateRetVoid();

  scopes_.pop_back();
  curFn_ = nullptr;
}

void IRGen::generate(const Module &ast) {
  // 第一趟:登记所有用户函数的签名。
  // 必须先做这一趟,否则调用点无法知道参数个数,而且函数之间的相互调用
  // (包括递归)在定义顺序上会受限制。
  for (const auto &s : ast.body) {
    auto *fd = dynamic_cast<const FuncDef *>(s.get());
    if (!fd) continue;

    const std::string sym = symbolFor(fd->name);
    if (!declaredSyms_.insert(sym).second) {
      irError(fd->line, fd->col, "函数 '" + fd->name + "' 重复定义");
    }
    funcs_.push_back(fd);

    std::vector<Type *> params;
    params.push_back(abi_.ptrTy());  // sret
    for (size_t i = 0; i < fd->params.size(); ++i) params.push_back(abi_.ptrTy());
    auto *ft = FunctionType::get(abi_.voidTy(), params, false);
    auto *fn = Function::Create(ft, Function::ExternalLinkage, sym, M_);
    fn->addParamAttr(0, abi_.sretAttr());
  }

  // 第二趟:填函数体
  for (const auto &s : ast.body) {
    if (auto *fd = dynamic_cast<const FuncDef *>(s.get())) genFunction(fd);
  }

  // 顶层语句
  genTopLevel(ast);
}

}  // namespace pylite
