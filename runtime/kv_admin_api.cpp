#include "kv_admin.h"
#include "cluster.h"
#include "../include/pylite/runtime.h"
#include <cstring>
#include <sstream>
#include <cstdlib>

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

// ====== 沙箱绑定与查找 ======
// 全局映射：节点 ID → 沙箱句柄
static std::map<std::string, int64_t> g_node_sandbox_map;
static std::mutex g_node_sandbox_mutex;

// kv.bind_sandbox(node_id, sandbox_handle) → 将沙箱绑定到节点
int64_t py_kv_bind_sandbox(const char *node_id, int64_t idLen, int64_t handle) {
    std::string id(node_id, idLen);
    std::lock_guard<std::mutex> lk(g_node_sandbox_mutex);
    g_node_sandbox_map[id] = handle;
    return 1;
}

// kv.get_sandbox(node_id) → 通过节点 ID 查找绑定的沙箱句柄
int64_t py_kv_get_sandbox(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::lock_guard<std::mutex> lk(g_node_sandbox_mutex);
    auto it = g_node_sandbox_map.find(id);
    if (it != g_node_sandbox_map.end()) return it->second;
    return -1;
}

// kv.unbind_sandbox(node_id) → 解除节点与沙箱的绑定
int64_t py_kv_unbind_sandbox(const char *node_id, int64_t idLen) {
    std::string id(node_id, idLen);
    std::lock_guard<std::mutex> lk(g_node_sandbox_mutex);
    g_node_sandbox_map.erase(id);
    return 1;
}

}  // extern "C"

// ============================================================================
// 分布式便捷内建:add(box) 统一编排与容器内定位(2026-10 增补)
// ============================================================================
// 反向映射:sandbox handle -> node_id,供 kv.locate(box) 由盒子反查所属节点
static std::map<int64_t, std::string> g_sandbox_node_map;

