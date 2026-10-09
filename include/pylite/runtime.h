// PyLite 运行时库的 C ABI。
//
// IRGen 会对这里每个函数生成 call;用户手写的 C++ 也可以直接调用它们。
// 所有函数都从 libpylite_runtime.dll 导出(靠 MinGW 的 -shared 默认自动导出,
// 刻意不加 __declspec(dllexport) —— 那会触发 MinGW "遇到第一个 dllexport
// 就停止自动导出其余符号"的行为,反而让 JIT 找不到别的符号)。
//
// ⚠️ 跨边界约定 —— 改动前务必先读 docs/llvm-notes.md 第 2 节
//
// 下面这些函数在 C++ 源码里是按值的 `PyValue f(PyValue, PyValue)`,
// 但 IRGen 在 IR 里必须把它们声明成:
//
//     void @f(ptr sret(%PyValue), ptr, ptr)
//
// 即参数是"指向 PyValue 的指针"、返回值写进 sret 指针。
// 两者在机器层面一致,因为 Win64 ABI 规定 16 字节聚合体按引用传递、
// 返回值走隐藏 sret 寄存器(RCX = sret,RDX/R8/... = 各参数)。
//
// 若在 IR 里按值声明(`%PyValue @f(%PyValue, %PyValue)`),LLVM 会选另一套
// 降低方式,与 GCC 不匹配 —— 链接通过、lookup 成功,但一执行就段错误。
//
// 其它约定:
//   * 谓词一律返回 int32_t 而非 bool(i1 的 ABI 标注 GCC 与 Clang 不一致)
//   * 数组参数(如 py_print 的 PyValue*)是普通指针,不受上面限制
#pragma once

#include "pylite/value.h"

