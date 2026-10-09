// 沙箱定位测试 —— C++ 宿主程序
#include <cstdio>

extern "C" void pylite_sandbox_locate_main();

int main() {
    pylite_sandbox_locate_main();
    return 0;
}
