#pragma once
// twigrpc::RegistryWatcher —— 客户端侧服务发现：
// 30s 轮询 Discover 刷新本地实例表 + 重建 Balancer 环（加锁交换 shared_ptr，读侧无锁）；
// invalidate() 供失败路径触发立即刷新。
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "balancer/Balancer.h"
#include "rpc.pb.h"

namespace config {
inline constexpr double kRefreshSec = 30.0; // 常规轮询周期
} // namespace config

namespace twigrpc {

class RpcClient; // 前置声明（避免与 RpcClient.h 循环包含）

class RegistryWatcher {
public:
    enum class BalancerType { RoundRobin, ConsistentHash };

    RegistryWatcher(netlib::EventLoop* loop, netlib::Endpoint registryAddr,
                    const std::string& service,
                    BalancerType type = BalancerType::RoundRobin);
    ~RegistryWatcher();

    RegistryWatcher(const RegistryWatcher&) = delete;
    RegistryWatcher& operator=(const RegistryWatcher&) = delete;

    void start();
    void stop();

    // 立即刷新（失败路径插队调用）
    void invalidate();

    // 过滤 healthy==false 后交 Balancer 选实例。
    // 按值返回：快照随时可能被 refresh 替换，返回引用/指针会悬空。
    std::optional<twigrpc::Instance> pickInstance(std::string_view key = "");

    // 当前健康实例快照
    std::vector<twigrpc::Instance> healthyInstances() const;

    uint64_t version() const { return version_.load(); }

private:
    void refresh(); // 一次 Discover
    void scheduleRetry(); // 失败后 0.5s 快速重试（单飞）

    netlib::EventLoop* loop_;
    netlib::Endpoint registryAddr_;
    std::string service_;

    // 独立连接到 registry 的客户端（同步 call，跑在 loop 线程定时回调里，
    // 流量低可接受；P4 可换异步）
    std::unique_ptr<RpcClient> client_;

    mutable std::mutex mtx_;
    std::shared_ptr<const std::vector<twigrpc::Instance>> snapshot_;
    std::unique_ptr<Balancer> balancer_;
    std::atomic<uint64_t> version_{0};
    netlib::TimerId timer_ = 0;
    std::atomic<bool> started_{false};
    std::atomic<bool> retryPending_{false};
};

} // namespace twigrpc
