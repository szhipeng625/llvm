path = 'irgen/irgen.cpp'
src = open(path, encoding='utf-8').read()

# 1. 把原 genForIterable 改为 genForIterableFromSlot：接收已求值的 seq 槽位与汇合块
old_head = '''void IRGen::genForIterable(const For *s) {
  Function *fn = curFn_;
  Value *src = genExpr(s->iterable.get());
  Value *seq = abi_.newSlot(B_, "for.seq");
  abi_.copySlot(B_, seq, src);   // 绑定一份,后面重新赋值变量不影响它
  BasicBlock *preBB = B_.GetInsertBlock();'''
new_head = '''void IRGen::genForIterableFromSlot(const For *s, Value *seq, BasicBlock *mergeBB) {
  Function *fn = curFn_;
  BasicBlock *preBB = B_.GetInsertBlock();'''
assert old_head in src, 'iter head anchor not found'
src = src.replace(old_head, new_head, 1)

# 2. 该函数末尾 exitBB 后跳到 mergeBB
old_tail = '''  Value *next = B_.CreateAdd(i, ConstantInt::get(abi_.i64Ty(), 1), "next");
  i->addIncoming(next, latchBB);
  B_.CreateBr(condBB);
  B_.SetInsertPoint(exitBB);
}
// for i in range(a, b, step) 降成带 PHI 的计数循环,不构造任何迭代器对象。'''
new_tail = '''  Value *next = B_.CreateAdd(i, ConstantInt::get(abi_.i64Ty(), 1), "next");
  i->addIncoming(next, latchBB);
  B_.CreateBr(condBB);
  B_.SetInsertPoint(exitBB);
  B_.CreateBr(mergeBB);
}
// 迭代器协议:for x in <实例> —— 每轮调用 next(),返回 None 时终止循环。
// 与 Python 的迭代协议同构,只是方法名固定为 next(语言的属性调用走统一分派)。
void IRGen::genForIteratorProtocol(const For *s, Value *itObj, BasicBlock *mergeBB) {
  Function *fn = curFn_;
  BasicBlock *condBB = BasicBlock::Create(Ctx_, "ip.cond", fn);
  BasicBlock *bodyBB = BasicBlock::Create(Ctx_, "ip.body", fn);
  BasicBlock *exitBB = BasicBlock::Create(Ctx_, "ip.exit", fn);
  B_.CreateBr(condBB);
  B_.SetInsertPoint(condBB);
  // item = it.next() —— 空参数的方法调用
  GlobalVariable *mname = B_.CreateGlobalString("next");
  Value *nullArgs = ConstantPointerNull::get(cast<PointerType>(abi_.ptrTy()));
  Value *item = abi_.callN(
      B_, "py_call_method",
      {abi_.ptrTy(), abi_.ptrTy(), abi_.i64Ty(), abi_.ptrTy(), abi_.i64Ty()},
      {itObj, mname, B_.getInt64(4), nullArgs, B_.getInt64(0)});
  // item 是槽位指针,取 tag 判 None
  Value *tag = abi_.loadTag(B_, item);
  Value *isNone = B_.CreateICmpEQ(tag, ConstantInt::get(abi_.i32Ty(), PY_NULL), "is.none");
  B_.CreateCondBr(isNone, exitBB, bodyBB);
  B_.SetInsertPoint(bodyBB);
  AllocaInst *ivar = lookupVar(s->var);
  if (!ivar) ivar = declareVar(s->var);
  abi_.copySlot(B_, ivar, item);
  // 迭代器循环没有 latch:next 调用本身就在 cond 里,continue 直接回 cond
  loops_.push_back({condBB, exitBB});
  genBlock(s->body);
  loops_.pop_back();
  if (!B_.GetInsertBlock()->getTerminator()) B_.CreateBr(condBB);
  B_.SetInsertPoint(exitBB);
  B_.CreateBr(mergeBB);
}
// for i in range(a, b, step) 降成带 PHI 的计数循环,不构造任何迭代器对象。'''
assert old_tail in src, 'iter tail anchor not found'
src = src.replace(old_tail, new_tail, 1)

# 3. genFor 分发处:probe 先求值存槽位再分支(修正:probe 不能在分支前重复求值)
old_probe = '''  // 迭代器协议:for x in <实例> —— 每轮调 next(),拿到 None 时终止
  Value *probe = genExpr(s->iterable.get());
  Value *tag = abi_.loadTag(B_, probe);
  Value *isInst = B_.CreateICmpEQ(tag, ConstantInt::get(abi_.i32Ty(), PY_INSTANCE), "is.inst");
  Function *fn = curFn_;
  BasicBlock *iterBB = BasicBlock::Create(Ctx_, "for.iterproto", fn);
  BasicBlock *countBB = BasicBlock::Create(Ctx_, "for.count", fn);
  BasicBlock *mergeBB = BasicBlock::Create(Ctx_, "for.done", fn);
  // 修正:probe 在两个分支里都要用,先存到槽位避免重复求值
  Value *probeSlot = abi_.newSlot(B_, "for.probe");
  abi_.copySlot(B_, probeSlot, probe);
  B_.CreateCondBr(isInst, iterBB, countBB);'''
new_probe = '''  // 迭代器协议:for x in <实例> —— 每轮调 next(),拿到 None 时终止
  Value *probe = genExpr(s->iterable.get());
  Value *probeSlot = abi_.newSlot(B_, "for.probe");
  abi_.copySlot(B_, probeSlot, probe);
  Value *tag = abi_.loadTag(B_, probeSlot);
  Value *isInst = B_.CreateICmpEQ(tag, ConstantInt::get(abi_.i32Ty(), PY_INSTANCE), "is.inst");
  Function *fn = curFn_;
  BasicBlock *iterBB = BasicBlock::Create(Ctx_, "for.iterproto", fn);
  BasicBlock *countBB = BasicBlock::Create(Ctx_, "for.count", fn);
  BasicBlock *mergeBB = BasicBlock::Create(Ctx_, "for.done", fn);
  B_.CreateCondBr(isInst, iterBB, countBB);'''
assert old_probe in src, 'probe anchor not found'
src = src.replace(old_probe, new_probe, 1)

open(path, 'w', encoding='utf-8').write(src)
print('irgen.cpp iter protocol applied')

# 4. 头文件声明更新
hpath = 'irgen/irgen.h'
hsrc = open(hpath, encoding='utf-8').read()
old_h = '  void genForIterable(const For *s);   // 遍历 list / str / dict'
new_h = '''  void genForIterableFromSlot(const For *s, llvm::Value *seq,
                              llvm::BasicBlock *mergeBB);   // 遍历 list / str / dict
  void genForIteratorProtocol(const For *s, llvm::Value *itObj,
                              llvm::BasicBlock *mergeBB);   // 迭代器协议:实例 next()'''
assert old_h in hsrc, 'header anchor not found'
open(hpath, 'w', encoding='utf-8').write(hsrc.replace(old_h, new_h, 1))
print('irgen.h updated')
