// 分布式模块 PyValue 适配器
// 将 PyValue* 参数转换为 const char* + int64_t 形式，
// 使 raft/cluster/kv 模块的函数可以被 PyLite 内置模块调用机制使用。
#include "pylite/runtime.h"
#include "pylite/value.h"
#include <cstring>

// 从 PyValue 中提取字符串指针和长度
static const char* strPtr(const PyValue* v) {
    if (v->tag != PY_STR) return "";
    return py_str_data(v);
}
static int64_t strLen(const PyValue* v) {
    if (v->tag != PY_STR) return 0;
    return py_str_size(v);
}
// ====== Raft 适配器 ======
extern "C" {

PyValue py_raft_build_v(const PyValue* config) {
    return py_raft_build(strPtr(config), strLen(config));
}

PyValue py_raft_erase_v(const PyValue* node_id) {
    return py_raft_erase(strPtr(node_id), strLen(node_id));
}

PyValue py_raft_status_v(const PyValue* node_id) {
    return py_raft_status(strPtr(node_id), strLen(node_id));
}

PyValue py_raft_list_nodes_v() {
    return py_raft_list_nodes();
}

PyValue py_raft_add_v(const PyValue* node_id, const PyValue* cmd_type, const PyValue* payload) {
    return py_raft_add(strPtr(node_id), strLen(node_id),
                       strPtr(cmd_type), strLen(cmd_type),
                       strPtr(payload), strLen(payload));
}

PyValue py_raft_register_handler_v(const PyValue* node_id, const PyValue* cmd_type) {
    int64_t r = py_raft_register_handler(strPtr(node_id), strLen(node_id),
                                          strPtr(cmd_type), strLen(cmd_type));
    return py_int(r);
}

PyValue py_raft_register_node_v(const PyValue* node_id, const PyValue* role, const PyValue* addr) {
    return py_raft_register_node(strPtr(node_id), strLen(node_id),
                                 strPtr(role), strLen(role),
                                 strPtr(addr), strLen(addr));
}

PyValue py_raft_cluster_nodes_v(const PyValue* node_id) {
    return py_raft_cluster_nodes(strPtr(node_id), strLen(node_id));
}

PyValue py_raft_mount_storage_v(const PyValue* node_id, const PyValue* name, const PyValue* path) {
    return py_raft_mount_storage(strPtr(node_id), strLen(node_id),
                                 strPtr(name), strLen(name),
                                 strPtr(path), strLen(path));
}

PyValue py_raft_plugin_load_v(const PyValue* node_id, const PyValue* name, const PyValue* so_path) {
    return py_raft_plugin_load(strPtr(node_id), strLen(node_id),
                               strPtr(name), strLen(name),
                               strPtr(so_path), strLen(so_path));
}

PyValue py_raft_plugin_unload_v(const PyValue* node_id, const PyValue* name) {
    return py_raft_plugin_unload(strPtr(node_id), strLen(node_id),
                                 strPtr(name), strLen(name));
}

PyValue py_raft_plugin_list_v(const PyValue* node_id) {
    return py_raft_plugin_list(strPtr(node_id), strLen(node_id));
}

// ====== Cluster 适配器 ======

PyValue py_cluster_connect_v(const PyValue* name, const PyValue* addr, const PyValue* secret) {
    return py_cluster_connect(strPtr(name), strLen(name),
                              strPtr(addr), strLen(addr),
                              strPtr(secret), strLen(secret));
}

PyValue py_cluster_disconnect_v(const PyValue* name) {
    return py_cluster_disconnect(strPtr(name), strLen(name));
}

PyValue py_cluster_list_v() {
    return py_cluster_list();
}

PyValue py_cluster_is_connected_v(const PyValue* name) {
    int64_t r = py_cluster_is_connected(strPtr(name), strLen(name));
    return py_int(r);
}

PyValue py_cluster_status_v(const PyValue* name) {
    return py_cluster_status(strPtr(name), strLen(name));
}

PyValue py_cluster_nodes_v(const PyValue* name) {
    return py_cluster_nodes(strPtr(name), strLen(name));
}

PyValue py_cluster_health_v(const PyValue* name) {
    int64_t r = py_cluster_health(strPtr(name), strLen(name));
    return py_int(r);
}

PyValue py_cluster_exec_v(const PyValue* name, const PyValue* cmd_type, const PyValue* payload) {
    return py_cluster_exec(strPtr(name), strLen(name),
                           strPtr(cmd_type), strLen(cmd_type),
                           strPtr(payload), strLen(payload));
}

PyValue py_cluster_query_v(const PyValue* name, const PyValue* path) {
    return py_cluster_query(strPtr(name), strLen(name),
                            strPtr(path), strLen(path));
}

PyValue py_cluster_broadcast_v(const PyValue* cmd_type, const PyValue* payload) {
    return py_cluster_broadcast(strPtr(cmd_type), strLen(cmd_type),
                                strPtr(payload), strLen(payload));
}

PyValue py_cluster_health_all_v() {
    return py_cluster_health_all();
}

// ====== KvAdmin 适配器 ======

PyValue py_kv_build_v(const PyValue* node_id, const PyValue* binary, const PyValue* args_json,
                      const PyValue* work_dir, const PyValue* addr) {
    return py_kv_build(strPtr(node_id), strLen(node_id),
                       strPtr(binary), strLen(binary),
                       strPtr(args_json), strLen(args_json),
                       strPtr(work_dir), strLen(work_dir),
                       strPtr(addr), strLen(addr));
}

PyValue py_kv_add_v(const PyValue* node_id, const PyValue* binary, const PyValue* args_json,
                    const PyValue* work_dir, const PyValue* addr) {
    return py_kv_add(strPtr(node_id), strLen(node_id),
                     strPtr(binary), strLen(binary),
                     strPtr(args_json), strLen(args_json),
                     strPtr(work_dir), strLen(work_dir),
                     strPtr(addr), strLen(addr));
}

PyValue py_kv_erase_v(const PyValue* node_id) {
    return py_kv_erase(strPtr(node_id), strLen(node_id));
}

PyValue py_kv_skip_v(const PyValue* node_id) {
    return py_kv_skip(strPtr(node_id), strLen(node_id));
}

PyValue py_kv_sleep_v(const PyValue* node_id) {
    return py_kv_sleep(strPtr(node_id), strLen(node_id));
}

PyValue py_kv_wakeup_v(const PyValue* node_id) {
    return py_kv_wakeup(strPtr(node_id), strLen(node_id));
}

PyValue py_kv_stop_v(const PyValue* node_id) {
    return py_kv_stop(strPtr(node_id), strLen(node_id));
}

PyValue py_kv_restart_v(const PyValue* node_id) {
    return py_kv_restart(strPtr(node_id), strLen(node_id));
}

PyValue py_kv_status_v(const PyValue* node_id) {
    return py_kv_status(strPtr(node_id), strLen(node_id));
}

PyValue py_kv_list_v() {
    return py_kv_list();
}

PyValue py_kv_alive_v(const PyValue* node_id) {
    int64_t r = py_kv_alive(strPtr(node_id), strLen(node_id));
    return py_int(r);
}

PyValue py_kv_bind_sandbox_v(const PyValue* node_id, const PyValue* handle) {
    int64_t r = py_kv_bind_sandbox(strPtr(node_id), strLen(node_id), handle->as.i);
    return py_int(r);
}

PyValue py_kv_get_sandbox_v(const PyValue* node_id) {
    int64_t r = py_kv_get_sandbox(strPtr(node_id), strLen(node_id));
    return py_int(r);
}

PyValue py_kv_unbind_sandbox_v(const PyValue* node_id) {
    int64_t r = py_kv_unbind_sandbox(strPtr(node_id), strLen(node_id));
    return py_int(r);
}

// ====== 分布式便捷内建适配器(2026-10 增补) ======
PyValue py_raft_leader_info_v(const PyValue* node_id) {
    return py_raft_leader_info(strPtr(node_id), strLen(node_id));
}
PyValue py_raft_send_v(const PyValue* node_id, const PyValue* msg) {
    return py_raft_send(strPtr(node_id), strLen(node_id),
                        strPtr(msg), strLen(msg));
}
PyValue py_raft_recv_v(const PyValue* node_id) {
    return py_raft_recv(strPtr(node_id), strLen(node_id));
}
// ====== add(box) 编排与容器内定位适配器(2026-10 增补) ======
// box 是实例对象,直接透传指针,不走 strPtr 字符串提取
PyValue py_kv_add_box_v(const PyValue* node_id, const PyValue* box, const PyValue* addr) {
    return py_kv_add_box(strPtr(node_id), strLen(node_id),
                         box,
                         strPtr(addr), strLen(addr));
}
PyValue py_kv_locate_v(const PyValue* box) {
    return py_kv_locate(box);
}
PyValue py_kv_self_v() {
    return py_kv_self();
}
// ====== 节点迭代器适配器(2026-10 增补) ======
PyValue py_kv_iter_v() {
    return py_kv_iter();
}
PyValue py_kv_iter_next_v(const PyValue* it) {
    return py_kv_iter_next(it);
}
PyValue py_kv_iter_rewind_v(const PyValue* it) {
    return py_kv_iter_rewind(it);
}
PyValue py_kv_iter_destroy_v(const PyValue* it) {
    return py_kv_iter_destroy(it);
}
PyValue py_kv_iter_live_v() {
    return py_kv_iter_live();
}
}  // extern "C"
