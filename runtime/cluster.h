#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <map>
#include <mutex>
#include <functional>

// ============================================================================
// 集群连接配置
// ============================================================================
struct ClusterConnection {
    std::string cluster_name;      // 集群别名（如 "cluster-a"）
    std::string remote_addr;       // 远程地址（如 "192.168.1.10:9000"）
    std::string secret;            // 通信密钥
    int64_t timeout_ms;            // 连接超时（毫秒）
    bool connected;                // 是否已连接
    int64_t connected_at_ms;       // 连接建立时间
    int64_t last_heartbeat_ms;     // 最后心跳时间

    ClusterConnection()
        : timeout_ms(5000)
        , connected(false)
        , connected_at_ms(0)
        , last_heartbeat_ms(0) {}
};

// ============================================================================
// 集群节点信息
// ============================================================================
struct ClusterNodeInfo {
    std::string node_id;
    std::string addr;
    std::string role;              // leader / follower / candidate
    std::string status;            // online / offline
    int64_t term;
    int64_t commit_index;
    int64_t uptime_ms;
};

// ============================================================================
// 集群状态快照
// ============================================================================
struct ClusterStatus {
    std::string cluster_name;
    bool connected;
    std::string leader_id;
    int64_t term;
    int64_t num_nodes;
    std::string health;            // "ok" / "degraded" / "down"
    std::string error;             // 错误信息（健康检查失败时）
    std::vector<ClusterNodeInfo> nodes;
    int64_t latency_ms;            // 到集群的网络延迟
};

// ============================================================================
// 远程执行结果
// ============================================================================
struct RemoteResult {
    bool success;
    int64_t status_code;           // HTTP 状态码
    std::string body;              // 响应体
    int64_t elapsed_ms;            // 耗时
    std::string error;             // 错误信息
};

// ============================================================================
// 集群管理台
// ============================================================================
class ClusterManager {
public:
    // 获取单例
    static ClusterManager& instance();

    // ---- 连接管理 ----

    // 连接到远程集群
    bool connect(const std::string& cluster_name,
                 const std::string& remote_addr,
                 const std::string& secret);

    // 断开集群连接
    bool disconnect(const std::string& cluster_name);

    // 检查是否已连接
    bool is_connected(const std::string& cluster_name);

    // 列出所有已连接集群
    std::vector<std::string> list_clusters();

    // ---- 状态查询 ----

    // 获取集群状态
    ClusterStatus get_status(const std::string& cluster_name);

    // 获取集群节点列表
    std::vector<ClusterNodeInfo> get_nodes(const std::string& cluster_name);

    // 健康检查（ping）
    bool health_check(const std::string& cluster_name);

    // ---- 远程执行 ----

    // 向集群发送命令（HTTP POST）
    RemoteResult exec(const std::string& cluster_name,
                      const std::string& cmd_type,
                      const std::string& payload);

    // 向集群发送 GET 请求
    RemoteResult query(const std::string& cluster_name,
                       const std::string& path);

    // ---- 批量操作 ----

    // 向所有已连接集群广播命令
    std::map<std::string, RemoteResult> broadcast(const std::string& cmd_type,
                                                   const std::string& payload);

    // 在所有集群上执行健康检查
    std::map<std::string, bool> health_check_all();

private:
    ClusterManager() = default;
    ~ClusterManager() = default;
    ClusterManager(const ClusterManager&) = delete;
    ClusterManager& operator=(const ClusterManager&) = delete;

    // 内部 HTTP 请求
    RemoteResult http_post(const std::string& url, const std::string& body,
                           const std::string& secret, int64_t timeout_ms);
    RemoteResult http_get(const std::string& url, const std::string& secret,
                          int64_t timeout_ms);

    // 解析集群状态 JSON
    ClusterStatus parse_status_json(const std::string& cluster_name,
                                     const std::string& json);

    std::map<std::string, ClusterConnection> connections_;
    std::mutex mutex_;
};