extern "C" {

// 从 box 实例中取 handle 属性;box 非实例或缺属性时返回 -1
static int64_t box_handle_of(const PyValue* box) {
    if (!box) return -1;
    PyValue h = py_instance_get_attr(box, "handle", 6);
    if (h.tag != PY_INT) return -1;
    return h.as.i;
}

// kv.add_box(node_id, box, addr) —— 一步完成『注册节点 + 绑定盒子』。
// addr 为空串则本地启动(进程就放进 box 沙箱里),填 host:port 则登记为远程节点。
// 同时维护正向(node->handle)与反向(handle->node)双向映射,
// 使容器内可以用 kv.locate(box) 定位到 kv->box 的归属关系。
PyValue py_kv_add_box(const char *node_id, int64_t idLen,
                      const PyValue *box,
                      const char *addr, int64_t addrLen) {
    std::string id(node_id, idLen);
    std::string a(addr, addrLen);
    int64_t handle = box_handle_of(box);
    if (handle < 0) {
        return op_result("add_box", id, false, "box 缺少 handle 属性");
    }
    // 从盒子配置里取真实登记的二进制路径(add_box 前用 sandbox.set_binary 设置)
    std::string binary;
    {
        PyValue bv = py_sandbox_binary_path(handle);
        if (bv.tag == PY_STR) {
            binary.assign(py_str_data(&bv), py_str_size(&bv));
        }
    }
    std::string error;
    bool ok = KvAdmin::instance().add(id, binary, {}, "/tmp", a, error);
    if (ok) {
        std::lock_guard<std::mutex> lk(g_node_sandbox_mutex);
        g_node_sandbox_map[id] = handle;
        g_sandbox_node_map[handle] = id;
    }
    // 远程地址:通过 cluster.exec 让对端自动拉起节点进程(远程 add_box 落地)
    if (ok && !a.empty()) {
        ClusterManager& cm = ClusterManager::instance();
        std::string cname = "auto-" + a;
        if (!cm.is_connected(cname)) {
            const char* sec = std::getenv("PYLITE_CLUSTER_SECRET");
            cm.connect(cname, a, sec ? sec : "pylite-secret");
        }
        std::ostringstream payload;
        payload << "{\"node_id\":\"" << id << "\","
                << "\"binary\":\"" << binary << "\","
                << "\"addr\":\"" << a << "\","
                << "\"work_dir\":\"/tmp\"}";
        auto rr = cm.exec(cname, "start_node", payload.str());
        std::ostringstream out;
        out << "{\"op\":\"add_box\",\"node_id\":\"" << id
            << "\",\"success\":" << (rr.success ? "true" : "false");
        if (!rr.error.empty()) out << ",\"error\":\"" << rr.error << "\"";
        if (rr.success && !rr.body.empty()) out << ",\"remote\":" << rr.body;
        out << "}";
        std::string s = out.str();
        return py_str_new(s.c_str(), s.size());
    }
    return op_result("add_box", id, ok, error);
}

// kv.locate(box) —— 由盒子反查所属 kv 节点的 kv->box 绑定关系。
// 返回 {node_id, handle, addr, state},未绑定返回 {node_id:"",found:false}
PyValue py_kv_locate(const PyValue *box) {
    int64_t handle = box_handle_of(box);
    std::string id;
    {
        std::lock_guard<std::mutex> lk(g_node_sandbox_mutex);
        auto it = g_sandbox_node_map.find(handle);
        if (it != g_sandbox_node_map.end()) id = it->second;
    }
    std::ostringstream json;
    if (id.empty()) {
        json << "{\"found\":false,\"handle\":" << handle << "}";
    } else {
        KvNode n;
        bool has = KvAdmin::instance().get_node(id, n);
        json << "{\"found\":true,\"handle\":" << handle
             << ",\"node_id\":\"" << id << "\"";
        if (has) {
            json << ",\"addr\":\"" << n.addr << "\""
                 << ",\"state\":" << static_cast<int64_t>(n.state);
        }
        json << "}";
    }
    std::string s = json.str();
    return py_str_new(s.c_str(), s.size());
}

// kv.self() —— 容器内定位:读启动时注入的 PYLITE_NODE_ID/ADDR,
// 让盒子里运行的代码知道自己属于哪个 kv 节点。未注入返回 {found:false}
PyValue py_kv_self() {
    const char* id = std::getenv("PYLITE_NODE_ID");
    const char* addr = std::getenv("PYLITE_NODE_ADDR");
    std::ostringstream json;
    if (!id) {
        json << "{\"found\":false}";
    } else {
        json << "{\"found\":true,\"node_id\":\"" << id << "\"";
        if (addr) json << ",\"addr\":\"" << addr << "\"";
        // 顺带报告本节点绑定的盒子句柄
        std::lock_guard<std::mutex> lk(g_node_sandbox_mutex);
        auto it = g_node_sandbox_map.find(id);
        if (it != g_node_sandbox_map.end()) {
            json << ",\"box\":" << it->second;
        }
        json << "}";
    }
    std::string s = json.str();
    return py_str_new(s.c_str(), s.size());
}

}  // extern "C"

// ============================================================================
// 节点迭代器适配器(2026-10 增补)
// 用法: it = kv.iter() -> 迭代器实例; kv.next(it) 逐个取节点信息;
//       kv.rewind(it) 重置游标。迭代器内部维护游标与节点快照。
// 元素为与 kv.list 相同的节点信息字符串,便于统一处理。
// ============================================================================
static const int64_t KV_ITER_CLASS_ID = 7;

// 迭代器状态:handle -> {快照节点信息列表, 当前游标}
struct KvIterState {
    std::vector<std::string> items;
    int64_t cursor;
    KvIterState() : cursor(0) {}
};
static std::map<int64_t, KvIterState> g_kv_iters;
static int64_t g_kv_iter_next_handle = 1;
static std::mutex g_kv_iter_mutex;

extern "C" {

// 迭代器实例的 next 方法处理:for 迭代协议经实例方法分派表路由到这里
static PyValue kv_iter_next_method(const PyValue *inst, PyValue *args, int64_t nargs) {
    (void)args; (void)nargs;
    return py_kv_iter_next(inst);
}

static void ensure_iter_methods_registered() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        py_instance_register_method("kv_iter", 7, "next", 4, kv_iter_next_method);
        py_instance_register_method("kv_iter_live", 12, "next", 4, kv_iter_next_method);
    });
}

