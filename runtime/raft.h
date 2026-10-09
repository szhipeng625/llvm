#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <functional>
#include <mutex>

// 全局节点注册表（extern 声明，定义在 raft.cpp）
extern std::map<std::string, class RaftNode*> g_nodes;
extern std::mutex g_nodes_mutex;

// ============================================================================
// Raft 节点配置
// ============================================================================
struct RaftNodeConfig {
    std::string node_id;           // 节点唯一标识
    std::string bind_addr;         // 绑定地址（如 0.0.0.0:9000）
    std::string cluster_secret;    // 集群通信密钥
    std::vector<std::string> peers; // 初始集群节点列表（addr:port）
    std::string data_dir;          // 持久化数据目录
    int64_t election_timeout_ms;   // 选举超时（默认 150ms）
    int64_t heartbeat_interval_ms; // 心跳间隔（默认 50ms）
    int64_t snapshot_interval;     // 快照间隔（日志条数，0 禁用）

    RaftNodeConfig()
        : election_timeout_ms(150)
        , heartbeat_interval_ms(50)
        , snapshot_interval(10000) {}
};

// ============================================================================
// Raft 节点状态
// ============================================================================
enum class RaftRole {
    FOLLOWER  = 0,
    CANDIDATE = 1,
    LEADER    = 2
};

struct RaftNodeStatus {
    std::string node_id;
    RaftRole role;
    int64_t term;
    std::string leader_id;
    int64_t commit_index;
    int64_t last_applied;
    int64_t num_peers;
    int64_t uptime_ms;
    std::vector<std::string> peer_list;
};

// ============================================================================
// 状态机命令
// ============================================================================
struct RaftCommand {
    std::string type;              // 命令类型
    std::string payload;           // JSON 负载
    std::string from_node;         // 发起节点
    int64_t timestamp_ms;          // 时间戳
};

// 状态机命令处理器类型
using CommandHandler = std::function<std::string(const RaftCommand&)>;

// ============================================================================
// 集群节点信息
// ============================================================================
struct ClusterNode {
    std::string node_id;
    std::string addr;
    std::string role;              // leader / follower / candidate
    std::string status;            // online / offline
    int64_t last_heartbeat_ms;
    int64_t term;
};

// ============================================================================
// 动态插件信息
// ============================================================================
struct PluginInfo {
    std::string name;
    std::string path;              // .so 文件路径
    std::string version;
    bool loaded;
    int64_t load_time_ms;
};

// ============================================================================
// Raft 节点类
// ============================================================================
class RaftNode {
public:
    // 构建并启动节点
    static RaftNode* build(const RaftNodeConfig& config);

    // 销毁节点
    static void erase(const std::string& node_id);

    // 获取节点状态
    static RaftNodeStatus status(const std::string& node_id);

    // 列出所有本地节点
    static std::vector<std::string> list_nodes();

    // ---- 状态机命令 ----
    // 提交命令到集群（仅 leader 可提交，follower 自动转发）
    std::string propose(const std::string& cmd_type, const std::string& payload);

    // 注册命令处理器
    void register_handler(const std::string& cmd_type, CommandHandler handler);

    // ---- 节点管理 ----
    // 注册节点角色
    void register_node_role(const std::string& role, const std::string& addr);

    // 获取集群节点列表
    std::vector<ClusterNode> get_cluster_nodes();

    // 注册共享存储
    void mount_storage(const std::string& name, const std::string& path);

    // ---- 动态插件 ----
    // 加载插件（集群同步）
    bool load_plugin(const std::string& name, const std::string& so_path);

    // 卸载插件
    bool unload_plugin(const std::string& name);

    // 列出已加载插件
    std::vector<PluginInfo> list_plugins();

    // ---- 内部 ----
    std::string get_node_id() const { return config_.node_id; }
    RaftRole get_role() const { return role_; }
    bool is_leader() const { return role_ == RaftRole::LEADER; }

private:
    RaftNode(const RaftNodeConfig& config);
    ~RaftNode();

    // 禁止拷贝
    RaftNode(const RaftNode&) = delete;
    RaftNode& operator=(const RaftNode&) = delete;

    // 内部方法
    void start_election_timer();
    void start_heartbeat_timer();
    void send_heartbeat();
    void request_votes();
    void append_entries(const std::string& peer);
    void apply_committed();
    std::string forward_to_leader(const std::string& cmd_type, const std::string& payload);

    RaftNodeConfig config_;
    RaftRole role_;
    int64_t term_;
    std::string voted_for_;
    std::string leader_id_;
    int64_t commit_index_;
    int64_t last_applied_;
    int64_t start_time_ms_;

    // 日志条目
    struct LogEntry {
        int64_t index;
        int64_t term;
        RaftCommand cmd;
    };
    std::vector<LogEntry> log_;

    // 命令处理器注册表
    std::map<std::string, CommandHandler> handlers_;

    // 集群节点信息
    std::map<std::string, ClusterNode> cluster_nodes_;

    // 插件注册表
    std::map<std::string, PluginInfo> plugins_;

    // 共享存储
    std::map<std::string, std::string> storage_mounts_;

    std::mutex mutex_;
};
