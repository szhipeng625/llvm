// lsm_client.h —— LSM (Redis 协议) 极简客户端,供节点注册表持久化
#pragma once
#include <string>
#include <vector>

namespace pylite_lsm {

// 极简 Redis(RESP) 客户端:支持 SET/GET/DEL/KEYS,同步阻塞,超时短
// 用法: LsmClient c("43.157.39.171", 6380); c.set("k","v"); auto v = c.get("k");
class LsmClient {
public:
    LsmClient(const std::string& host, int port, int timeout_ms = 3000);
    ~LsmClient();
    bool ok() const { return fd_ >= 0; }
    bool set(const std::string& key, const std::string& val);
    bool get(const std::string& key, std::string& val);
    bool del(const std::string& key);
    bool keys(const std::string& pattern, std::vector<std::string>& out);
private:
    int fd_{-1};
    int timeout_ms_;
    bool cmd(const std::vector<std::string>& parts, std::string& reply);
};

}  // namespace pylite_lsm
