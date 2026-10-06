// 端到端集成测试:手写 C++,链接 pylitec 编译出来的目标文件。
//
// 这是整个项目的核心诉求所在 —— "在编译期能够引用"。
// 这里的 main.cpp 完全不知道 PyLite 的存在,它只是调用一个 extern "C" 函数,
// 那个函数的实现由 arith.pys 编译而来。
#include "pylite/value.h"

#include <cstdio>

// 由 pylitec 编译 arith.pys 生成(符号名规则:pylite_<模块名>_<函数名>)
extern "C" void pylite_arith_main();

int main() {
  std::printf("--- 调用 PyLite 编译产物 ---\n");
  pylite_arith_main();
  std::printf("--- 返回 ---\n");
  return 0;
}