extern "C" {

// --- 错误 ---------------------------------------------------------------
// [[noreturn]] 不只是给编译器看的优化提示:没有它,GCC 会认为
// badOperands 这类同样标了 [[noreturn]] 的辅助函数"会返回",从而报警告。
[[noreturn]] void py_runtime_error(const char *msg);
const char *py_tag_name(int32_t tag);

// --- 构造 ---------------------------------------------------------------
// 刻意没有 py_int/py_float/py_bool 的运行时版本 —— 它们作为 inline 函数
// 定义在 value.h 里。IRGen 用 insertvalue 指令内联构造 PyValue,不发起调用;
// 若在这里再声明一份 extern "C" 版本,会与 value.h 的 C++ 链接版本构成
// 同名不同语言链接的非法重声明。

// --- 算术 ---------------------------------------------------------------
PyValue py_add(PyValue a, PyValue b);       // 数值相加 / 字符串拼接 / 列表拼接
PyValue py_sub(PyValue a, PyValue b);
PyValue py_mul(PyValue a, PyValue b);       // 数值相乘 / 字符串与列表重复
PyValue py_div(PyValue a, PyValue b);       // 真除法,恒为 float
PyValue py_floordiv(PyValue a, PyValue b);  // 向下取整除法
PyValue py_mod(PyValue a, PyValue b);       // 取模,符号跟随除数
PyValue py_pow(PyValue a, PyValue b);
PyValue py_neg(PyValue a);                  // 一元负号

// a += b。与 py_add 的区别**只在列表上**:Python 的 `a += [x]` 是就地 extend,
// 别名看得见改动;而 `a = a + [x]` 是重新绑定,别名不受影响。
// 第一个参数因此是可写的 —— 列表就地改,其余类型(数值、字符串不可变)
// 一律返回新值,由调用点写回自己的槽位。
PyValue py_iadd(PyValue *a, const PyValue *b);

// --- 比较(返回 bool 值的 PyValue)----------------------------------------
PyValue py_eq(PyValue a, PyValue b);
PyValue py_ne(PyValue a, PyValue b);
PyValue py_lt(PyValue a, PyValue b);
PyValue py_le(PyValue a, PyValue b);
PyValue py_gt(PyValue a, PyValue b);
PyValue py_ge(PyValue a, PyValue b);

// 三路比较,返回 -1 / 0 / 1。数值之间比大小、两个字符串按字典序比;
// 类型不可比时报运行时错误(供 < <= > >= 与 list.sort 共用)。
int32_t py_compare(const PyValue *a, const PyValue *b);

// --- 逻辑 ---------------------------------------------------------------
// and / or 的短路由 IRGen 用 PHI 实现(且必须返回操作数本身而非布尔值),
// 所以运行时只提供 not。
PyValue py_not(PyValue a);

// --- 真值判定 -----------------------------------------------------------
int32_t py_truthy(PyValue v);

// --- 转换 ---------------------------------------------------------------
// 取出整数负载;非整数时报运行时错误(用于 range 的边界)
int64_t py_to_int(PyValue v);

// --- 输出 ---------------------------------------------------------------
// 打印 n 个值,空格分隔,末尾换行。
void py_print(PyValue *vals, int64_t n);

// 按 repr 规则渲染成一个新字符串:容器里的字符串带引号并转义,
// 与 print 的顶层 str 规则不同 —— print("hi") 打 hi,print(["hi"]) 打 ['hi']。
// 目前供字典的 KeyError 类错误信息使用。
PyValue py_repr(const PyValue *v);

// --- 输入 ---------------------------------------------------------------
// input(int, int) 在 IRGen 里被拆成若干个具体读取调用,结果打包成元组
// 再交给多目标赋值解包。全部返回 PyValue,在 IR 里声明成 void(ptr sret)。
PyValue py_read_int();
PyValue py_read_float();
PyValue py_read_bool();
PyValue py_read_str();

// --- 内存与垃圾回收 ------------------------------------------------------
// 保守式标记-清除(见 runtime/gc.cpp 的说明)。只有 str/list/dict/tuple 是堆
// 对象,int/float/bool/None 是内联标量、根本不分配。
//
// 根 = 保守扫描本线程的栈与寄存器 + 显式根栈。**根必须放在栈上** —— 全局变量、
// 静态变量、thread_local 里放 PyValue 不会被认为是根,对象会被回收掉。
//
// 下面这些是运行时内部用的;用户手写 C++ 只需要 py_alloc。
typedef enum {
  PY_GC_SCAN_NONE = 0,    // 纯字节缓冲:字符串内容、临时的 char 缓冲
  PY_GC_SCAN_WORDS = 1,   // 保守逐字扫描:容器对象头(含 items/keys/vals 指针)
  PY_GC_SCAN_VALUES = 2,  // 按 PyValue 单元格扫描,由 tag 决定是否跟随 payload
} PyGcScan;

typedef struct {
  int64_t alloc_bytes_total;
  int64_t freed_bytes_total;
  int64_t live_bytes;
  int64_t live_blocks;
  int64_t collections;
  // 当前模式(来自环境变量)。dry_run 下被判定为死的块只下毒、**不归还内存**,
  // 所以"存活块数下降"这类断言在干跑模式里不成立 —— 测试要据此调整。
  int32_t dry_run;
  int32_t disabled;
} PyGcStats;

// 分配 n 字节并登记为一个 GC 块。n 会向上取整到 16 的倍数。
void *py_gc_alloc(size_t n, PyGcScan kind);
// SCAN_VALUES 的专用入口:从 firstCellOffset 起是若干 16 字节的 PyValue 单元格。
// ⚠️ 元组传 8(PyTuple 是 {int64 len; PyValue items[]}),传 0 会把 len 当 tag、
// 整条链错位,那是会丢根的。
void *py_gc_alloc_values(size_t n, size_t firstCellOffset);

void py_gc_collect();          // 手动触发一次回收
void py_gc_stats(PyGcStats *out);
void py_gc_init();             // 可选:提前读环境变量并注册 atexit 统计

// 显式根栈。用于运行时内部的 C++ 局部变量 —— 见 gc.cpp 里"最可能出错的一处"。
// slot 必须指向一个局部指针变量;取地址这个动作会迫使该值留在内存里,
// 于是不再依赖编译器恰好在 -O2 下把它 spill 到栈上。
void py_gc_root_push(void **slot);
void py_gc_root_pop(size_t n);

// 历史符号,保留以维持 ABI(用户手写 C++ 会直接调 py_alloc)。
// py_alloc 按最保守的方式扫描 —— 宁可多保留,不可漏根。
void *py_alloc(size_t n);
void py_arena_reset();         // 语义已改为"强制回收一次"
// --- 引用计数(共享指针 / 弱指针语义) -----------------------------------
// 计数位记在**块头**里,对象自身数据布局与 PyValue 布局都不动,不触碰 ABI 契约。
// 生命周期:
//   * 强引用归零                 -> 对象内容失效(仍可被弱引用观察)
//   * 强引用归零且已无弱引用     -> 真正归还内存
//   * 弱引用归零                 -> 对象早已失效时,此刻归还内存
// ⚠️ 强引用计数为 0 表示"尚无强引用登记",此时减引用不做任何事,交回标记-清除;
//    因此即使生成代码漏插计数调用,也只是退回原行为,绝不会误释放活对象。
// ⚠️ 循环引用回收不了,由标记-清除按"从根可达"兜底,整体不会泄漏。
// payload 是堆对象指针(与 PyValue.as.ptr 一致)。
void py_incref(void *payload);        // 加强引用
void py_decref(void *payload);        // 减弱引用(归零即失效/释放)
void py_weakref(void *payload);       // 登记一个弱引用(不影响对象生命周期)
void py_weak_release(void *payload);  // 释放一个弱引用
// 弱引用升级为强引用:对象已失效返回 0,否则加引用并返回 1 —— 绝不返回悬空指针。
int32_t py_weak_upgrade(void *payload);
int32_t py_weak_alive(void *payload); // 弱引用是否仍指向活对象
int64_t py_refcount(void *payload);   // 读取强引用计数

// --- 元组 ---------------------------------------------------------------
// 全部按指针收发,避免聚合体按值跨 ABI 边界
PyValue py_tuple_new(PyValue *elems, int64_t n);
int64_t py_tuple_len(const PyValue *t);
PyValue py_tuple_get(const PyValue *t, int64_t i);   // 支持负索引
void py_unpack(const PyValue *t, PyValue *out, int64_t n);

// --- 字符串 -------------------------------------------------------------
PyValue py_str_new(const char *bytes, int64_t len);
int64_t py_str_size(const PyValue *s);
const char *py_str_data(const PyValue *s);
PyValue py_str_concat(const PyValue *a, const PyValue *b);
PyValue py_str_get(const PyValue *s, int64_t i);          // 取单个字符,支持负索引
PyValue py_str_repeat(const PyValue *s, int64_t times);
// 按字节序比较,返回 -1 / 0 / 1
int32_t py_str_compare(const PyValue *a, const PyValue *b);
int32_t py_str_find(const PyValue *s, const PyValue *needle, int64_t from);
int32_t py_str_starts_with(const PyValue *s, const PyValue *prefix);
int32_t py_str_ends_with(const PyValue *s, const PyValue *suffix);

// --- 列表 ---------------------------------------------------------------
// PyList 是可变对象:append/insert/pop/... 就地改,别名的调用方看得见改动。
// 增长靠"新分配 + 拷贝",因为 bump 分配器没有 realloc(旧数组就地泄漏,
// 与项目其它部分一致,见 runtime/arena.cpp)。
PyValue py_list_new(PyValue *elems, int64_t n);
int64_t py_list_len(const PyValue *v);
PyValue py_list_get(const PyValue *v, int64_t i);          // 支持负索引
void py_list_set(const PyValue *v, int64_t i, const PyValue *val);
PyValue py_list_concat(const PyValue *a, const PyValue *b);  // 新列表
PyValue py_list_repeat(const PyValue *a, int64_t times);     // 新列表

void py_list_append(const PyValue *v, const PyValue *item);
void py_list_extend(const PyValue *v, const PyValue *other);
void py_list_insert(const PyValue *v, int64_t i, const PyValue *item);
void py_list_remove(const PyValue *v, const PyValue *item);
// idxOrNone 传的是一个 **None 值**(py_none()),不是空指针 —— 长度为 0 的
// 下标在语言里表达不出来,所以借 None 当"没给下标"的哨兵。传 nullptr 会崩。
PyValue py_list_pop(const PyValue *v, const PyValue *idxOrNone);
int64_t py_list_index(const PyValue *v, const PyValue *item);     // 找不到报错
int64_t py_list_count(const PyValue *v, const PyValue *item);
void py_list_reverse(const PyValue *v);
void py_list_clear(const PyValue *v);
void py_list_sort(const PyValue *v);

// --- 字典 ---------------------------------------------------------------
// 插入有序 + 线性扫描。不用哈希表:维护 Python 3.7 的可观测插入序,
// 且省掉为每种键类型写 hash 的面上。代价是查找 O(n),见 docs/language.md。
PyValue py_dict_new(PyValue *keys, PyValue *vals, int64_t n);
int64_t py_dict_len(const PyValue *d);
PyValue py_dict_get(const PyValue *d, const PyValue *key);   // 键不存在则报错
PyValue py_dict_get_default(const PyValue *d, const PyValue *key,
                            const PyValue *def);
int32_t py_dict_has(const PyValue *d, const PyValue *key);
void py_dict_set(const PyValue *d, const PyValue *key, const PyValue *val);
PyValue py_dict_key_at(const PyValue *d, int64_t i);   // 按插入序取第 i 个键
PyValue py_dict_val_at(const PyValue *d, int64_t i);
// pop 必须给键。这里刻意不用 None 当"没给键"的哨兵 —— None 本身是合法的键,
// 两者混淆会让 `d.pop(None)` 变得无法表达。
PyValue py_dict_pop(const PyValue *d, const PyValue *key);
void py_dict_clear(const PyValue *d);

// --- 下标 / 切片 / 长度 --------------------------------------------------
// "不支持下标"这类错误的唯一产生点。按 tag 分派到上面各类型的访问器。
int64_t py_len(const PyValue *v);       // str / list / dict / tuple
PyValue py_index(const PyValue *obj, const PyValue *idx);
// obj[idx] = val。列表按位置改,字典按键插入或更新(新增键追加在末尾)。
void py_setindex(const PyValue *obj, const PyValue *idx, const PyValue *val);
// lo / hi / step 省略时传 None(即 PY_NULL),各自按默认值处理
PyValue py_slice(const PyValue *obj, const PyValue *lo, const PyValue *hi,
                 const PyValue *step);

// for 循环用的"第 i 个元素"。**不能**拿 py_index 代替:遍历字典要的是第 i 个
// 键,而下标 d[k] 是按键查找,两者语义不同。
PyValue py_iter_at(const PyValue *obj, int64_t i);

// --- 方法调用 -----------------------------------------------------------
// obj.print(...) 形式统一走这一个入口,按 tag + 方法名分派。
// 方法表在运行时里,所以拼错方法名是**运行时**报错 —— 本项目的类型注解
// 本来就不做检查(见 docs/language.md),这是动态语言的必然。
PyValue py_call_method(const PyValue *obj, const char *name, int64_t nameLen,
                       PyValue *args, int64_t nargs);

// --- Docker 容器调用 -----------------------------------------------------
// 通过 popen 调用 docker CLI，不引入额外依赖。
// 部署到有 Docker daemon 的环境中即可直接使用。
PyValue py_docker_run(const PyValue *image, const PyValue *cmd);
PyValue py_docker_ps();
void py_docker_stop(const PyValue *containerId);
PyValue py_docker_logs(const PyValue *containerId);
PyValue py_docker_pull(const PyValue *image);

// Docker 项目封装
PyValue py_docker_generate_dockerfile(const PyValue *baseImage,
                                       const PyValue *projectName,
                                       const PyValue *setupCommands,
                                       const PyValue *entrypoint,
                                       const PyValue *exposePort);
PyValue py_docker_build(const PyValue *dockerfile, const PyValue *imageName,
                         const PyValue *tag, const PyValue *contextDir);
PyValue py_docker_push(const PyValue *imageName, const PyValue *tag,
                        const PyValue *registry);
PyValue py_docker_project_package(const PyValue *projectDir,
                                   const PyValue *imageName,
                                   const PyValue *tag,
                                   const PyValue *baseImage,
                                   const PyValue *setupCommands,
                                   const PyValue *entrypoint,
                                   const PyValue *exposePort);

// --- LLM 神经网络接口 -----------------------------------------------------
// 通过 curl 调用 OpenAI 兼容 API，支持网络结构描述与修改。
PyValue py_llm_chat(const PyValue *prompt, const PyValue *systemPrompt,
                    const PyValue *model, const PyValue *apiKey,
                    const PyValue *endpoint);
PyValue py_llm_create_network(const PyValue *name, const PyValue *layers);
PyValue py_llm_add_layer(const PyValue *network, const PyValue *layer);
PyValue py_llm_remove_layer(const PyValue *network, const PyValue *index);
PyValue py_llm_network_summary(const PyValue *network);

// LLM 微调（Fine-tuning）
PyValue py_llm_create_dataset(const PyValue *examples, const PyValue *systemPrompt);
PyValue py_llm_upload_file(const PyValue *data, const PyValue *filename,
                           const PyValue *apiKey, const PyValue *endpoint);
PyValue py_llm_create_finetune(const PyValue *fileId, const PyValue *model,
                                const PyValue *suffix, const PyValue *apiKey,
                                const PyValue *endpoint);
PyValue py_llm_finetune_status(const PyValue *jobId, const PyValue *apiKey,
                                const PyValue *endpoint);
PyValue py_llm_list_finetunes(const PyValue *apiKey, const PyValue *endpoint);

// --- CUDA 编程支持 --------------------------------------------------------
// 通过 CUDA Driver API 和 LLVM NVPTX 后端实现 GPU 编程。
// 需要 NVIDIA GPU 和 CUDA 驱动。
PyValue py_cuda_info();
PyValue py_cuda_compile_ptx(const PyValue *kernelCode, const PyValue *kernelName);
PyValue py_cuda_launch_kernel(const PyValue *ptxCode, const PyValue *kernelName,
                               const PyValue *gridX, const PyValue *gridY,
                               const PyValue *gridZ, const PyValue *blockX,
                               const PyValue *blockY, const PyValue *blockZ,
                               const PyValue *args);
PyValue py_cuda_alloc(const PyValue *size);
void py_cuda_free(const PyValue *handle);
void py_cuda_memcpy_to_device(const PyValue *handle, const PyValue *data);
PyValue py_cuda_memcpy_from_device(const PyValue *handle, const PyValue *size);

// --- 算子融合优化 ---------------------------------------------------------
// LLM 推理加速：算子 IR 描述、融合规则引擎、CUDA 内核生成、自动调优。
PyValue py_fusion_create_graph(const PyValue *name);
PyValue py_fusion_add_op(const PyValue *graph, const PyValue *opType,
                          const PyValue *opName, const PyValue *params);
PyValue py_fusion_apply_rules(const PyValue *graph);
PyValue py_fusion_generate_kernel(const PyValue *fusedOp, const PyValue *config);
PyValue py_fusion_autotune(const PyValue *opType, const PyValue *M,
                            const PyValue *N, const PyValue *K);
PyValue py_fusion_compile_and_run(const PyValue *kernelCode,
                                   const PyValue *kernelName,
                                   const PyValue *M, const PyValue *N,
                                   const PyValue *K);
PyValue py_fusion_benchmark(const PyValue *kernelCode, const PyValue *kernelName,
                             const PyValue *M, const PyValue *N,
                             const PyValue *K, const PyValue *iterations);

// --- LLM 推理运行时 -------------------------------------------------------
// 模型加载、Token 生成、KV Cache 管理、FlashAttention、多 GPU、性能分析。
PyValue py_inference_load_model(const PyValue *modelPath, const PyValue *configPath);
PyValue py_inference_generate(const PyValue *prompt, const PyValue *maxTokens,
                               const PyValue *temperature, const PyValue *topP,
                               const PyValue *topK);
PyValue py_inference_kv_cache_stats();
PyValue py_inference_flash_attention_kernel();
PyValue py_inference_multi_gpu_info();
PyValue py_inference_profile();
void py_inference_unload();

// --- Tokenizer 分词器 -----------------------------------------------------
// BPE 编码/解码，支持 HuggingFace tokenizer.json 格式。
PyValue py_tokenizer_load(const PyValue *path);
PyValue py_tokenizer_encode(const PyValue *text);
PyValue py_tokenizer_decode(const PyValue *tokenIds);
PyValue py_tokenizer_info();

// --- REST API 服务 --------------------------------------------------------
// OpenAI 兼容的 HTTP 推理接口，零外部依赖。
PyValue py_server_start(const PyValue *port);
void py_server_stop();
PyValue py_server_status();

// --- 模型量化工具与多架构支持 ---------------------------------------------
// FP16→AWQ 4-bit 转换、主流架构配置模板、量化精度验证。
PyValue py_quantize_fp16_to_awq(const PyValue *weightsJson, const PyValue *bits,
                                 const PyValue *groupSize);
PyValue py_quantize_arch_template(const PyValue *archName);
PyValue py_quantize_verify_precision(const PyValue *fp16Values,
                                      const PyValue *awqValues);
PyValue py_quantize_model_info(const PyValue *modelPath);

// --- PyLite 原生推理服务引擎 ---------------------------------------------
// vLLM 风格的 LLM 推理引擎：PagedAttention KV Cache + 连续批处理。
PyValue py_engine_init(const PyValue *configJson);
PyValue py_engine_add_request(const PyValue *prompt, const PyValue *maxTokens,
                               const PyValue *temperature, const PyValue *topP,
                               const PyValue *topK);
PyValue py_engine_step();
PyValue py_engine_status();
void py_engine_reset();
PyValue py_engine_serve(const PyValue *port);

// --- 线程 / 进程 / 资源管理 -----------------------------------------------
// 线程任务用注册表管理:这门语言目前还没有一等函数,线程入口无法在语言里
// 直接写,由宿主 C++ 按名字注册、语言层按名字启动。
// 所有跨线程共享状态由一把互斥锁保护,句柄登记在带锁的表里、结束可回收。
// 线程任务函数的真实签名是 void (*)(void *),arg 原样透传。
int64_t py_thread_register_task(const char *name, void *fn, void *arg);
PyValue py_thread_start(const char *taskName);   // 返回线程号
int64_t py_thread_join(int64_t id);              // 等待结束并回收条目
int64_t py_thread_alive(int64_t id);             // 1 = 仍在运行
int64_t py_thread_count();                       // 登记表中的线程数
// 进程:同步执行(返回标准输出)、后台启动(返回进程号)、等待(返回退出码)、
// 判活、终止。
PyValue py_process_run(const PyValue *cmd);
int64_t py_process_spawn(const PyValue *cmd);
int64_t py_process_wait(int64_t id);
int64_t py_process_alive(int64_t id);
int64_t py_process_kill(int64_t id);
// 资源管理:快照(线程与进程计数)与回收已结束的线程条目。
PyValue py_resource_stats();
int64_t py_resource_reap();

// --- 类实例 ---------------------------------------------------------------
// 实例 = 类标识 + 属性表(用字典承载)。属性名是字符串,实例有没有某个属性
// 只有运行时才知道 —— 与"方法名写错是运行时错误"是同一套取舍。
// 方法不进属性表:成员方法由编译期直接解析成函数调用,运行时只负责属性。
PyValue py_instance_new(int64_t classId, const PyValue *attrs);
int64_t py_instance_class_id(const PyValue *inst);
void py_instance_set_attr(const PyValue *inst, const char *name, int64_t nameLen,
                          const PyValue *val);
PyValue py_instance_get_attr(const PyValue *inst, const char *name, int64_t nameLen);
int32_t py_instance_has_attr(const PyValue *inst, const char *name, int64_t nameLen);
int64_t py_instance_attr_count(const PyValue *inst);

// --- 互斥锁与条件变量 -----------------------------------------------------
// 句柄是一个整数,实际对象登记在运行时内部,靠句柄访问 —— 语言层不必认识原生指针。
// 条件变量的等待会原子地放开传入的那把锁,被唤醒后重新拿回它;
// 这正是"检查条件"与"等待"之间不丢唤醒的关键。
// ⚠️ 等条件变量前必须先拿到配套的那把锁:检查条件与进入等待之间不能有缝,
// 否则通知可能在两者之间发出,等待者就永远等下去了。
PyValue py_sync_mutex_new();                              // 创建锁,返回句柄
PyValue py_sync_mutex_lock(const PyValue *h);             // 加锁(阻塞直到拿到)
PyValue py_sync_mutex_unlock(const PyValue *h);           // 解锁
PyValue py_sync_mutex_free(const PyValue *h);             // 销毁锁
PyValue py_sync_cond_new();                               // 创建条件变量,返回句柄
PyValue py_sync_cond_wait(const PyValue *ch, const PyValue *mh);  // 等待通知
PyValue py_sync_cond_signal(const PyValue *ch);           // 唤醒一个等待者
PyValue py_sync_cond_broadcast(const PyValue *ch);        // 唤醒全部等待者
PyValue py_sync_cond_free(const PyValue *ch);             // 销毁条件变量
PyValue py_sync_stats();                                  // 同步原语数量快照

// --- 运行时通信 -----------------------------------------------------------
// 把程序运行情况(函数进入/退出、变量赋值、错误)实时推送到前端。
// 启动后会在指定端口上跑一个极简 HTTP 服务,前端通过服务器推送事件连接。
// port 为 0 表示用默认端口 18900。
void py_trace_start(int port);                            // 启动通信服务
void py_trace_stop();                                     // 停止通信服务
int64_t py_trace_enter(const char *name, int64_t parentId); // 函数进入,返回帧标识
void py_trace_exit(int64_t id, const char *name, int ok, int64_t elapsed); // 函数退出
void py_trace_vars(const char *varsJson);                 // 变量赋值(JSON 键值对)
void py_trace_error(const char *msg);                     // 错误事件
int32_t py_trace_enabled();                               // 是否已启用
// --- 供编译期插桩使用的便捷接口 ---
int64_t py_trace_begin(const char *name);                 // 函数进入:自动计时与层级
int64_t py_trace_begin_args(const char *name, int count,
                            const char *const *paramNames,
                            const PyValue *const *paramValues);  // 函数进入:附带形参名与实参值
void py_trace_end(int64_t id, const char *name, int ok);  // 函数退出:自动算耗时
void py_trace_var(const char *name, const char *value);   // 变量上报(字符串值)
void py_trace_var_value(const char *name, const PyValue *v); // 变量上报(直接收值)
void py_trace_set_dump(const char *path);                 // 设置事件落盘文件路径
// cuda / docker 操作事件:category 取 cuda 或 docker,op 为操作名,
// detail 为对象描述,ok 为是否成功,elapsed 为耗时(毫秒)。
void py_trace_op(const char *category, const char *op, const char *detail,
                 int ok, int64_t elapsed);

// --- 判题沙箱 -------------------------------------------------------------
// 在受控环境中执行用户提交的代码：seccomp 系统调用白名单 + 降权 + 资源限制。
// 这些接口供 .pys 脚本在编译时通过 IRGen 生成调用，也可在 C++ 侧直接使用。

// 创建沙箱实例，返回 PY_INSTANCE 对象
PyValue py_sandbox_create();
// 设置 CPU 时间限制（毫秒）
void py_sandbox_set_cpu_limit(const PyValue *inst, uint32_t cpu_limit_ms);
// 设置内存限制（字节）
void py_sandbox_set_mem_limit(const PyValue *inst, uint64_t mem_limit_bytes);
// 设置最大进程数（防进程炸弹）
void py_sandbox_set_max_pids(const PyValue *inst, int64_t max_pids);
// 设置工作目录
void py_sandbox_set_work_dir(const PyValue *inst, const char *path, int64_t pathLen);
// 设置用户可执行文件路径
void py_sandbox_set_binary(const PyValue *inst, const char *path, int64_t pathLen);
// 添加命令行参数
void py_sandbox_add_arg(const PyValue *inst, const char *arg, int64_t argLen);
// 设置标准输入/输出/错误重定向文件
void py_sandbox_set_stdin(const PyValue *inst, const char *path, int64_t pathLen);
void py_sandbox_set_stdout(const PyValue *inst, const char *path, int64_t pathLen);
void py_sandbox_set_stderr(const PyValue *inst, const char *path, int64_t pathLen);
// 执行沙箱（fork + sandbox_main + exec），返回子进程 pid
int64_t py_sandbox_exec(const PyValue *inst);
// 销毁沙箱实例
void py_sandbox_destroy(const PyValue *inst);

// --- cgroup 资源管理 ------------------------------------------------------
// 基于 Linux cgroup v2 的 CPU/内存/PID 限制与用量监控。

// 创建判题 cgroup（高 CPU 权重 200），返回 cgroup 路径字符串
PyValue py_cgroup_create_judge(const char *task_id, int64_t taskIdLen,
                               uint32_t cpu_limit_ms, uint64_t mem_limit_bytes,
                               int64_t max_pids);
// 将进程加入 cgroup
void py_cgroup_attach(const char *cg_path, int64_t pathLen, int64_t pid);
// 读取 CPU 使用时间（纳秒）
int64_t py_cgroup_read_cpu_ns(const char *cg_path, int64_t pathLen);
// 读取内存使用量（字节）
int64_t py_cgroup_read_mem_bytes(const char *cg_path, int64_t pathLen);
// 销毁 cgroup
void py_cgroup_destroy(const char *cg_path, int64_t pathLen);

// --- 系统监控 (sysmon) ----------------------------------------------------
// 基于 /proc 文件系统的 Linux 系统监控：CPU、内存、进程、网络、文件系统。
// 所有返回 JSON 字符串的函数，调用方负责通过 py_str_* 解析。

// CPU 统计（所有核心 + 每个核心 + 使用率）
PyValue py_sysmon_cpu_stats();
// 指定进程的 CPU 使用时间（毫秒）
int64_t py_sysmon_process_cpu_ms(int64_t pid);
// 系统内存信息（总量/可用/缓存/Swap + 使用率）
PyValue py_sysmon_mem_info();
// 指定进程的物理内存（RSS，KB）
int64_t py_sysmon_process_mem_kb(int64_t pid);
// 指定进程的详细内存信息（峰值/虚拟/RSS/数据段/栈）
PyValue py_sysmon_process_mem_info(int64_t pid);
// 所有进程 PID 列表
PyValue py_sysmon_list_processes();
// 指定进程的详细信息（PID/PPID/名称/状态/CPU/内存/线程/子进程）
PyValue py_sysmon_process_info(int64_t pid);
// 进程树的总资源用量（CPU/内存/PID 数）
PyValue py_sysmon_tree_usage(int64_t root_pid);
// 网络接口统计（收发字节/包数）
PyValue py_sysmon_net_stats();
// 文件系统配额（总量/已用/可用/inode + 使用率）
PyValue py_sysmon_fs_quota(const char *path, int64_t pathLen);
// 目录总大小（字节，递归）
int64_t py_sysmon_dir_size(const char *path, int64_t pathLen);
// 目录文件数量（递归）
int64_t py_sysmon_dir_file_count(const char *path, int64_t pathLen);
// 系统信息（主机名/内核/发行版/运行时间/CPU 数/总内存）
PyValue py_sysmon_sys_info();
// 系统负载（1/5/15 分钟）
PyValue py_sysmon_load_avg();

// --- Raft 分布式框架 ------------------------------------------------------
// 基于 Raft 共识算法的分布式集群管理：节点生命周期、状态机命令、
// 节点注册、动态插件加载。所有接口返回 JSON 字符串。

// 构建并启动 Raft 节点（config_json 含 node_id/bind_addr/peers 等）
PyValue py_raft_build(const char *config_json, int64_t jsonLen);
// 删除指定节点
PyValue py_raft_erase(const char *node_id, int64_t idLen);
// 获取节点状态（角色/任期/leader/提交索引等）
PyValue py_raft_status(const char *node_id, int64_t idLen);
// 列出所有本地节点 ID
PyValue py_raft_list_nodes();
// 提交命令到集群（仅 leader 可提交，follower 自动转发）
PyValue py_raft_add(const char *node_id, int64_t idLen,
                    const char *cmd_type, int64_t typeLen,
                    const char *payload, int64_t payloadLen);
// 注册命令处理器
int64_t py_raft_register_handler(const char *node_id, int64_t idLen,
                                  const char *cmd_type, int64_t typeLen);
// 注册节点角色
PyValue py_raft_register_node(const char *node_id, int64_t idLen,
                               const char *role, int64_t roleLen,
                               const char *addr, int64_t addrLen);
// 获取集群节点列表
PyValue py_raft_cluster_nodes(const char *node_id, int64_t idLen);
// 注册共享存储
PyValue py_raft_mount_storage(const char *node_id, int64_t idLen,
                               const char *name, int64_t nameLen,
                               const char *path, int64_t pathLen);
// 加载动态插件（集群同步）
PyValue py_raft_plugin_load(const char *node_id, int64_t idLen,
                             const char *name, int64_t nameLen,
                             const char *so_path, int64_t pathLen);
// 卸载插件
PyValue py_raft_plugin_unload(const char *node_id, int64_t idLen,
                               const char *name, int64_t nameLen);
// 列出已加载插件
PyValue py_raft_plugin_list(const char *node_id, int64_t idLen);

// --- 集群管理台 (cluster) -------------------------------------------------
// 轻量级多集群管理控制台：通过指定远程地址连接多个独立集群，
// 统一查看状态、执行命令、批量操作。管理台本身不运行 Raft 共识。

// 连接到远程集群（name: 别名, addr: host:port, secret: 密钥）
PyValue py_cluster_connect(const char *name, int64_t nameLen,
                            const char *addr, int64_t addrLen,
                            const char *secret, int64_t secretLen);
// 断开集群连接
PyValue py_cluster_disconnect(const char *name, int64_t nameLen);
// 列出所有已连接集群
PyValue py_cluster_list();
// 检查集群是否已连接（返回 1/0）
int64_t py_cluster_is_connected(const char *name, int64_t nameLen);
// 获取集群完整状态（leader/term/节点列表/健康/延迟）
PyValue py_cluster_status(const char *name, int64_t nameLen);
// 获取集群节点列表
PyValue py_cluster_nodes(const char *name, int64_t nameLen);
// 健康检查（ping 集群 /status 端点）
int64_t py_cluster_health(const char *name, int64_t nameLen);
// 向集群发送命令（HTTP POST /exec）
PyValue py_cluster_exec(const char *name, int64_t nameLen,
                         const char *cmd_type, int64_t typeLen,
                         const char *payload, int64_t payloadLen);
// 向集群发送查询（HTTP GET）
PyValue py_cluster_query(const char *name, int64_t nameLen,
                          const char *path, int64_t pathLen);
// 向所有已连接集群广播命令
PyValue py_cluster_broadcast(const char *cmd_type, int64_t typeLen,
                              const char *payload, int64_t payloadLen);
// 所有集群健康检查
PyValue py_cluster_health_all();

// --- 节点管理 (kv_admin) --------------------------------------------------
// 结合 AdvisKV 的节点生命周期管理：建立节点、进程控制（sleep/wakeup）、
// 逻辑跳过（skip）、永久移除（erase）。节点间通过 Raft 互相通信。

// 建立节点并启动进程（node_id + 可执行文件 + 参数 + 工作目录 + 地址）
PyValue py_kv_build(const char *node_id, int64_t idLen,
                    const char *binary, int64_t binLen,
                    const char *args_json, int64_t argsLen,
                    const char *work_dir, int64_t dirLen,
                    const char *addr, int64_t addrLen);
// 向集群追加节点（语义同 build）
PyValue py_kv_add(const char *node_id, int64_t idLen,
                  const char *binary, int64_t binLen,
                  const char *args_json, int64_t argsLen,
                  const char *work_dir, int64_t dirLen,
                  const char *addr, int64_t addrLen);
// 永久移除节点（停止进程并删除注册）
PyValue py_kv_erase(const char *node_id, int64_t idLen);
// 逻辑跳过节点（标记 LOST，等待副本替换）
PyValue py_kv_skip(const char *node_id, int64_t idLen);
// 暂停节点（SIGSTOP，故障注入）
PyValue py_kv_sleep(const char *node_id, int64_t idLen);
// 唤醒节点（SIGCONT）
PyValue py_kv_wakeup(const char *node_id, int64_t idLen);
// 停止节点（保留注册，可重启）
PyValue py_kv_stop(const char *node_id, int64_t idLen);
// 重启节点
PyValue py_kv_restart(const char *node_id, int64_t idLen);
// 获取节点详情 JSON
PyValue py_kv_status(const char *node_id, int64_t idLen);
// 列出所有节点 JSON
PyValue py_kv_list();
// 节点进程是否存活（1/0）
int64_t py_kv_alive(const char *node_id, int64_t idLen);

// 沙箱绑定与查找：将沙箱句柄关联到分布式节点
int64_t py_kv_bind_sandbox(const char *node_id, int64_t idLen, int64_t handle);
int64_t py_kv_get_sandbox(const char *node_id, int64_t idLen);
int64_t py_kv_unbind_sandbox(const char *node_id, int64_t idLen);


// ---- 分布式便捷内建(2026-10 增补) ----
// sleep_ms(ms):当前线程休眠指定毫秒数
void py_sleep_ms(int64_t ms);
// time_ms() / time_s():当前 Unix 时间戳(毫秒 / 秒)
PyValue py_time_ms();
PyValue py_time_s();
// raft 选主查询:返回 {node_id,role,leader_id,is_leader,term}
PyValue py_raft_leader_info(const char *node_id, int64_t idLen);
// raft 消息收件箱:send 入队 / recv 出队(空返回空串)
PyValue py_raft_send(const char *node_id, int64_t idLen,
                     const char *msg, int64_t msgLen);
PyValue py_raft_recv(const char *node_id, int64_t idLen);

// ---- add(box) 统一编排与容器内定位(2026-10 增补) ----
// kv.add_box(node_id, box, addr):一步完成注册节点+绑定盒子,addr 空则本地启动
PyValue py_kv_add_box(const char *node_id, int64_t idLen,
                      const PyValue *box,
                      const char *addr, int64_t addrLen);
// kv.locate(box):由盒子反查所属 kv 节点的 kv->box 绑定关系
PyValue py_kv_locate(const PyValue *box);
// kv.self():容器内定位,读 PYLITE_NODE_ID 环境变量反查所属节点
PyValue py_kv_self();

// 按 handle 取盒子登记的二进制路径(供 kv.add_box 使用)
PyValue py_sandbox_binary_path(int64_t handle);

// ---- 节点迭代器适配器(2026-10 增补) ----
// kv.iter():创建节点迭代器实例(快照当前节点列表)
PyValue py_kv_iter();
// kv.next(it):取下一个节点信息,迭代结束返回 None
PyValue py_kv_iter_next(const PyValue *it);
// kv.rewind(it):重置游标到开头,返回元素个数
PyValue py_kv_iter_rewind(const PyValue *it);
// kv.iter_destroy(it):销毁迭代器释放快照
PyValue py_kv_iter_destroy(const PyValue *it);

// server.config(secret, node_id):配置集群通信密钥与本节点ID
PyValue py_server_config(const PyValue *secret, const PyValue *node_id);
// kv.iter_live():实时迭代器,next 时实时查询节点列表
PyValue py_kv_iter_live();

// 实例方法类型化注册(迭代协议分派入口)
typedef PyValue (*PyInstanceMethodFn)(const PyValue *inst, PyValue *args, int64_t nargs);
void py_instance_register_method(const char *type_name, int64_t typeLen,
                                 const char *method, int64_t mLen,
                                 PyInstanceMethodFn fn);
}  // extern "C"
