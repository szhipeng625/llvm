#pragma once
#include <string>
#include <cstdint>

class CgroupManager {
public:
    // 创建判题专用 cgroup（高 CPU 权重 200），返回 cgroup 路径
    static std::string create_judge(const std::string& task_id,
                                    uint32_t cpu_limit_ms,
                                    uint64_t mem_limit_bytes,
                                    int64_t max_pids);

    // 将指定 pid 加入 cgroup
    static void attach_pid(const std::string& cg_path, pid_t pid);

    // 读取实际资源用量
    struct Usage {
        uint64_t cpu_ns;      // CPU 使用时间（纳秒）
        uint64_t mem_bytes;   // 内存使用量（字节）
    };
    static Usage read_usage(const std::string& cg_path);

    // 销毁 cgroup 目录
    static void destroy(const std::string& cg_path);

    // 获取 cgroup 根路径（自动检测 delegate 子树根）
    static std::string get_root();

private:
    static std::string create_with_weight(const std::string& name,
                                          uint32_t cpu_weight,
                                          uint32_t cpu_limit_ms,
                                          uint64_t mem_limit_bytes,
                                          int64_t max_pids);
};
