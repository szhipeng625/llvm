#include "sysmon.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

// ============================================================================
// 辅助函数
// ============================================================================

// 读取文件全部内容
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.good()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 读取 /proc/[pid]/status 中指定键的值
static uint64_t read_status_kv(pid_t pid, const std::string& key) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream f(path);
    if (!f.good()) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key, 0) == 0) {
            // 格式: "Key:\tvalue kB"
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string val = line.substr(colon + 1);
            // 去掉前导空白和尾部单位
            size_t start = val.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            val = val.substr(start);
            size_t end = val.find_first_not_of("0123456789");
            if (end != std::string::npos) val = val.substr(0, end);
            try { return std::stoull(val); } catch (...) { return 0; }
        }
    }
    return 0;
}

// ============================================================================
// CPU 监控
// ============================================================================

CpuStats read_cpu_stats() {
    CpuStats stats{};
    stats.num_cores = sysconf(_SC_NPROCESSORS_ONLN);

    std::ifstream f("/proc/stat");
    if (!f.good()) return stats;

    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("cpu ", 0) == 0) {
            // 总计行: cpu  user nice system idle iowait irq softirq steal ...
            std::istringstream iss(line.substr(4));
            iss >> stats.total.user >> stats.total.nice >> stats.total.system
                >> stats.total.idle >> stats.total.iowait >> stats.total.irq
                >> stats.total.softirq >> stats.total.steal;
        } else if (line.rfind("cpu", 0) == 0 && line[3] >= '0' && line[3] <= '9') {
            // 每个核心: cpu0 user nice ...
            CpuTimes ct{};
            std::istringstream iss(line.substr(3));
            int core_id;
            iss >> core_id >> ct.user >> ct.nice >> ct.system
                >> ct.idle >> ct.iowait >> ct.irq >> ct.softirq >> ct.steal;
            stats.cores.push_back(ct);
        }
    }

    // 计算总使用率（简化：基于 total 字段）
    uint64_t total_time = stats.total.user + stats.total.nice + stats.total.system
                        + stats.total.idle + stats.total.iowait + stats.total.irq
                        + stats.total.softirq + stats.total.steal;
    uint64_t idle_time = stats.total.idle + stats.total.iowait;
    if (total_time > 0) {
        stats.usage_percent = 100.0 * (1.0 - (double)idle_time / total_time);
    }

    return stats;
}

double read_process_cpu_percent(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::string content = read_file(path);
    if (content.empty()) return -1.0;

    // /proc/[pid]/stat 格式: pid (comm) state ppid ... utime stime cutime cstime ...
    // utime 是第 14 个字段，stime 是第 15 个
    // 需要跳过括号内的 comm（可能含空格）
    size_t rparen = content.rfind(')');
    if (rparen == std::string::npos) return -1.0;

    std::istringstream iss(content.substr(rparen + 2));
    std::string state;
    int64_t ppid;
    // 跳过 state(1) ppid(2) pgrp(3) session(4) tty(5) tpgid(6) flags(7)
    // minflt(8) cminflt(9) majflt(10) cmajflt(11)
    std::string dummy;
    for (int i = 0; i < 11; i++) iss >> dummy;

    uint64_t utime, stime;
    iss >> utime >> stime;

    // CPU 时间 = (utime + stime) / sysconf(_SC_CLK_TCK) 秒
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    return (double)(utime + stime) / clk_tck * 1000.0;  // 返回毫秒
}

// ============================================================================
// 内存监控
// ============================================================================

MemInfo read_mem_info() {
    MemInfo info{};
    std::ifstream f("/proc/meminfo");
    if (!f.good()) return info;

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string key;
        uint64_t val;
        std::string unit;
        iss >> key >> val >> unit;
        key = key.substr(0, key.size() - 1);  // 去掉末尾冒号

        if (key == "MemTotal") info.total_kb = val;
        else if (key == "MemFree") info.free_kb = val;
        else if (key == "MemAvailable") info.available_kb = val;
        else if (key == "Buffers") info.buffers_kb = val;
        else if (key == "Cached") info.cached_kb = val;
        else if (key == "SwapTotal") info.swap_total_kb = val;
        else if (key == "SwapFree") info.swap_free_kb = val;
    }

    if (info.total_kb > 0) {
        info.usage_percent = 100.0 * (1.0 - (double)info.available_kb / info.total_kb);
    }

    return info;
}

