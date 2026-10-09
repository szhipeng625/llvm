#include "raft.h"
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <random>
#include <sstream>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

// ============================================================================
// 全局节点注册表
// ============================================================================
std::map<std::string, RaftNode*> g_nodes;
std::mutex g_nodes_mutex;

// ============================================================================
// 辅助函数
// ============================================================================

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int64_t random_election_timeout(int64_t base_ms) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<int64_t> dist(base_ms, base_ms * 2);
    return dist(gen);
}

static std::string role_str(RaftRole r) {
    switch (r) {
        case RaftRole::FOLLOWER:  return "follower";
        case RaftRole::CANDIDATE: return "candidate";
        case RaftRole::LEADER:    return "leader";
    }
    return "unknown";
}

// ============================================================================
// RaftNode 实现
// ============================================================================

RaftNode::RaftNode(const RaftNodeConfig& config)
    : config_(config)
    , role_(RaftRole::FOLLOWER)
    , term_(0)
    , commit_index_(0)
    , last_applied_(0)
    , start_time_ms_(now_ms()) {

    // 确保数据目录存在
    if (!config_.data_dir.empty()) {
        mkdir(config_.data_dir.c_str(), 0755);
    }

    // 初始化集群节点列表
    for (const auto& peer : config_.peers) {
        ClusterNode node;
        node.node_id = peer;
        node.addr = peer;
        node.role = "follower";
        node.status = "online";
        node.last_heartbeat_ms = now_ms();
        node.term = 0;
        cluster_nodes_[peer] = node;
    }

    // 把自己也加入节点列表
    ClusterNode self;
    self.node_id = config_.node_id;
    self.addr = config_.bind_addr;
    self.role = "follower";
    self.status = "online";
    self.last_heartbeat_ms = now_ms();
    self.term = 0;
    cluster_nodes_[config_.node_id] = self;

    std::cout << "[raft] node " << config_.node_id << " created, bind="
              << config_.bind_addr << ", peers=" << config_.peers.size() << std::endl;
}

RaftNode::~RaftNode() {
    std::cout << "[raft] node " << config_.node_id << " destroyed" << std::endl;
}

// ---- 静态方法 ----

RaftNode* RaftNode::build(const RaftNodeConfig& config) {
    std::lock_guard<std::mutex> lk(g_nodes_mutex);

    // 检查是否已存在
    if (g_nodes.find(config.node_id) != g_nodes.end()) {
        std::cerr << "[raft] node " << config.node_id << " already exists" << std::endl;
        return g_nodes[config.node_id];
    }

    RaftNode* node = new RaftNode(config);
    g_nodes[config.node_id] = node;

    // 启动选举定时器（在后台线程中）
    std::thread([node]() {
        while (true) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(random_election_timeout(node->config_.election_timeout_ms)));

            std::lock_guard<std::mutex> lk(node->mutex_);
            if (node->role_ == RaftRole::FOLLOWER) {
                // 超时未收到心跳，转为候选者
                node->role_ = RaftRole::CANDIDATE;
                node->term_++;
                node->voted_for_ = node->config_.node_id;
                std::cout << "[raft] " << node->config_.node_id
                          << " became candidate, term=" << node->term_ << std::endl;

                // 模拟选举：如果没有其他节点，直接成为 leader
                if (node->config_.peers.empty()) {
                    node->role_ = RaftRole::LEADER;
                    node->leader_id_ = node->config_.node_id;
                    std::cout << "[raft] " << node->config_.node_id
                              << " became leader (single node), term=" << node->term_ << std::endl;
                }
            } else if (node->role_ == RaftRole::CANDIDATE) {
                // 选举超时，重新发起
                node->term_++;
                std::cout << "[raft] " << node->config_.node_id
                          << " election timeout, new term=" << node->term_ << std::endl;
            }
        }
    }).detach();

    return node;
}

void RaftNode::erase(const std::string& node_id) {
    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(node_id);
    if (it != g_nodes.end()) {
        delete it->second;
        g_nodes.erase(it);
        std::cout << "[raft] node " << node_id << " erased" << std::endl;
    }
}

RaftNodeStatus RaftNode::status(const std::string& node_id) {
    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    auto it = g_nodes.find(node_id);
    if (it == g_nodes.end()) {
        RaftNodeStatus st{};
        st.node_id = node_id;
        st.role = RaftRole::FOLLOWER;
        return st;
    }

    RaftNode* node = it->second;
    std::lock_guard<std::mutex> nl(node->mutex_);

    RaftNodeStatus st;
    st.node_id = node->config_.node_id;
    st.role = node->role_;
    st.term = node->term_;
    st.leader_id = node->leader_id_;
    st.commit_index = node->commit_index_;
    st.last_applied = node->last_applied_;
    st.num_peers = static_cast<int64_t>(node->config_.peers.size());
    st.uptime_ms = now_ms() - node->start_time_ms_;
    st.peer_list = node->config_.peers;
    return st;
}

