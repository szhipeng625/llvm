#include "sandbox.h"
#include <unordered_map>
#include <mutex>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>

// 全局沙箱配置注册表
static std::unordered_map<int64_t, SandboxConfig> g_sandbox_configs;
static std::mutex g_sandbox_mutex;
static int64_t g_next_handle = 1;

extern "C" {

int64_t py_sandbox_create() {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    int64_t handle = g_next_handle++;
    g_sandbox_configs[handle] = SandboxConfig{};
    return handle;
}

void py_sandbox_set_cpu_limit(int64_t handle, uint32_t cpu_limit_ms) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.cpu_limit_ms = cpu_limit_ms;
    }
}

void py_sandbox_set_mem_limit(int64_t handle, uint64_t mem_limit_bytes) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.mem_limit_bytes = mem_limit_bytes;
    }
}

void py_sandbox_set_max_pids(int64_t handle, int64_t max_pids) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.max_pids = max_pids;
    }
}

void py_sandbox_set_work_dir(int64_t handle, const char *path, int64_t pathLen) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.work_dir = std::string(path, pathLen);
    }
}

void py_sandbox_set_binary(int64_t handle, const char *path, int64_t pathLen) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.user_binary = std::string(path, pathLen);
    }
}

void py_sandbox_add_arg(int64_t handle, const char *arg, int64_t argLen) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.args.push_back(std::string(arg, argLen));
    }
}

void py_sandbox_set_stdin(int64_t handle, const char *path, int64_t pathLen) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.stdin_file = std::string(path, pathLen);
    }
}

void py_sandbox_set_stdout(int64_t handle, const char *path, int64_t pathLen) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.stdout_file = std::string(path, pathLen);
    }
}

void py_sandbox_set_stderr(int64_t handle, const char *path, int64_t pathLen) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    auto it = g_sandbox_configs.find(handle);
    if (it != g_sandbox_configs.end()) {
        it->second.stderr_file = std::string(path, pathLen);
    }
}

int64_t py_sandbox_exec(int64_t handle) {
    SandboxConfig config;
    {
        std::lock_guard<std::mutex> lk(g_sandbox_mutex);
        auto it = g_sandbox_configs.find(handle);
        if (it == g_sandbox_configs.end()) {
            return -1;  // 无效句柄
        }
        config = it->second;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;  // fork 失败
    }
    if (pid == 0) {
        // 子进程：进入沙箱并执行用户程序
        sandbox_main(config);
        // sandbox_main 不返回
    }
    // 父进程：返回子进程 pid
    return static_cast<int64_t>(pid);
}

void py_sandbox_destroy(int64_t handle) {
    std::lock_guard<std::mutex> lk(g_sandbox_mutex);
    g_sandbox_configs.erase(handle);
}

}  // extern "C"
