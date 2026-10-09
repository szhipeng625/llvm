// 分布式节点测试 —— C++ 宿主程序
// 调用 PyLite 编译 distributed.pys 生成的入口函数
#include <cstdio>

// 由 pylitec 编译 distributed.pys 生成
// 符号名规则: pylite_<模块名>_main
extern "C" void pylite_distributed_main();

int main() {
    pylite_distributed_main();
    return 0;
}
