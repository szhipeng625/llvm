#include "sandbox.h"
#include "../include/pylite/runtime.h"
#include <unordered_map>
#include <mutex>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>

// 全局沙箱配置注册表
static std::unordered_map<int64_t, SandboxConfig> g_sandbox_configs;
static std::mutex g_sandbox_mutex;
static int64_t g_next_handle = 1;

// 沙箱实例的 classId
static const int64_t SANDBOX_CLASS_ID = 2;

extern "C" {

// 创建沙箱实例，返回 PY_INSTANCE 对象
PyValue py_sandbox_create() {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    int64_t handle = g_next_handle++;
    g_sandbox_configs[handle] = SandboxConfig{};
    // 创建属性字典，存入句柄
    PyValue attrs = py_dict_new(nullptr, nullptr, 0);
    PyValue k = py_str_new("handle", 6);
    PyValue v = py_int(handle);
    py_dict_set(&attrs, &k, &v);
    // 存入类型名
    PyValue tk = py_str_new("type", 4);
    PyValue tv = py_str_new("sandbox", 7);
    py_dict_set(&attrs, &tk, &tv);
    return py_instance_new(SANDBOX_CLASS_ID, &attrs);
}

// 从沙箱实例中提取句柄
static int64_t get_handle(const PyValue* inst) {
    if (!inst || inst->tag != 9) return -1;  // PY_INSTANCE = 9
    PyValue k = py_str_new("handle", 6);
    PyValue v = py_instance_get_attr(inst, "handle", 6);
    return py_as_int(v);
}

void py_sandbox_set_cpu_limit(const PyValue* inst, uint32_t cpu_limit_ms) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.cpu_limit_ms = cpu_limit_ms;
    }
}

void py_sandbox_set_mem_limit(const PyValue* inst, uint64_t mem_limit_bytes) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.mem_limit_bytes = mem_limit_bytes;
    }
}

void py_sandbox_set_max_pids(const PyValue* inst, int64_t max_pids) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.max_pids = max_pids;
    }
}

// 关闭系统调用过滤(编译等可信阶段使用,2026-10 增补)
void py_sandbox_set_no_seccomp(const PyValue* inst, int64_t flag) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.no_seccomp = (flag != 0);
    }
}

void py_sandbox_set_work_dir(const PyValue* inst, const char *path, int64_t pathLen) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.work_dir = std::string(path, pathLen);
    }
}

// 按 handle 取盒子登记的二进制路径(供 kv.add_box 使用,2026-10 增补)
PyValue py_sandbox_binary_path(int64_t handle) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it == g_sandbox_configs.end()) return py_str_new("", 0);
    const std::string& p = it->second.user_binary;
    return py_str_new(p.c_str(), p.size());
}

void py_sandbox_set_binary(const PyValue* inst, const char *path, int64_t pathLen) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.user_binary = std::string(path, pathLen);
    }
}

void py_sandbox_add_arg(const PyValue* inst, const char *arg, int64_t argLen) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.args.push_back(std::string(arg, argLen));
    }
}

void py_sandbox_set_stdin(const PyValue* inst, const char *path, int64_t pathLen) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.stdin_file = std::string(path, pathLen);
    }
}

void py_sandbox_set_stdout(const PyValue* inst, const char *path, int64_t pathLen) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.stdout_file = std::string(path, pathLen);
    }
}

void py_sandbox_set_stderr(const PyValue* inst, const char *path, int64_t pathLen) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.stderr_file = std::string(path, pathLen);
    }
}

