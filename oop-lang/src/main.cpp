#include "lang.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  std::string in, outObj, outIr;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) outObj = argv[++i];
    else if (a == "--emit-ir" && i + 1 < argc) outIr = argv[++i];
    else if (a[0] != '-') in = a;
  }
  if (in.empty()) {
    std::cerr << "用法: oopc <源文件> [-o 目标文件.o] [--emit-ir 输出.ll]\n";
    return 1;
  }
  std::ifstream f(in);
  if (!f) { std::cerr << "无法打开源文件: " << in << "\n"; return 1; }
  std::stringstream ss;
  ss << f.rdbuf();
  try {
    Lexer lex(ss.str());
    auto toks = lex.run();
    Parser p(std::move(toks));
    Program prog = p.parse();
    return runCodeGen(prog, outIr, outObj);
  } catch (const std::exception& e) {
    std::cerr << "编译失败: " << e.what() << "\n";
    return 1;
  }
}
