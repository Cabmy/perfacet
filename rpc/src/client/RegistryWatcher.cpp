#include "client/RegistryWatcher.h"

#include <cstdio>

#include "client/RpcClient.h"

namespace twigrpc {

RegistryWatcher::RegistryWatcher(netlib::EventLoop* loop,
                                 netlib::Endpoint registryAddr,
                                 const std::string& service, BalancerType type)
    : loop_(loop), registryAddr_(std::move(registryAddr)),
      service_(service) {
    if (type == BalancerType::ConsistentHash) {
        balancer_ = std::make_unique<ConsistentHashBalancer>();
    } else {
        balancer_ = std::make_unique<RoundRobinBalancer>();
    }
}

RegistryWatcher::~RegistryWatcher() {
    stop();
}

void RegistryWatcher::start() {
    if (started_.exchange(true)) return;
    client_ = std::make_unique<RpcClient>(registryAddr_, 1);
    client_->waitConnected(500);
    // 立即刷新一次，再挂周期定时
    loop_->runInLoop([this]() { refresh(); });
    timer_ = loop_->runEvery(config::kRefreshSec, [this]() { refresh(); });
}

void RegistryWatcher::stop() {
    if (!started_.exchange(false)) return;
    loop_->cancel(timer_);
}

void RegistryWatcher::invalidate() {
    // 失败路径立即刷新（插队）
    loop_->runInLoop([this]() { refresh(); });
}

void RegistryWatcher::refresh() {
    if (!client_) return;
    try {
        twigrpc::DiscoverRequest req;
        req.set_service(service_);
        // 同步调用（loop 线程内，registry 流量低可接受）
        auto resp = client_->call<twigrpc::DiscoverResponse>(
            "twigrpc.Registry.Discover", req, CallOpts{1000});

        std::vector<twigrpc::Instance> healthy;
        for (const auto& inst : resp.instances()) {
            if (inst.healthy()) healthy.push_back(inst);
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            snapshot_ = std::make_shared<const std::vector<twigrpc::Instance>>(
                std::move(healthy));
        }
        version_.store(resp.version());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Watcher] discover failed: %s\n", e.what());
        // 快速重试（建连竞态/瞬时故障；仅挂一个，避免叠加周期刷新）
        scheduleRetry();
    }
}

void RegistryWatcher::scheduleRetry() {
    if (retryPending_.exchange(true)) return;
    loop_->runAfter(0.5, [this]() {
        retryPending_ = false;
        if (started_.load()) refresh();
    });
}

std::optional<twigrpc::Instance> RegistryWatcher::pickInstance(std::string_view key) {
    std::shared_ptr<const std::vector<twigrpc::Instance>> snap;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap = snapshot_;
    }
    if (!snap || snap->empty()) return std::nullopt;
    // 持有 snap 期间选出实例并拷贝返回（防止快照被并发替换后悬空）
    const twigrpc::Instance* picked = balancer_->pick(*snap, key);
    if (!picked) return std::nullopt;
    return *picked;
}

std::vector<twigrpc::Instance> RegistryWatcher::healthyInstances() const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!snapshot_) return {};
    return *snapshot_;
}

} // namespace twigrpc
