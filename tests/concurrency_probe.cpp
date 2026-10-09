// 线程 / 进程 / 资源管理验证探针。
#include "pylite/runtime.h"
#include "pylite/value.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;
static void check(bool ok, const char *what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_fail;
}

// 线程任务:累加到一个全局计数器,睡眠一小会儿。
static volatile int g_counter = 0;
static void worker(void *) {
  for (int i = 0; i < 100000; ++i) ++g_counter;
}

int main() {
  std::printf("=== 线程 ===\n");
  PyValue name = py_str_new("worker", 6);
  py_thread_register_task(py_str_data(&name), reinterpret_cast<void *>(&worker), nullptr);
  PyValue tid = py_thread_start(py_str_data(&name));
  check(tid.tag == PY_INT, "启动线程返回线程号");
  const int64_t id = tid.as.i;
  py_thread_join(id);
  check(g_counter == 100000, "线程任务确实执行完毕");
  check(py_thread_alive(id) == 0, "线程结束后判活为假");

  std::printf("=== 进程 ===\n");
  PyValue cmd = py_str_new("echo hello-concurrency", 22);
  PyValue out = py_process_run(&cmd);
  check(out.tag == PY_STR, "同步执行命令返回字符串");
  check(std::strstr(py_str_data(&out), "hello-concurrency") != nullptr,
        "命令输出符合预期");

  std::printf("=== 资源管理 ===\n");
  PyValue stats = py_resource_stats();
  check(stats.tag == PY_STR, "资源快照可获取");
  check(std::strstr(py_str_data(&stats), "threads") != nullptr,
        "快照包含线程统计");
  py_resource_reap();
  check(py_thread_count() == 0, "回收后线程登记表为空");

  std::printf("\n%s (失败 %d 项)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