std::vector<std::string> RaftNode::list_nodes() {
    std::lock_guard<std::mutex> lk(g_nodes_mutex);
    std::vector<std::string> ids;
    for (const auto& [id, _] : g_nodes) {
        ids.push_back(id);
    }
    return ids;
}

// ---- 状态机命令 ----

std::string RaftNode::propose(const std::string& cmd_type, const std::string& payload) {
    std::lock_guard<std::mutex> lk(mutex_);

    if (role_ != RaftRole::LEADER) {
        // 如果不是 leader，尝试转发
        if (!leader_id_.empty() && leader_id_ != config_.node_id) {
            return forward_to_leader(cmd_type, payload);
        }
        return "{\"error\":\"not leader\",\"leader\":\"" + leader_id_ + "\"}";
    }

    // 构建日志条目
    LogEntry entry;
    entry.index = static_cast<int64_t>(log_.size()) + 1;
    entry.term = term_;
    entry.cmd.type = cmd_type;
    entry.cmd.payload = payload;
    entry.cmd.from_node = config_.node_id;
    entry.cmd.timestamp_ms = now_ms();
    log_.push_back(entry);
    commit_index_ = entry.index;

    // 应用命令
    auto handler_it = handlers_.find(cmd_type);
    if (handler_it != handlers_.end()) {
        return handler_it->second(entry.cmd);
    }

    return "{\"status\":\"committed\",\"index\":" + std::to_string(entry.index) + "}";
}

void RaftNode::register_handler(const std::string& cmd_type, CommandHandler handler) {
    std::lock_guard<std::mutex> lk(mutex_);
    handlers_[cmd_type] = handler;
}

// ---- 节点管理 ----

void RaftNode::register_node_role(const std::string& role, const std::string& addr) {
    std::lock_guard<std::mutex> lk(mutex_);
    ClusterNode node;
    node.node_id = addr;
    node.addr = addr;
    node.role = role;
    node.status = "online";
    node.last_heartbeat_ms = now_ms();
    node.term = term_;
    cluster_nodes_[addr] = node;
    std::cout << "[raft] registered node " << addr << " as " << role << std::endl;
}

std::vector<ClusterNode> RaftNode::get_cluster_nodes() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<ClusterNode> nodes;
    for (const auto& [_, node] : cluster_nodes_) {
        nodes.push_back(node);
    }
    return nodes;
}

void RaftNode::mount_storage(const std::string& name, const std::string& path) {
    std::lock_guard<std::mutex> lk(mutex_);
    storage_mounts_[name] = path;
    std::cout << "[raft] mounted storage " << name << " at " << path << std::endl;
}

// ---- 动态插件 ----

bool RaftNode::load_plugin(const std::string& name, const std::string& so_path) {
    std::lock_guard<std::mutex> lk(mutex_);

    // 检查是否已加载
    if (plugins_.find(name) != plugins_.end()) {
        std::cerr << "[raft] plugin " << name << " already loaded" << std::endl;
        return false;
    }

    // 检查 .so 文件是否存在
    struct stat st;
    if (stat(so_path.c_str(), &st) != 0) {
        std::cerr << "[raft] plugin file not found: " << so_path << std::endl;
        return false;
    }

    PluginInfo info;
    info.name = name;
    info.path = so_path;
    info.version = "1.0";
    info.loaded = true;
    info.load_time_ms = now_ms();
    plugins_[name] = info;

    std::cout << "[raft] plugin " << name << " loaded from " << so_path << std::endl;

    // 如果是 leader，通过 Raft 日志同步到其他节点
    if (role_ == RaftRole::LEADER) {
        std::string payload = "{\"name\":\"" + name + "\",\"path\":\"" + so_path + "\"}";
        propose("plugin_load", payload);
    }

    return true;
}

bool RaftNode::unload_plugin(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);

    auto it = plugins_.find(name);
    if (it == plugins_.end()) {
        return false;
    }

    plugins_.erase(it);
    std::cout << "[raft] plugin " << name << " unloaded" << std::endl;

    if (role_ == RaftRole::LEADER) {
        propose("plugin_unload", "{\"name\":\"" + name + "\"}");
    }

    return true;
}

std::vector<PluginInfo> RaftNode::list_plugins() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<PluginInfo> list;
    for (const auto& [_, info] : plugins_) {
        list.push_back(info);
    }
    return list;
}

// ---- 内部方法 ----

std::string RaftNode::forward_to_leader(const std::string& cmd_type, const std::string& payload) {
    // 简化实现：返回 leader 地址，由调用方重试
    std::ostringstream json;
    json << "{";
    json << "\"redirect\":true,";
    json << "\"leader\":\"" << leader_id_ << "\",";
    json << "\"cmd_type\":\"" << cmd_type << "\"";
    json << "}";
    return json.str();
}
