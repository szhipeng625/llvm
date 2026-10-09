#include "kv_admin.h"
#include "../include/pylite/runtime.h"
#include <cstring>
#include <sstream>

// 辅助：节点转 JSON
static std::string node_to_json(const KvNode& n) {
    const char* state = "unknown";
    switch (n.state) {
        case KvNodeState::IDLE:     state = "idle"; break;
        case KvNodeState::RUNNING:  state = "running"; break;
        case KvNodeState::SLEEPING: state = "sleeping"; break;
        case KvNodeState::STOPPED:  state = "stopped"; break;
        case KvNodeState::ERASED:   state = "erased"; break;
        case KvNodeState::LOST:     state = "lost"; break;
    }
    std::ostringstream json;
    json << "{";
    json << "\"node_id\":\"" << n.node_id << "\",";
    json << "\"addr\":\"" << n.addr << "\",";
    json << "\"state\":\"" << state << "\",";
    json << "\"pid\":" << n.pid << ",";
    json << "\"started_at_ms\":" << n.started_at_ms << ",";
    json << "\"last_active_ms\":" << n.last_active_ms << ",";
    json << "\"restart_count\":" << n.restart_count;
    json << "}";
    return json.str();
}

// 辅助：执行操作并返回 JSON 结果
static PyValue op_result(const char* op, const std::string& node_id, bool ok, const std::string& error) {
    std::ostringstream json;
    json << "{";
    json << "\"op\":\"" << op << "\",";
    json << "\"node_id\":\"" << node_id << "\",";
    json << "\"success\":" << (ok ? "true" : "false");
    if (!ok && !error.empty()) {
        json << ",\"error\":\"" << error << "\"";
    }
    json << "}";
    std::string s = json.str();
    return py_str_new(s.c_str(), s.size());
}

extern "C" {

// kv.build(node_id, binary, args_json, work_dir, addr) → 建立节点并启动
PyValue py_kv_build(const char *node_id, int64_t idLen,
                    const char *binary, int64_t binLen,
                    const char *args_json, int64_t argsLen,
                    const char *work_dir, int64_t dirLen,
                    const char *addr, int64_t addrLen) {
    std::string id(node_id, idLen);
    std::string bin(binary, binLen);
    std::string args_str(args_json, argsLen);
    std::string dir(work_dir, dirLen);
    std::string a(addr, addrLen);

    // 解析参数 JSON 数组: ["arg1", "arg2"]
    std::vector<std::string> args;
    size_t start = 0;
    while (start < args_str.size()) {
        size_t q1 = args_str.find('"', start);
        if (q1 == std::string::npos) break;
        size_t q2 = args_str.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        args.push_back(args_str.substr(q1 + 1, q2 - q1 - 1));
        start = q2 + 1;
    }

    std::string error;
    bool ok = KvAdmin::instance().build(id, bin, args, dir, a, error);
    return op_result("build", id, ok, error);
}

// kv.add(node_id, binary, args_json, work_dir, addr) → 向集群追加节点
PyValue py_kv_add(const char *node_id, int64_t idLen,
                  const char *binary, int64_t binLen,
                  const char *args_json, int64_t argsLen,
                  const char *work_dir, int64_t dirLen,
                  const char *addr, int64_t addrLen) {
    std::string id(node_id, idLen);
    std::string bin(binary, binLen);
    std::string args_str(args_json, argsLen);
    std::string dir(work_dir, dirLen);
    std::string a(addr, addrLen);

    std::vector<std::string> args;
    size_t start = 0;
    while (start < args_str.size()) {
        size_t q1 = args_str.find('"', start);
        if (q1 == std::string::npos) break;
        size_t q2 = args_str.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        args.push_back(args_str.substr(q1 + 1, q2 - q1 - 1));
        start = q2 + 1;
    }

    std::string error;
    bool ok = KvAdmin::instance().add(id, bin, args, dir, a, error);
    return op_result("add", id, ok, error);
}

// kv.erase(node_id) → 永久移除节点
PyValue py_kv_erase(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::string error;
    bool ok = KvAdmin::instance().erase(id, error);
    return op_result("erase", id, ok, error);
}

// kv.skip(node_id) → 逻辑跳过（标记 LOST）
PyValue py_kv_skip(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::string error;
    bool ok = KvAdmin::instance().skip(id, error);
    return op_result("skip", id, ok, error);
}

// kv.sleep(node_id) → 暂停节点（SIGSTOP）
PyValue py_kv_sleep(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::string error;
    bool ok = KvAdmin::instance().sleep_node(id, error);
    return op_result("sleep", id, ok, error);
}

// kv.wakeup(node_id) → 唤醒节点（SIGCONT）
PyValue py_kv_wakeup(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::string error;
    bool ok = KvAdmin::instance().wakeup(id, error);
    return op_result("wakeup", id, ok, error);
}

// kv.stop(node_id) → 停止节点（保留注册）
PyValue py_kv_stop(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::string error;
    bool ok = KvAdmin::instance().stop(id, error);
    return op_result("stop", id, ok, error);
}

// kv.restart(node_id) → 重启节点
PyValue py_kv_restart(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::string error;
    bool ok = KvAdmin::instance().restart(id, error);
    return op_result("restart", id, ok, error);
}

// kv.status(node_id) → 节点详情 JSON
PyValue py_kv_status(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    KvNode node;
    if (!KvAdmin::instance().get_node(id, node)) {
        return op_result("status", id, false, "node not found");
    }
    std::string s = node_to_json(node);
    return py_str_new(s.c_str(), s.size());
}

// kv.list() → 所有节点列表 JSON
PyValue py_kv_list() {
    auto nodes = KvAdmin::instance().list_nodes();
    std::ostringstream json;
    json << "[";
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (i > 0) json << ",";
        json << node_to_json(nodes[i]);
    }
    json << "]";
    std::string s = json.str();
    return py_str_new(s.c_str(), s.size());
}

// kv.alive(node_id) → 节点进程是否存活（1/0）
int64_t py_kv_alive(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    return KvAdmin::instance().is_alive(id) ? 1 : 0;
}

}  // extern "C"
