#include "heartbeat/Provider.h"

#include <cstdio>

#include "registry.pb.h"

namespace twigrpc {

Provider::Provider(netlib::Endpoint registryAddr, twigrpc::Instance self)
    : registryAddr_(std::move(registryAddr)), self_(std::move(self)) {}

Provider::~Provider() {
    stop();
    if (th_.joinable()) th_.join();
}

void Provider::start() {
    if (started_.exchange(true)) return;
    th_ = std::thread([this]() { loop_.loop(); });
    // loop 线程内：建 client → 等连上再 Register → 挂周期心跳
    // （未注册成时由 beat / 0.5s 快重试兑现，不空等 5s 心跳）
    loop_.runInLoop([this]() {
        client_ = std::make_unique<RpcClient>(registryAddr_, 1);
        client_->waitConnected(500);
        register_();
        timer_ = loop_.runEvery(config::kHeartbeatSec, [this]() { beat(); });
    });
}

void Provider::stop() {
    if (!started_.exchange(false)) return;
    loop_.cancel(timer_);
    loop_.quit();
}

void Provider::register_() {
    if (!client_) return;
    try {
        twigrpc::RegisterRequest req;
        *req.mutable_instance() = self_;
        client_->call<twigrpc::RegisterResponse>(
            "twigrpc.Registry.Register", req, CallOpts{1000});
        registered_ = true;
        std::fprintf(stderr, "[Provider] registered %s/%s\n",
                     self_.service().c_str(), self_.instance_id().c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Provider] register failed: %s\n", e.what());
        // 建连竞态/瞬时失败：0.5s 后再打，不等心跳周期
        if (started_.load() && !registered_.load()) {
            loop_.runAfter(0.5, [this]() {
                if (started_.load() && !registered_.load()) register_();
            });
        }
    }
}

void Provider::beat() {
    if (!client_) return;
    if (!registered_.load()) {
        register_(); // 首次注册未成（建连竞态等）：心跳周期内重试
        return;
    }
    try {
        twigrpc::HeartbeatRequest req;
        req.set_instance_id(self_.instance_id());
        client_->call<twigrpc::HeartbeatResponse>("twigrpc.Registry.Heartbeat", req,
                                                CallOpts{500});
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[Provider] heartbeat failed: %s\n", e.what());
    }
}

} // namespace twigrpc
