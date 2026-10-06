# LLVM 集成笔记

本文记录在本机环境(Windows 11 + MSYS2 UCRT64 + LLVM 22.1.8)上实测得出的
结论。**每一条都是实测的,不是推测的** —— 与 LLVM 官方文档或第三方教程冲突时,
以本文为准。

## 1. 环境事实

| 项目 | 值 |
|---|---|
| 编译器 | `D:\msys64\ucrt64\bin\g++.exe`,**16.2.0**,target `x86_64-w64-mingw32` |
| LLVM | **22.1.8**(`mingw-w64-ucrt-x86_64-llvm`) |
| `llvm-config --cmakedir` | `D:/msys64/ucrt64/lib/cmake/llvm` |
| `llvm-config --shared-mode` | **shared** —— 只有 `libLLVM-22.dll`(141MB) |
| 构建 shell | MSYS2 **UCRT64**,即 `D:\msys64\ucrt64.exe` |

### 严禁混用工具链

`D:\mingw64` 上另有一个 g++ 13.2.0,它是 **MSVCRT** 版(configure 路径含
`msvcrt-rt_v11`)。它与 UCRT64 的 LLVM 包 ABI 不兼容 —— 混用轻则链接报错,
重则堆和 `std::string` 静默损坏。

判断自己在哪个环境:
```bash
g++ --version | head -1     # 必须是 16.2.0;若显示 13.2.0 说明在错误的 shell 里
```

## 2. ⚠️ 最重要的一条:PyValue 绝不能按值跨 ABI 边界

**这是实测踩到的坑,会直接导致段错误。**

### 现象

在 IR 里按值声明并调用:

```llvm
; ❌ 错误写法 —— 调用时必然段错误
declare %PyValue @py_add(%PyValue, %PyValue)
%sum = call %PyValue @py_add(%PyValue %a, %PyValue %b)
```

`lookup` 能成功,JIT 链接也没问题,但**一执行就 segfault**。

### 原因

Win64 ABI 规定:大于 8 字节的聚合体**按引用传递**。GCC 编译
`PyValue py_add(PyValue a, PyValue b)` 时严格遵循这条规则。反汇编
`libpylite_runtime.dll` 里的 `py_add` 序言可以看到:

```asm
py_add:
    sub  $0x28,%rsp
    mov  0x8(%rdx),%r9     ; 解引用 RDX —— 说明 RDX 是指向 PyValue 的指针
    mov  (%r8),%r10        ; 解引用 R8
    mov  %rcx,%rax         ; RCX 是 sret 返回缓冲区指针
    mov  (%rdx),%rcx
```

即 GCC 侧的约定是:

| 寄存器 | 含义 |
|---|---|
| `RCX` | sret 返回缓冲区指针 |
| `RDX` | `&a`(PyValue 的指针) |
| `R8`  | `&b`(PyValue 的指针) |

而按值声明时,LLVM 选用了**另一套**降低方式,两边对不上,于是崩溃。

### 正确写法

按 Win64 ABI 显式声明边界,把"按引用"和"sret"写进 IR:

```llvm
; ✅ 正确写法
declare void @py_add(ptr sret(%PyValue), ptr, ptr)

%a.slot = alloca %PyValue, align 8
%b.slot = alloca %PyValue, align 8
%r.slot = alloca %PyValue, align 8
store %PyValue %a, ptr %a.slot, align 8
store %PyValue %b, ptr %b.slot, align 8
call void @py_add(ptr sret(%PyValue) %r.slot, ptr %a.slot, ptr %b.slot)
; 结果从 %r.slot 里 load 出来
```

对应的 LLVM C++ API:

```cpp
auto *Ptr = PointerType::getUnqual(Ctx);
Attribute SretAttr = Attribute::getWithStructRetType(Ctx, V);  // V = {i32,i64}

// 函数声明:void(ptr sret(%PyValue), ptr, ptr)
auto *Fn = Function::Create(
    FunctionType::get(Type::getVoidTy(Ctx), {Ptr, Ptr, Ptr}, false),
    Function::ExternalLinkage, "py_add", *M);
Fn->addParamAttr(0, SretAttr);

// 调用点也要重复标一次(属性不会自动从声明传播到调用)
auto *C = B.CreateCall(Fn, {SlotR, SlotA, SlotB});
C->addParamAttr(0, SretAttr);
```

> **给 IRGen 的硬性要求**
>
> 1. 任何以 PyValue 为参数或返回值的运行时函数,**一律**声明成
>    `void(ptr sret(%PyValue)?, ptr..., ...)` —— 参数是"指向 PyValue 的指针",
>    返回值走 sret。
> 2. 调用前把操作数 store 进 alloca,把 alloca 的指针传进去。
> 3. 标量参数(int64/指针/等)不受此影响,正常按值传。
> 4. 若某个函数要返回多个值,同样只用一个 sret 缓冲区,不要返回聚合体。
>
> 这条规则要固化进 `irgen/abi.h`,不能靠每个调用点自己记得。

### 为什么这个坑不容易发现

`lookup()` 成功、JIT 链接成功、IR 验证通过 —— 所有静态检查都显示"没问题",
只有真正执行时才炸。所以 `tests/jit_spike.cpp` 同时保留了一个不调用运行时的
对照函数 `jit_add_plain`:它证明 JIT 执行路径本身是好的,从而把问题精确锁定
在 ABI 边界上。