// kv.iter() → 创建节点迭代器实例(快照当前节点列表)
PyValue py_kv_iter() {
    ensure_iter_methods_registered();
    auto nodes = KvAdmin::instance().list_nodes();
    std::lock_guard<std::mutex> lk(g_kv_iter_mutex);
    int64_t handle = g_kv_iter_next_handle++;
    KvIterState st;
    for (const auto& n : nodes) st.items.push_back(node_to_json(n));
    g_kv_iters[handle] = st;
    // 构造实例属性: handle + type + count
    PyValue attrs = py_dict_new(nullptr, nullptr, 0);
    PyValue k1 = py_str_new("handle", 6);
    PyValue v1 = py_int(handle);
    py_dict_set(&attrs, &k1, &v1);
    PyValue k2 = py_str_new("type", 4);
    PyValue v2 = py_str_new("kv_iter", 7);
    py_dict_set(&attrs, &k2, &v2);
    PyValue k3 = py_str_new("count", 5);
    PyValue v3 = py_int(static_cast<int64_t>(st.items.size()));
    py_dict_set(&attrs, &k3, &v3);
    return py_instance_new(KV_ITER_CLASS_ID, &attrs);
}

// 从迭代器实例中取 handle
static int64_t kv_iter_handle_of(const PyValue* it) {
    if (!it) return -1;
    PyValue h = py_instance_get_attr(it, "handle", 6);
    if (h.tag != PY_INT) return -1;
    return h.as.i;
}

// kv.next(it) → 取下一个节点信息字符串; 迭代结束返回 None
PyValue py_kv_iter_next(const PyValue* it) {
    int64_t handle = kv_iter_handle_of(it);
    std::lock_guard<std::mutex> lk(g_kv_iter_mutex);
    auto itr = g_kv_iters.find(handle);
    if (itr == g_kv_iters.end()) return py_none();
    KvIterState& st = itr->second;
    if (st.items.empty()) {
        // live 模式:实时查询当前节点列表
        auto nodes = KvAdmin::instance().list_nodes();
        if (st.cursor >= static_cast<int64_t>(nodes.size())) return py_none();
        std::string s = node_to_json(nodes[st.cursor++]);
        return py_str_new(s.c_str(), s.size());
    }
    if (st.cursor >= static_cast<int64_t>(st.items.size())) return py_none();
    const std::string& s = st.items[st.cursor++];
    return py_str_new(s.c_str(), s.size());
}

// kv.rewind(it) → 重置游标到开头, 返回剩余元素个数
PyValue py_kv_iter_rewind(const PyValue* it) {
    int64_t handle = kv_iter_handle_of(it);
    std::lock_guard<std::mutex> lk(g_kv_iter_mutex);
    auto itr = g_kv_iters.find(handle);
    if (itr == g_kv_iters.end()) return py_int(0);
    itr->second.cursor = 0;
    return py_int(static_cast<int64_t>(itr->second.items.size()));
}

// kv.iter_live() → 实时迭代器:不做快照,next 时实时查询节点列表。
// 与 kv.iter 的快照语义互补:迭代期间节点的增删立即可见,
// 代价是每次 next 都要拿一次管理器锁。
PyValue py_kv_iter_live() {
    ensure_iter_methods_registered();
    std::lock_guard<std::mutex> lk(g_kv_iter_mutex);
    int64_t handle = g_kv_iter_next_handle++;
    KvIterState st;   // items 留空,作为 live 标记
    g_kv_iters[handle] = st;
    PyValue attrs = py_dict_new(nullptr, nullptr, 0);
    PyValue k1 = py_str_new("handle", 6);
    PyValue v1 = py_int(handle);
    py_dict_set(&attrs, &k1, &v1);
    PyValue k2 = py_str_new("type", 4);
    PyValue v2 = py_str_new("kv_iter_live", 12);
    py_dict_set(&attrs, &k2, &v2);
    return py_instance_new(KV_ITER_CLASS_ID, &attrs);
}

// kv.iter_destroy(it) → 销毁迭代器, 释放快照
PyValue py_kv_iter_destroy(const PyValue* it) {
    int64_t handle = kv_iter_handle_of(it);
    std::lock_guard<std::mutex> lk(g_kv_iter_mutex);
    g_kv_iters.erase(handle);
    return py_none();
}

}  // extern "C"