ProcMemInfo read_process_mem(pid_t pid) {
    ProcMemInfo info{};
    info.vm_peak_kb = read_status_kv(pid, "VmPeak");
    info.vm_size_kb = read_status_kv(pid, "VmSize");
    info.vm_rss_kb  = read_status_kv(pid, "VmRSS");
    info.vm_data_kb = read_status_kv(pid, "VmData");
    info.vm_stack_kb = read_status_kv(pid, "VmStk");
    return info;
}

// ============================================================================
// 进程监控
// ============================================================================

ProcInfo read_process_info(pid_t pid) {
    ProcInfo info{};
    info.pid = pid;

    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    std::string content = read_file(path);
    if (content.empty()) return info;

    size_t rparen = content.rfind(')');
    if (rparen == std::string::npos) return info;

    // 提取 comm（括号内）
    size_t lparen = content.find('(');
    if (lparen != std::string::npos && lparen < rparen) {
        info.name = content.substr(lparen + 1, rparen - lparen - 1);
    }

    std::istringstream iss(content.substr(rparen + 2));
    iss >> info.state;
    iss >> info.ppid;

    // 跳过 pgrp session tty tpgid flags minflt cminflt majflt cmajflt
    std::string dummy;
    for (int i = 0; i < 10; i++) iss >> dummy;

    uint64_t utime, stime;
    iss >> utime >> stime;
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    info.cpu_time_ns = (utime + stime) * (1000000000ULL / clk_tck);

    // 跳过 cutime cstime priority nice
    for (int i = 0; i < 4; i++) iss >> dummy;

    iss >> info.num_threads;

    // 读取 RSS
    info.mem_rss_kb = read_status_kv(pid, "VmRSS");

    // 读取子进程
    std::string children_path = "/proc/" + std::to_string(pid) + "/task/"
                               + std::to_string(pid) + "/children";
    std::string children_str = read_file(children_path);
    if (!children_str.empty()) {
        std::istringstream ciss(children_str);
        pid_t child;
        while (ciss >> child) {
            info.children.push_back(child);
        }
    }

    return info;
}

std::vector<pid_t> list_processes() {
    std::vector<pid_t> pids;
    DIR* dir = opendir("/proc");
    if (!dir) return pids;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            pid_t pid = 0;
            bool is_num = true;
            for (const char* p = entry->d_name; *p; ++p) {
                if (*p < '0' || *p > '9') { is_num = false; break; }
                pid = pid * 10 + (*p - '0');
            }
            if (is_num && pid > 0) {
                pids.push_back(pid);
            }
        }
    }
    closedir(dir);
    return pids;
}

std::vector<ProcInfo> read_process_tree(pid_t root_pid) {
    std::vector<ProcInfo> tree;
    ProcInfo root = read_process_info(root_pid);
    if (root.pid == 0) return tree;
    tree.push_back(root);

    // BFS 遍历子进程
    for (size_t i = 0; i < tree.size(); ++i) {
        for (pid_t child_pid : tree[i].children) {
            ProcInfo child = read_process_info(child_pid);
            if (child.pid != 0) {
                tree.push_back(child);
            }
        }
    }

    return tree;
}

TreeUsage read_tree_usage(pid_t root_pid) {
    TreeUsage usage{};
    auto tree = read_process_tree(root_pid);
    for (const auto& p : tree) {
        usage.total_cpu_ns += p.cpu_time_ns;
        usage.total_mem_kb += p.mem_rss_kb;
        usage.total_pids++;
    }
    return usage;
}

// ============================================================================
// 网络监控
// ============================================================================

