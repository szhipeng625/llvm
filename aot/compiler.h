// AOT 后端:LLVM 模块 -> 目标文件 / IR 文本。
#pragma once

#include "llvm/IR/Module.h"

#include <string>

namespace pylite {

// 本项目的目标平台。显式钉死,不靠 LLVM 自动推断 ——
// AOT 与 JIT 两条路必须用同一个 triple/DataLayout,否则 {i32,i64} 的
// 结构体填充会不一致,而且是静默不一致(见 docs/llvm-notes.md R4)。
inline constexpr const char *kTargetTriple = "x86_64-pc-linux-gnu";

// 把模块编译成 COFF 目标文件。失败时返回 false 并填写 error。
// 会先跑一遍优化流水线(默认 O2),把 IRGen 产生的大量 alloca/load/store 清掉。
bool emitObjectFile(llvm::Module &M, const std::string &outPath,
                    std::string *error = nullptr);

// 把模块写成 LLVM IR 文本,方便人工检查 IRGen 的产物。
bool emitIRText(llvm::Module &M, const std::string &outPath,
                std::string *error = nullptr);

// 调用一次即可,进程内幂等。生成代码前必须调用。
void initializeCodeGenTargets();

}  // namespace pylite
