#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <mutex>

// ============================================================================
// 节点状态
// ============================================================================
enum class KvNodeState {
    IDLE      = 0,   // 已注册但未启动
    RUNNING   = 1,   // 进程运行中
    SLEEPING  = 2,   // SIGSTOP 暂停（故障注入）
    STOPPED   = 3,   // 已停止（可重新启动）
    ERASED    = 4,   // 已永久移除
    LOST      = 5    // 逻辑跳过（副本标记丢失）
};

struct KvNode {
    std::string node_id;            // 节点唯一标识
    std::string addr;               // 监听地址（host:port）
    std::string binary;             // 可执行文件路径
    std::vector<std::string> args;  // 启动参数
    std::string work_dir;           // 工作目录
    KvNodeState state;              // 当前状态
    int64_t pid;                    // 进程 PID（-1 表示未运行）
    int64_t started_at_ms;          // 启动时间
    int64_t last_active_ms;         // 最后活跃时间
    int64_t restart_count;          // 重启次数
};

// ============================================================================
// 节点管理器
// ============================================================================
class KvAdmin {
public:
    static KvAdmin& instance();

    // ---- 节点生命周期 ----

    // 建立节点：spawn 进程并注册（幂等：已存在则返回已有节点）
    bool build(const std::string& node_id,
               const std::string& binary,
               const std::vector<std::string>& args,
               const std::string& work_dir,
               const std::string& addr,
               std::string& error);

    // 向集群追加节点（语义同 build，别名）
    bool add(const std::string& node_id,
             const std::string& binary,
             const std::vector<std::string>& args,
             const std::string& work_dir,
             const std::string& addr,
             std::string& error);

    // 永久移除节点：停止进程并从注册表删除
    bool erase(const std::string& node_id, std::string& error);

    // 逻辑跳过：标记 LOST 但保留进程（数据保留，等待副本替换）
    bool skip(const std::string& node_id, std::string& error);

    // 暂停节点：SIGSTOP（故障注入）
    bool sleep_node(const std::string& node_id, std::string& error);

    // 唤醒节点：SIGCONT
    bool wakeup(const std::string& node_id, std::string& error);

    // 停止节点但保留注册（可重新启动）
    bool stop(const std::string& node_id, std::string& error);

    // 重新启动已停止的节点
    bool restart(const std::string& node_id, std::string& error);

    // ---- 状态查询 ----

    // 获取节点信息（不存在返回 false）
    bool get_node(const std::string& node_id, KvNode& out);

    // 列出所有节点
    std::vector<KvNode> list_nodes();

    // 检查节点进程是否存活
    bool is_alive(const std::string& node_id);

    // ---- LSM 持久化 ----
    // 配置 LSM 后端地址(如 hermes 的 6380)。配置后节点变更自动写入,
    // 进程内首次使用时自动从 LSM 恢复注册表。addr 为空则关闭持久化。
    void configure_persistence(const std::string& addr);
    // 从 LSM 恢复注册表(进程启动或首次操作时调用,幂等)
    void restore_from_lsm();
    // 读取当前持久化地址(节点内置 MQ 用于定位队列存储,2026-10 增补)
    std::string persistence_addr() const { return lsm_addr_; }

private:
    KvAdmin() = default;
    ~KvAdmin() = default;
    KvAdmin(const KvAdmin&) = delete;
    KvAdmin& operator=(const KvAdmin&) = delete;

    // 内部：spawn 进程
    int64_t spawn_process(const KvNode& node, std::string& error);
    // 内部：发送信号
    bool send_signal(int64_t pid, int sig, std::string& error);
    // 内部：检查进程存活（waitpid WNOHANG）
    bool process_alive(int64_t pid);

    std::map<std::string, KvNode> nodes_;
    std::mutex mutex_;

    // ---- LSM 持久化内部状态 ----
    std::string lsm_addr_;            // LSM 后端地址(host:port),空表示未启用
    bool lsm_restored_{false};        // 是否已从 LSM 恢复过(幂等控制)
    // 将单个节点状态同步写入 LSM(需在已持有锁或变更点调用)
    void persist_node(const KvNode& node);
    // 从 LSM 删除节点记录
    void persist_erase(const std::string& node_id);
};
