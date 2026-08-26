#pragma once
// twigrpc::RegistryDaemon —— 注册中心守护进程（框架自举：用 RpcServer 自身实现）。
// Register/Discover/Heartbeat 三个 RPC 方法 + 周期扫描：
//   15s 无心跳标 DOWN（healthy=false，保留在 Discover 结果中），30s 剔除。
// 内存态粗锁：registry 流量低，正确性优先（取舍见注释）。
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "netlib/EventLoop.h"
#include "server/RpcServer.h"
#include "rpc.pb.h"

namespace config {
inline constexpr double kRegistrySweepSec = 5.0;   // 扫描周期
inline constexpr double kRegistryDownAfterSec = 15.0; // 无心跳标 DOWN
inline constexpr double kRegistryEvictAfterSec = 30.0; // 无心跳剔除
} // namespace config

namespace twigrpc {

class RegistryDaemon {
public:
    explicit RegistryDaemon(netlib::EventLoop* mainLoop,
                            netlib::Endpoint listenAddr = netlib::Endpoint("0.0.0.0", 8500));

    void serve();
    void stop();
    uint16_t listenPort() const { return server_.listenPort(); }
    void sweep(); // 过期扫描（由外部定时驱动，如 registryd main 的 runEvery）

private:
    void bindMethods();

    struct Entry {
        twigrpc::Instance instance;
        std::chrono::steady_clock::time_point lastBeat;
    };

    RpcServer server_;
    // 粗锁保护：service -> (instanceId -> Entry)
    std::map<std::string, std::map<std::string, Entry>> table_;
    std::mutex mtx_;
    uint64_t version_ = 0;
};

} // namespace twigrpc
