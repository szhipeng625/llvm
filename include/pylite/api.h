// PyLite C++ API —— 统一的 C++ 调用接口
//
// 本文件为所有 PyLite 运行时函数提供命名空间化的 C++ 封装，
// 外部 C++ 项目只需包含此头文件并链接 libpylite_runtime.so 即可使用。
//
// 使用方式：
//   #include "pylite/api.h"
//   // 编译: g++ -std=c++17 -I<include_dir> -L<lib_dir> -lpylite_runtime main.cpp
//
// 命名空间结构：
//   pylite::docker::run()       - Docker 容器管理
//   pylite::llm::chat()         - LLM 推理调用
//   pylite::llm::finetune::*    - LLM 微调
//   pylite::cuda::info()        - CUDA 编程
//   pylite::fusion::create_graph() - 算子融合优化
//
// 所有函数返回 std::string（自动管理内存），失败时抛出 std::runtime_error。
#pragma once

#include "pylite/runtime.h"
#include "pylite/value.h"

#include <string>
#include <stdexcept>
#include <cstring>

namespace pylite {

// ===========================================================================
// 辅助函数
// ===========================================================================

// 将 PyValue 字符串转换为 std::string
inline std::string to_string(const PyValue &v) {
  if (v.tag != PY_STR) throw std::runtime_error("PyValue is not a string");
  return std::string(py_str_data(&v), static_cast<size_t>(py_str_size(&v)));
}

// 从 std::string 创建 PyValue 字符串
inline PyValue from_string(const std::string &s) {
  return py_str_new(s.data(), static_cast<int64_t>(s.size()));
}

// 从 int64_t 创建 PyValue 整数
inline PyValue from_int(int64_t v) { return py_int(v); }

// 检查 PyValue 是否为 None
inline bool is_none(const PyValue &v) { return v.tag == PY_NULL; }

// ===========================================================================
// Docker 容器管理
// ===========================================================================
namespace docker {

// 启动容器，返回容器 ID
inline std::string run(const std::string &image, const std::string &command) {
  PyValue img = from_string(image);
  PyValue cmd = from_string(command);
  PyValue result = py_docker_run(&img, &cmd);
  return to_string(result);
}

// 列出运行中的容器
inline std::string ps() {
  PyValue result = py_docker_ps();
  return to_string(result);
}

// 停止容器
inline void stop(const std::string &containerId) {
  PyValue cid = from_string(containerId);
  py_docker_stop(&cid);
}

// 获取容器日志
inline std::string logs(const std::string &containerId) {
  PyValue cid = from_string(containerId);
  PyValue result = py_docker_logs(&cid);
  return to_string(result);
}

// 拉取镜像
inline std::string pull(const std::string &image) {
  PyValue img = from_string(image);
  PyValue result = py_docker_pull(&img);
  return to_string(result);
}

// 生成 Dockerfile
inline std::string generate_dockerfile(
    const std::string &baseImage,
    const std::string &projectName,
    const std::string &setupCommands,
    const std::string &entrypoint,
    int64_t exposePort = 0) {
  PyValue img = from_string(baseImage);
  PyValue name = from_string(projectName);
  PyValue cmds = from_string(setupCommands);
  PyValue entry = from_string(entrypoint);
  PyValue port = from_int(exposePort);
  PyValue result = py_docker_generate_dockerfile(&img, &name, &cmds, &entry, &port);
  return to_string(result);
}

// 构建镜像
inline std::string build(const std::string &dockerfile,
                          const std::string &imageName,
                          const std::string &tag,
                          const std::string &contextDir) {
  PyValue df = from_string(dockerfile);
  PyValue name = from_string(imageName);
  PyValue t = from_string(tag);
  PyValue ctx = from_string(contextDir);
  PyValue result = py_docker_build(&df, &name, &t, &ctx);
  return to_string(result);
}

// 推送镜像
inline std::string push(const std::string &imageName,
                         const std::string &tag,
                         const std::string &registry = "") {
  PyValue name = from_string(imageName);
  PyValue t = from_string(tag);
  PyValue reg = from_string(registry);
  PyValue result = py_docker_push(&name, &t, &reg);
  return to_string(result);
}

// 一键封装项目为 Docker 镜像
inline std::string project_package(
    const std::string &projectDir,
    const std::string &imageName,
    const std::string &tag,
    const std::string &baseImage,
    const std::string &setupCommands,
    const std::string &entrypoint,
    int64_t exposePort = 0) {
  PyValue dir = from_string(projectDir);
  PyValue name = from_string(imageName);
  PyValue t = from_string(tag);
  PyValue img = from_string(baseImage);
  PyValue cmds = from_string(setupCommands);
  PyValue entry = from_string(entrypoint);
  PyValue port = from_int(exposePort);
  PyValue result = py_docker_project_package(&dir, &name, &t, &img, &cmds, &entry, &port);
  return to_string(result);
}

}  // namespace docker

// ===========================================================================
// LLM 推理与微调
// ===========================================================================
namespace llm {

// 调用 LLM 推理 API
inline std::string chat(const std::string &prompt,
                         const std::string &systemPrompt,
                         const std::string &model,
                         const std::string &apiKey,
                         const std::string &endpoint) {
  PyValue p = from_string(prompt);
  PyValue sp = from_string(systemPrompt);
  PyValue m = from_string(model);
  PyValue key = from_string(apiKey);
  PyValue ep = from_string(endpoint);
  PyValue result = py_llm_chat(&p, &sp, &m, &key, &ep);
  return to_string(result);
}

// 创建神经网络结构描述
inline std::string create_network(const std::string &name,
                                   const std::string &layersJson) {
  PyValue n = from_string(name);
  PyValue l = from_string(layersJson);
  PyValue result = py_llm_create_network(&n, &l);
  return to_string(result);
}

// 向网络添加层
inline std::string add_layer(const std::string &networkJson,
                              const std::string &layerJson) {
  PyValue net = from_string(networkJson);
  PyValue lay = from_string(layerJson);
  PyValue result = py_llm_add_layer(&net, &lay);
  return to_string(result);
}

// 网络结构摘要
inline std::string network_summary(const std::string &networkJson) {
  PyValue net = from_string(networkJson);
  PyValue result = py_llm_network_summary(&net);
  return to_string(result);
}

// ===========================================================================
// LLM 微调子命名空间
// ===========================================================================
namespace finetune {

// 创建训练数据集（JSONL 格式）
inline std::string create_dataset(const std::string &examplesJson,
                                   const std::string &systemPrompt = "") {
  PyValue ex = from_string(examplesJson);
  PyValue sp = from_string(systemPrompt);
  PyValue result = py_llm_create_dataset(&ex, &sp);
  return to_string(result);
}

// 上传训练文件
inline std::string upload_file(const std::string &data,
                                const std::string &filename,
                                const std::string &apiKey,
                                const std::string &endpoint) {
  PyValue d = from_string(data);
  PyValue fn = from_string(filename);
  PyValue key = from_string(apiKey);
  PyValue ep = from_string(endpoint);
  PyValue result = py_llm_upload_file(&d, &fn, &key, &ep);
  return to_string(result);
}

// 创建微调任务
inline std::string create_job(const std::string &fileId,
                               const std::string &model,
                               const std::string &suffix,
                               const std::string &apiKey,
                               const std::string &endpoint) {
  PyValue fid = from_string(fileId);
  PyValue m = from_string(model);
  PyValue suf = from_string(suffix);
  PyValue key = from_string(apiKey);
  PyValue ep = from_string(endpoint);
  PyValue result = py_llm_create_finetune(&fid, &m, &suf, &key, &ep);
  return to_string(result);
}

// 查询微调状态
inline std::string job_status(const std::string &jobId,
                               const std::string &apiKey,
                               const std::string &endpoint) {
  PyValue jid = from_string(jobId);
  PyValue key = from_string(apiKey);
  PyValue ep = from_string(endpoint);
  PyValue result = py_llm_finetune_status(&jid, &key, &ep);
  return to_string(result);
}

// 列出所有微调任务
inline std::string list_jobs(const std::string &apiKey,
                              const std::string &endpoint) {
  PyValue key = from_string(apiKey);
  PyValue ep = from_string(endpoint);
  PyValue result = py_llm_list_finetunes(&key, &ep);
  return to_string(result);
}

}  // namespace finetune
}  // namespace llm

// ===========================================================================
// CUDA 编程
// ===========================================================================
namespace cuda {

// 获取 GPU 信息
inline std::string info() {
  PyValue result = py_cuda_info();
  return to_string(result);
}

// 编译 CUDA 内核为 PTX
inline std::string compile_ptx(const std::string &kernelCode,
                                const std::string &kernelName) {
  PyValue code = from_string(kernelCode);
  PyValue name = from_string(kernelName);
  PyValue result = py_cuda_compile_ptx(&code, &name);
  return to_string(result);
}

// 启动 CUDA 内核
inline std::string launch_kernel(const std::string &ptxCode,
                                  const std::string &kernelName,
                                  int64_t gridX = 1, int64_t gridY = 1, int64_t gridZ = 1,
                                  int64_t blockX = 256, int64_t blockY = 1, int64_t blockZ = 1,
                                  const std::string &args = "[]") {
  PyValue ptx = from_string(ptxCode);
  PyValue name = from_string(kernelName);
  PyValue gx = from_int(gridX);
  PyValue gy = from_int(gridY);
  PyValue gz = from_int(gridZ);
  PyValue bx = from_int(blockX);
  PyValue by = from_int(blockY);
  PyValue bz = from_int(blockZ);
  PyValue a = from_string(args);
  PyValue result = py_cuda_launch_kernel(&ptx, &name, &gx, &gy, &gz, &bx, &by, &bz, &a);
  return to_string(result);
}

// GPU 内存分配
inline std::string alloc(int64_t size) {
  PyValue sz = from_int(size);
  PyValue result = py_cuda_alloc(&sz);
  return to_string(result);
}

// GPU 内存释放
inline void free(const std::string &handle) {
  PyValue h = from_string(handle);
  py_cuda_free(&h);
}

// 数据复制到 GPU
inline void memcpy_to_device(const std::string &handle, const std::string &data) {
  PyValue h = from_string(handle);
  PyValue d = from_string(data);
  py_cuda_memcpy_to_device(&h, &d);
}

// 从 GPU 复制数据
inline std::string memcpy_from_device(const std::string &handle, int64_t size) {
  PyValue h = from_string(handle);
  PyValue sz = from_int(size);
  PyValue result = py_cuda_memcpy_from_device(&h, &sz);
  return to_string(result);
}

}  // namespace cuda

// ===========================================================================
// 算子融合优化
// ===========================================================================
namespace fusion {

// 创建计算图
inline std::string create_graph(const std::string &name) {
  PyValue n = from_string(name);
  PyValue result = py_fusion_create_graph(&n);
  return to_string(result);
}

// 添加算子
inline std::string add_op(const std::string &graph,
                           const std::string &opType,
                           const std::string &opName,
                           const std::string &params = "{}") {
  PyValue g = from_string(graph);
  PyValue type = from_string(opType);
  PyValue name = from_string(opName);
  PyValue p = from_string(params);
  PyValue result = py_fusion_add_op(&g, &type, &name, &p);
  return to_string(result);
}

// 应用融合规则
inline std::string apply_rules(const std::string &graph) {
  PyValue g = from_string(graph);
  PyValue result = py_fusion_apply_rules(&g);
  return to_string(result);
}

// 生成 CUDA 内核
inline std::string generate_kernel(const std::string &fusedOp,
                                    const std::string &config = "{}") {
  PyValue op = from_string(fusedOp);
  PyValue cfg = from_string(config);
  PyValue result = py_fusion_generate_kernel(&op, &cfg);
  return to_string(result);
}

// 自动调优
inline std::string autotune(const std::string &opType,
                             int64_t M, int64_t N, int64_t K) {
  PyValue op = from_string(opType);
  PyValue m = from_int(M);
  PyValue n = from_int(N);
  PyValue k = from_int(K);
  PyValue result = py_fusion_autotune(&op, &m, &n, &k);
  return to_string(result);
}

// 编译并运行内核
inline std::string compile_and_run(const std::string &kernelCode,
                                    const std::string &kernelName,
                                    int64_t M = 1024, int64_t N = 4096, int64_t K = 768) {
  PyValue code = from_string(kernelCode);
  PyValue name = from_string(kernelName);
  PyValue m = from_int(M);
  PyValue n = from_int(N);
  PyValue k = from_int(K);
  PyValue result = py_fusion_compile_and_run(&code, &name, &m, &n, &k);
  return to_string(result);
}

// 性能基准测试
inline std::string benchmark(const std::string &kernelCode,
                              const std::string &kernelName,
                              int64_t M = 1024, int64_t N = 4096, int64_t K = 768,
                              int64_t iterations = 100) {
  PyValue code = from_string(kernelCode);
  PyValue name = from_string(kernelName);
  PyValue m = from_int(M);
  PyValue n = from_int(N);
  PyValue k = from_int(K);
  PyValue iters = from_int(iterations);
  PyValue result = py_fusion_benchmark(&code, &name, &m, &n, &k, &iters);
  return to_string(result);
}

}  // namespace fusion

}  // namespace pylite