## 3. LLVM 22 的 API 与常见教程的差异

以下都是在本机 LLVM 22.1.8 头文件里核对过的,与多数网上教程(基于 LLVM 14–17)不同。

### 3.1 符号搜索生成器改名了

```cpp
// ❌ LLVM 22 中不存在,头文件也没有
#include "llvm/ExecutionEngine/Orc/DynamicLibrarySearchGenerator.h"
DynamicLibrarySearchGenerator::Load(...)

// ✅ LLVM 22 的正确形式
#include "llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h"
auto Gen = ExitOnErr(orc::EPCDynamicLibrarySearchGenerator::Load(
    ES, "libpylite_runtime.dll"));   // 注意第一个参数是 ExecutionSession&
J.getMainJITDylib().addGenerator(std::move(Gen));
```

签名:`Load(ExecutionSession &ES, const char *LibraryPath, SymbolPredicate Allow = {}, AddAbsoluteSymbolsFn = nullptr)`

`ES` 由 `JIT.getExecutionSession()` 取得。

### 3.2 没有 `setUseJITLink`

`LLJITBuilder` 上**不存在** `setUseJITLink`。LLVM 22 已把 Windows/COFF 的原生
JIT 支持做进默认路径(头文件里有 `COFFPlatform.h`、`COFFVCRuntimeSupport.h`),
不需要手动切换链接器。实测默认配置直接可用。

### 3.3 `lookup` 返回 `ExecutorAddr`

```cpp
// ✅ LLVM 22
llvm::orc::ExecutorAddr A = ExitOnErr(J.lookup("jit_test"));
auto *Fn = A.toPtr<int64_t(*)(int64_t, int64_t)>();
```

`JITEvaluatedSymbol` / `jitTargetAddressToFunction` 都已移除。

### 3.4 CMake:shared 模式下链接极简

因为是 shared 模式,`LLVMConfig.cmake` 导出了一个名为 `LLVM` 的 imported
target(`add_library(LLVM SHARED IMPORTED)`)。链接它就够了:

```cmake
find_package(LLVM REQUIRED CONFIG)
target_include_directories(目标 PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(目标 PRIVATE LLVM)
```

**不要**去枚举组件名。实测:
- 组件名在本 build 里**全小写**(`x86codegen` 存在,`X86CodeGen` 不存在)
- `llvm_map_components_to_libnames` 在这种配置下没必要
- `llvm-config --libs orcjit jitlink native` 只返回 `-lLLVM-22`

`LLVM_DEFINITIONS` 是含 `-D` 前缀的空格分隔字符串,要用
`add_compile_options` + `separate_arguments`,不能用 `target_compile_definitions`。

### 3.5 代码生成文件类型

```cpp
// ✅ LLVM 22(enum class,在 llvm/Support/CodeGen.h)
llvm::CodeGenFileType::ObjectFile

// ❌ 旧写法,已废弃
llvm::CGFT_ObjectFile / llvm::TargetMachine::CGFT_ObjectFile
```

`addPassesToEmitFile` 签名(**失败返回 true**):

```cpp
bool addPassesToEmitFile(PassManagerBase &PM, raw_pwrite_stream &Out,
                         raw_pwrite_stream *DwoOut, CodeGenFileType FileType,
                         bool DisableVerify = true,
                         MachineModuleInfoWrapperPass *MMIWP = nullptr);
```

## 4. 运行时库必须是 DLL

JIT 生成的代码要在运行时解析 `py_add` 这类符号。Windows 上**不能**靠
"搜索当前进程导出符号"(`GetForCurrentProcess`)—— MinGW 链接出的 exe
几乎不导出任何东西。

所以 `pylite_runtime` 必须是 `SHARED`(DLL),并用
`EPCDynamicLibrarySearchGenerator::Load` 按名字显式加载。

### 不要把 __declspec(dllexport) 撒进运行时

MinGW 对 `-shared` 库**默认自动导出全部符号**。而一旦出现第一个
`__declspec(dllexport)`,后续符号就不再自动导出 —— 反而会漏掉一批,
让 JIT 找不到 `py_add`。保持默认即可。

## 5. 运行时 DLL 的依赖

- `jit_spike.exe` / `pylite_repl.exe` 需要 `libLLVM-22.dll` 在 PATH 上。
  从 **UCRT64 shell** 运行即可(`/ucrt64/bin` 已在 PATH);从别处运行会因
  找不到 DLL 而启动失败。
- `libpylite_runtime.dll` 与 exe 同在 `build/bin/` 下,自动能找到。

## 6. 谓词返回 i32,不返回 i1

`py_truthy` 等判定函数一律返回 `int32_t`。GCC 与 Clang 对 `bool` 返回值的
ABI 标注不一致(`i1` vs `zeroext i1`),跨 DLL 边界不可靠。

## 7. 复现验证

```bash
# 从 UCRT64 shell
cd /d/pylite
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build
cd build && ctest --output-on-failure
```

`tests/abi_test.cpp` 与 `tests/jit_spike.cpp` 全过,说明 ABI 边界是通的。
**在写任何前端代码之前必须先过这两个测试。**
