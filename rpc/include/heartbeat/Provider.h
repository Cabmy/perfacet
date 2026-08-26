#pragma once
// twigrpc::Provider —— 服务提供者侧心跳组件：
// 构造时 Register，随后 runEvery(kHeartbeatSec) 发 Heartbeat RPC
// （内部 RpcClient 指向 registry——自举的完整闭环）。
// 做成 RpcServer 的可选组件（withRegistry）。
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "netlib/EventLoop.h"
#include "client/RpcClient.h"
#include "rpc.pb.h"

namespace config {
inline constexpr double kHeartbeatSec = 5.0; // Provider 心跳周期
} // namespace config

namespace twigrpc {

class Provider {
public:
    Provider(netlib::Endpoint registryAddr, twigrpc::Instance self);
    ~Provider();

    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    void start(); // Register + 周期心跳
    void stop();

private:
    void register_(); // Register RPC（幂等）
    void beat();

    // 独立线程 + loop：心跳不阻塞业务 IO
    netlib::EventLoop loop_;
    std::thread th_;
    std::unique_ptr<RpcClient> client_;
    netlib::Endpoint registryAddr_;
    twigrpc::Instance self_;
    netlib::TimerId timer_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<bool> registered_{false};
};

} // namespace twigrpc