// 返回值约定:
//   >= 0   子进程真实退出码(0 表示成功)
//   -1     配置缺失或 fork 失败
//   -124   执行超过 cpu_limit_ms,子进程被终止(与 coreutils timeout 的 124 对齐,便于判题识别 TLE)
int64_t py_sandbox_exec(const PyValue* inst) {
    int64_t handle = get_handle(inst);
    SandboxConfig config;
    {
        std::lock_guard<std::mutex> lk(g_sandbox_mutex);
        auto it = g_sandbox_configs.find(handle);
        if (it == g_sandbox_configs.end()) {
            return -1;
        }
        config = it->second;
    }
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        sandbox_main(config);
        _exit(127);
    }
    // 等待子进程结束,带 CPU 时限看护:超时先 SIGKILL,避免判题端被死循环拖住
    int64_t limit_ms = config.cpu_limit_ms > 0 ? config.cpu_limit_ms : 0;
    int64_t waited_ms = 0;
    const int64_t step_ms = 20;
    int64_t budget_ms = limit_ms > 0 ? limit_ms + 1000 : 0;  // 无限制时无限等待
    int status = 0;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return -1;
        }
        if (r < 0) return -1;
        if (budget_ms > 0 && waited_ms >= budget_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -124;
        }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = step_ms * 1000000L;
        nanosleep(&ts, nullptr);
        waited_ms += step_ms;
    }
}

void py_sandbox_destroy(const PyValue* inst) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    g_sandbox_configs.erase(handle);
}

}  // extern "C"

// ============================================================================
// sandbox 模块参数适配器(2026-10 增补)
// 模块调用统一按 PyValue* 传参,而底层函数签名是 (inst, 标量/char*, len) 混合形式,
// 直接映射会把 PyValue* 误当成 char* 内容解读(崩溃根因)。
// 这里补一层适配器,从 PyValue 中正确提取字符串与整数。
// ============================================================================
#include "pylite/runtime.h"
#include "pylite/value.h"

static const char* sb_str(const PyValue* v) {
    if (!v || v->tag != PY_STR) return "";
    return py_str_data(v);
}
static int64_t sb_len(const PyValue* v) {
    if (!v || v->tag != PY_STR) return 0;
    return py_str_size(v);
}
static int64_t sb_int(const PyValue* v) {
    if (!v || v->tag != PY_INT) return 0;
    return v->as.i;
}

extern "C" {
PyValue py_sandbox_set_work_dir_v(const PyValue* inst, const PyValue* path) {
    py_sandbox_set_work_dir(inst, sb_str(path), sb_len(path));
    return py_none();
}
PyValue py_sandbox_set_binary_v(const PyValue* inst, const PyValue* path) {
    py_sandbox_set_binary(inst, sb_str(path), sb_len(path));
    return py_none();
}
PyValue py_sandbox_add_arg_v(const PyValue* inst, const PyValue* arg) {
    py_sandbox_add_arg(inst, sb_str(arg), sb_len(arg));
    return py_none();
}
PyValue py_sandbox_set_stdin_v(const PyValue* inst, const PyValue* path) {
    py_sandbox_set_stdin(inst, sb_str(path), sb_len(path));
    return py_none();
}
PyValue py_sandbox_set_stdout_v(const PyValue* inst, const PyValue* path) {
    py_sandbox_set_stdout(inst, sb_str(path), sb_len(path));
    return py_none();
}
PyValue py_sandbox_set_stderr_v(const PyValue* inst, const PyValue* path) {
    py_sandbox_set_stderr(inst, sb_str(path), sb_len(path));
    return py_none();
}
PyValue py_sandbox_set_cpu_limit_v(const PyValue* inst, const PyValue* ms) {
    py_sandbox_set_cpu_limit(inst, static_cast<uint32_t>(sb_int(ms)));
    return py_none();
}
PyValue py_sandbox_set_mem_limit_v(const PyValue* inst, const PyValue* bytes) {
    py_sandbox_set_mem_limit(inst, static_cast<uint64_t>(sb_int(bytes)));
    return py_none();
}
PyValue py_sandbox_set_max_pids_v(const PyValue* inst, const PyValue* n) {
    py_sandbox_set_max_pids(inst, sb_int(n));
    return py_none();
}
PyValue py_sandbox_set_no_seccomp_v(const PyValue* inst, const PyValue* flag) {
    py_sandbox_set_no_seccomp(inst, sb_int(flag));
    return py_none();
}
PyValue py_sandbox_exec_v(const PyValue* inst) {
    int64_t r = py_sandbox_exec(inst);
    return py_int(r);
}
PyValue py_sandbox_destroy_v(const PyValue* inst) {
    py_sandbox_destroy(inst);
    return py_none();
}
}  // extern "C"
