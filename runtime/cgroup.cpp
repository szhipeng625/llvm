#include "cgroup.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

// 自动检测 cgroup 根路径
// 优先查找 cloud_judge.service 的 delegate cgroup，回退到 /proc/self/cgroup
std::string CgroupManager::get_root() {
    // 尝试常见的 delegate cgroup 路径
    const char* candidates[] = {
        "/sys/fs/cgroup/system.slice/cloud_judge.service",
        "/sys/fs/cgroup/system.slice/cloud_judge_api.service",
        nullptr
    };

    for (int i = 0; candidates[i] != nullptr; ++i) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            // 验证是否支持 subtree_control
            std::string ctrl_path = std::string(candidates[i]) + "/cgroup.subtree_control";
            std::ifstream ctrl(ctrl_path);
            if (ctrl.good()) {
                return candidates[i];
            }
        }
    }

    // 回退：从 /proc/self/cgroup 解析
    std::ifstream self_cg("/proc/self/cgroup");
    if (!self_cg.good()) {
        throw std::runtime_error("cannot detect cgroup root: /proc/self/cgroup not readable");
    }

    std::string line;
    while (std::getline(self_cg, line)) {
        // 格式: 0::/system.slice/xxx.service/...
        size_t pos = line.find("0::");
        if (pos == 0) {
            std::string path = line.substr(3);
            // 取到 .service 层级
            size_t svc = path.find(".service");
            if (svc != std::string::npos) {
                path = path.substr(0, svc + 8); // ".service" 长度 8
                std::string full = "/sys/fs/cgroup" + path;
                struct stat st;
                if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    return full;
                }
            }
        }
    }

    throw std::runtime_error("cannot detect cgroup root from /proc/self/cgroup");
}

std::string CgroupManager::create_with_weight(const std::string& name,
                                               uint32_t cpu_weight,
                                               uint32_t cpu_limit_ms,
                                               uint64_t mem_limit_bytes,
                                               int64_t max_pids) {
    std::string root = get_root();
    std::string cg_path = root + "/" + name;

    if (mkdir(cg_path.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("mkdir " + cg_path + " failed: " + strerror(errno));
    }

    // 设置 CPU 权重
    std::string weight_path = cg_path + "/cpu.weight";
    std::ofstream weight_f(weight_path);
    if (weight_f.good()) {
        weight_f << cpu_weight;
        weight_f.close();
    }

    // 设置 CPU 硬限制（cpu.max: "$MAX $PERIOD"，单位微秒）
    if (cpu_limit_ms > 0) {
        std::string max_path = cg_path + "/cpu.max";
        std::ofstream max_f(max_path);
        if (max_f.good()) {
            max_f << (cpu_limit_ms * 1000) << " 1000000";
            max_f.close();
        }
    }

    // 设置内存限制
    if (mem_limit_bytes > 0) {
        std::string mem_path = cg_path + "/memory.max";
        std::ofstream mem_f(mem_path);
        if (mem_f.good()) {
            mem_f << mem_limit_bytes;
            mem_f.close();
        }
    }

    // 设置 PID 限制（防进程炸弹）
    if (max_pids > 0) {
        std::string pids_path = cg_path + "/pids.max";
        std::ofstream pids_f(pids_path);
        if (pids_f.good()) {
            pids_f << max_pids;
            pids_f.close();
        }
    }

    return cg_path;
}

std::string CgroupManager::create_judge(const std::string& task_id,
                                         uint32_t cpu_limit_ms,
                                         uint64_t mem_limit_bytes,
                                         int64_t max_pids) {
    return create_with_weight("judge_" + task_id, 200, cpu_limit_ms, mem_limit_bytes, max_pids);
}

void CgroupManager::attach_pid(const std::string& cg_path, pid_t pid) {
    std::string procs_path = cg_path + "/cgroup.procs";
    std::ofstream procs(procs_path);
    if (!procs.good()) {
        throw std::runtime_error("cannot open " + procs_path);
    }
    procs << pid;
    procs.close();
}

CgroupManager::Usage CgroupManager::read_usage(const std::string& cg_path) {
    Usage usage = {0, 0};

    // 读取 CPU 使用时间（cpu.stat 中的 usage_usec）
    std::ifstream cpu_stat(cg_path + "/cpu.stat");
    if (cpu_stat.good()) {
        std::string line;
        while (std::getline(cpu_stat, line)) {
            if (line.rfind("usage_usec", 0) == 0) {
                // 格式: usage_usec 123456
                size_t space = line.find(' ');
                if (space != std::string::npos) {
                    usage.cpu_ns = std::stoull(line.substr(space + 1)) * 1000;
                }
                break;
            }
        }
    }

    // 读取内存使用量（优先 memory.peak，回退 memory.current）
    std::ifstream mem_peak(cg_path + "/memory.peak");
    if (mem_peak.good()) {
        mem_peak >> usage.mem_bytes;
    } else {
        std::ifstream mem_cur(cg_path + "/memory.current");
        if (mem_cur.good()) {
            mem_cur >> usage.mem_bytes;
        }
    }

    return usage;
}

void CgroupManager::destroy(const std::string& cg_path) {
    rmdir(cg_path.c_str());
}
