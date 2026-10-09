#pragma once
#include <string>
#include <cstdint>
#include <vector>

// ============================================================================
// CPU 监控
// ============================================================================
struct CpuTimes {
    uint64_t user;
    uint64_t nice;
    uint64_t system;
    uint64_t idle;
    uint64_t iowait;
    uint64_t irq;
    uint64_t softirq;
    uint64_t steal;
};

struct CpuStats {
    CpuTimes total;              // 所有 CPU 合计
    std::vector<CpuTimes> cores; // 每个核心单独统计
    double usage_percent;        // 自上次调用以来的 CPU 使用率 (0-100)
    int64_t num_cores;           // 逻辑核心数
};

// 读取 /proc/stat 获取 CPU 时间统计
CpuStats read_cpu_stats();

// 获取当前进程的 CPU 使用率（需要两次调用间计算差值）
double read_process_cpu_percent(pid_t pid);

// ============================================================================
// 内存监控
// ============================================================================
struct MemInfo {
    uint64_t total_kb;
    uint64_t free_kb;
    uint64_t available_kb;
    uint64_t buffers_kb;
    uint64_t cached_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
    double usage_percent;        // 内存使用率 (0-100)
};

// 读取 /proc/meminfo 获取系统内存信息
MemInfo read_mem_info();

// 读取 /proc/[pid]/status 获取进程内存使用
struct ProcMemInfo {
    uint64_t vm_peak_kb;         // 虚拟内存峰值
    uint64_t vm_size_kb;         // 虚拟内存当前
    uint64_t vm_rss_kb;          // 物理内存（RSS）
    uint64_t vm_data_kb;         // 数据段
    uint64_t vm_stack_kb;        // 栈
};
ProcMemInfo read_process_mem(pid_t pid);

// ============================================================================
// 进程监控
// ============================================================================
struct ProcInfo {
    pid_t pid;
    pid_t ppid;
    std::string name;
    std::string state;           // R/S/D/Z/T 等
    uint64_t cpu_time_ns;        // CPU 时间（纳秒）
    uint64_t mem_rss_kb;         // 物理内存
    int64_t num_threads;         // 线程数
    std::vector<pid_t> children; // 子进程列表
};

// 读取 /proc/[pid]/stat 和 /proc/[pid]/status 获取进程信息
ProcInfo read_process_info(pid_t pid);

// 遍历 /proc 获取所有进程列表
std::vector<pid_t> list_processes();

// 获取进程树（当前进程及其所有子孙进程）
std::vector<ProcInfo> read_process_tree(pid_t root_pid);

// 统计进程树的总资源用量
struct TreeUsage {
    uint64_t total_cpu_ns;
    uint64_t total_mem_kb;
    int64_t total_pids;
};
TreeUsage read_tree_usage(pid_t root_pid);

// ============================================================================
// 网络监控
// ============================================================================
struct NetDevInfo {
    std::string name;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
};

// 读取 /proc/net/dev 获取网络接口统计
std::vector<NetDevInfo> read_net_stats();

// 检查进程是否在指定网络命名空间中
bool check_net_namespace(pid_t pid, const std::string& ns_path);

// ============================================================================
// 文件系统监控
// ============================================================================
struct FsQuota {
    std::string mount_point;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t available_bytes;
    uint64_t inodes_total;
    uint64_t inodes_used;
    double usage_percent;
};

// 读取 statfs/statvfs 获取文件系统配额
FsQuota read_fs_quota(const std::string& path);

// 统计目录下文件总大小（递归）
uint64_t dir_total_size(const std::string& path);

// 统计目录下文件数量（递归）
int64_t dir_file_count(const std::string& path);

// ============================================================================
// 系统信息
// ============================================================================
struct SysInfo {
    std::string hostname;
    std::string kernel_version;
    std::string os_release;
    int64_t uptime_seconds;
    int64_t num_cpus;
    uint64_t total_mem_kb;
};

// 读取 /proc/version、/proc/uptime、uname 等获取系统信息
SysInfo read_sys_info();

// 读取 /proc/loadavg 获取系统负载
double read_load_avg_1min();
double read_load_avg_5min();
double read_load_avg_15min();
