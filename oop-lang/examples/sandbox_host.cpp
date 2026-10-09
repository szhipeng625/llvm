// 沙箱节点测试 —— C++ 宿主程序
#include <cstdio>

extern "C" void pylite_sandbox_node_main();

int main() {
    pylite_sandbox_node_main();
    return 0;
}
