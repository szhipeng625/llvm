# PyLite

一个用 LLVM 实现的 Python 风格小语言,能**在编译期编译成目标文件、与 C++ 一起链接**,
同一套前端也驱动 **ORC JIT** 做运行时执行。

```python
a, b = input(int, int)
for i in range(1, a):
    print(i)
```

这行代码不是合法 C++ —— 逗号表达式加 `=` 在语法分析阶段就不成立,模板元编程和
运算符重载都在它之后才生效,救不了。所以必须有一个前端把它降成 LLVM IR。

## 构建

需要 MSYS2 的 UCRT64 环境(不是 MSVCRT 的那个 MinGW):

```bash
# 在 D:\msys64\ucrt64.exe 打开的 shell 里
cd /d/pylite
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build
cd build && ctest --output-on-failure
```

依赖:`mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,llvm,llvm-tools}`,LLVM 22.1.8。

**不要在别的 shell 里构建。** 用自带的 MSVCRT 版 MinGW 与 UCRT64 的 LLVM 混用
会导致链接失败甚至堆静默损坏。判断方式:`g++ --version` 必须显示 16.x 而非 13.x。

## 用法

### 编译成目标文件,与 C++ 链接

```bash
./build/bin/pylitec.exe math.pys -o math.o
```

生成的符号是 `pylite_<模块名>_<函数名>`,顶层语句进 `pylite_<模块名>_main`:

```cpp
extern "C" void pylite_math_main();   // 按 ABI 约定,参数/返回值都是 PyValue*

int main() {
  pylite_math_main();
}
```

链接时需要 `libpylite_runtime.dll`(在 `build/bin/` 下)。

### 查看中间产物

```bash
./build/bin/pylitec.exe math.pys -o math.o --emit-ir math.ll   # LLVM IR
./build/bin/lex_dump.exe math.pys                              # token 流
./build/bin/ast_dump.exe math.pys                              # 语法树
```

## 目录结构

| 路径 | 内容 |
|---|---|
| `frontend/` | 词法、语法、AST —— **不依赖 LLVM**,可独立测试 |
| `irgen/` | AST → LLVM IR,含集中管理 ABI 约定的 `abi.h` |
| `aot/` | 目标文件 / IR 文本生成 |
| `runtime/` | 运行时库(`libpylite_runtime.dll`),由 GCC 编译 |
| `include/pylite/value.h` | `PyValue` —— 整个项目的 ABI 基石 |
| `docs/llvm-notes.md` | LLVM 集成的实测结论,**改代码前先读** |
| `docs/language.md` | 支持的语言子集与已知限制 |
| `tests/e2e/` | 端到端用例:`*.pys` + `*.expected`(可选 `*.in`) |

## 两个务必先读的点

1. **[docs/llvm-notes.md](docs/llvm-notes.md) 第 2 节:PyValue 绝不能按值跨 ABI 边界。**
   Win64 规定大于 8 字节的聚合体按引用传递。在 IR 里按值声明会**链接通过、
   lookup 成功、一执行就段错误**。所有运行时调用都要经过 `irgen/abi.h`。

2. **运行时必须是 DLL,不能是静态库。** JIT 生成的代码要在运行时解析 `py_add`
   这类符号,而 Windows 上 MinGW 链接出的 exe 几乎不导出任何符号。

## 已知限制

- 内存只增不还(bump 分配器),循环里拼字符串会无界增长
- 没有异常:运行时错误打印消息后 `exit(1)`
- 字符串/列表/字典的方法与切片尚未实现
- 比较不支持 Python 的链式写法(`a < b < c`)
- 整数是 64 位,不会自动提升为任意精度
