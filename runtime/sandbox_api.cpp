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

void py_sandbox_set_work_dir(const PyValue* inst, const char *path, int64_t pathLen) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.work_dir = std::string(path, pathLen);
    }
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
    }
    return static_cast<int64_t>(pid);
}

void py_sandbox_destroy(const PyValue* inst) {
    int64_t handle = get_handle(inst);
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    g_sandbox_configs.erase(handle);
}

}  // extern "C"