std::vector<NetDevInfo> read_net_stats() {
    std::vector<NetDevInfo> devs;
    std::ifstream f("/proc/net/dev");
    if (!f.good()) return devs;

    std::string line;
    // 跳过前两行（标题）
    std::getline(f, line);
    std::getline(f, line);

    while (std::getline(f, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        NetDevInfo dev;
        dev.name = line.substr(0, colon);
        // 去掉前导空白
        size_t start = dev.name.find_first_not_of(" \t");
        if (start != std::string::npos) dev.name = dev.name.substr(start);

        std::istringstream iss(line.substr(colon + 1));
        iss >> dev.rx_bytes >> dev.rx_packets;
        // 跳过 err drop fifo frame compressed multicast
        for (int i = 0; i < 6; i++) { uint64_t d; iss >> d; }
        iss >> dev.tx_bytes >> dev.tx_packets;

        devs.push_back(dev);
    }

    return devs;
}

bool check_net_namespace(pid_t pid, const std::string& ns_path) {
    std::string proc_ns = "/proc/" + std::to_string(pid) + "/ns/net";
    std::string proc_link = read_file(proc_ns);
    // 简化：检查 /proc/[pid]/ns/net 是否存在
    return !proc_link.empty();
}

// ============================================================================
// 文件系统监控
// ============================================================================

FsQuota read_fs_quota(const std::string& path) {
    FsQuota quota{};
    quota.mount_point = path;

    struct statvfs st{};
    if (statvfs(path.c_str(), &st) != 0) return quota;

    quota.total_bytes = st.f_blocks * st.f_frsize;
    quota.available_bytes = st.f_bavail * st.f_frsize;
    quota.used_bytes = quota.total_bytes - st.f_bfree * st.f_frsize;
    quota.inodes_total = st.f_files;
    quota.inodes_used = st.f_files - st.f_ffree;

    if (quota.total_bytes > 0) {
        quota.usage_percent = 100.0 * (double)quota.used_bytes / quota.total_bytes;
    }

    return quota;
}

uint64_t dir_total_size(const std::string& path) {
    // 简化实现：使用 du -sb 命令
    std::string cmd = "du -sb " + path + " 2>/dev/null | cut -f1";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return 0;
    uint64_t size = 0;
    char buf[64];
    if (fgets(buf, sizeof(buf), fp)) {
        size = strtoull(buf, nullptr, 10);
    }
    pclose(fp);
    return size;
}

int64_t dir_file_count(const std::string& path) {
    std::string cmd = "find " + path + " -type f 2>/dev/null | wc -l";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return -1;
    int64_t count = -1;
    char buf[64];
    if (fgets(buf, sizeof(buf), fp)) {
        count = strtoll(buf, nullptr, 10);
    }
    pclose(fp);
    return count;
}

// ============================================================================
// 系统信息
// ============================================================================

SysInfo read_sys_info() {
    SysInfo info{};

    // hostname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        info.hostname = hostname;
    }

    // kernel version
    struct utsname uts;
    if (uname(&uts) == 0) {
        info.kernel_version = std::string(uts.sysname) + " " + uts.release;
    }

    // os release
    std::string os_release = read_file("/etc/os-release");
    std::istringstream oss(os_release);
    std::string line;
    while (std::getline(oss, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            info.os_release = line.substr(12);
            // 去掉引号
            if (info.os_release.size() >= 2 && info.os_release.front() == '"') {
                info.os_release = info.os_release.substr(1, info.os_release.size() - 2);
            }
            break;
        }
    }

    // uptime
    std::string uptime_str = read_file("/proc/uptime");
    if (!uptime_str.empty()) {
        std::istringstream uiss(uptime_str);
        double uptime;
        uiss >> uptime;
        info.uptime_seconds = static_cast<int64_t>(uptime);
    }

    // num cpus
    info.num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    // total mem
    info.total_mem_kb = 0;
    std::ifstream memf("/proc/meminfo");
    if (memf.good()) {
        std::string mline;
        while (std::getline(memf, mline)) {
            if (mline.rfind("MemTotal:", 0) == 0) {
                std::istringstream miss(mline.substr(9));
                miss >> info.total_mem_kb;
                break;
            }
        }
    }

    return info;
}

double read_load_avg_1min() {
    std::string content = read_file("/proc/loadavg");
    if (content.empty()) return -1.0;
    std::istringstream iss(content);
    double load;
    iss >> load;
    return load;
}

double read_load_avg_5min() {
    std::string content = read_file("/proc/loadavg");
    if (content.empty()) return -1.0;
    std::istringstream iss(content);
    double l1, l5;
    iss >> l1 >> l5;
    return l5;
}

double read_load_avg_15min() {
    std::string content = read_file("/proc/loadavg");
    if (content.empty()) return -1.0;
    std::istringstream iss(content);
    double l1, l5, l15;
    iss >> l1 >> l5 >> l15;
    return l15;
}
