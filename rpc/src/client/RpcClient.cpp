#include "client/RpcClient.h"

#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace twigrpc {

RpcClient::RpcClient(netlib::Endpoint addr, size_t conns)
    : pool_(std::make_unique<ConnPool>(addr, conns)) {}

RpcClient::RpcClient(netlib::Endpoint registryAddr, const std::string& service,
                     RegistryWatcher::BalancerType type) {
    wloop_ = std::make_unique<netlib::EventLoop>();
    wth_ = std::make_unique<std::thread>([this]() { wloop_->loop(); });
    watcher_ = std::make_unique<RegistryWatcher>(wloop_.get(), std::move(registryAddr),
                                                 service, type);
    watcher_->start();
}

RpcClient::~RpcClient() {
    if (watcher_) watcher_->stop();
    if (wloop_) wloop_->quit();
    if (wth_ && wth_->joinable()) wth_->join();
    std::lock_guard<std::mutex> lk(instMtx_);
    instConns_.clear();
}

bool RpcClient::waitDiscovered(int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (watcher_ && !watcher_->healthyInstances().empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return watcher_ && !watcher_->healthyInstances().empty();
}

Frame RpcClient::callFrame(const std::string& method, const std::string& body,
                           const CallOpts& opts) {
    if (pool_) return callFrameDirect(method, body, opts);
    return callFrameDiscovery(method, body, opts);
}

void RpcClient::trackInflight(uint64_t requestId, std::shared_ptr<RpcConn> conn) {
    std::lock_guard<std::mutex> lk(inflightMtx_);
    inflight_[requestId] = std::move(conn);
}

void RpcClient::untrackInflight(uint64_t requestId) {
    std::lock_guard<std::mutex> lk(inflightMtx_);
    inflight_.erase(requestId);
}

void RpcClient::cancelInflight(uint64_t requestId) {
    std::shared_ptr<RpcConn> conn;
    {
        std::lock_guard<std::mutex> lk(inflightMtx_);
        auto it = inflight_.find(requestId);
        if (it == inflight_.end()) return; // 已完成或不存在
        conn = it->second;
    }
    if (conn) conn->cancel(requestId);
}

Frame RpcClient::waitFrame(std::future<Frame>& fut,
                           const CallOpts& opts, uint64_t requestId) {
    auto st = fut.wait_for(std::chrono::milliseconds(
        static_cast<int64_t>(opts.timeoutMs) + 500));
    untrackInflight(requestId);
    if (st != std::future_status::ready) {
        // sweeper 会随后发出 CANCEL 并失败 promise；此处先抛，future 随之析构
        throw RpcException(Status::TIMEOUT, "client wait timeout");
    }
    try {
        return fut.get();
    } catch (const RpcException&) {
        throw; // sweeper 的 TIMEOUT / 断连的 CONN_CLOSED 原样上抛
    } catch (const std::exception& e) {
        throw RpcException(Status::CONN_CLOSED, e.what());
    }
}

Frame RpcClient::callFrameDirect(const std::string& method,
                                  const std::string& body, const CallOpts& opts) {
    std::shared_ptr<RpcConn> conn = pool_->pick();
    uint64_t id = 0;
    auto fut = conn->callAsync(method, body, opts, &id);
    trackInflight(id, std::move(conn));
    dispatchHook_.invoke(id, opts.taskId);
    return waitFrame(fut, opts, id);
}

Frame RpcClient::callFrameDiscovery(const std::string& method,
                                    const std::string& body, const CallOpts& opts) {
    auto instOpt = watcher_->pickInstance(opts.hashKey);
    if (!instOpt) {
        watcher_->invalidate(); // 触发立即刷新，本次直接失败
        throw RpcException(Status::CONN_CLOSED, "no healthy instance");
    }
    std::shared_ptr<RpcConn> conn = connForInstance(*instOpt);
    if (!conn) {
        watcher_->invalidate();
        throw RpcException(Status::CONN_CLOSED,
                           "connect failed to " + instOpt->instance_id());
    }
    uint64_t id = 0;
    auto fut = conn->callAsync(method, body, opts, &id);
    trackInflight(id, conn);
    dispatchHook_.invoke(id, opts.taskId);
    try {
        return waitFrame(fut, opts, id);
    } catch (...) {
        // 失败路径：invalidate 刷新快照，下次调用换实例
        watcher_->invalidate();
        throw;
    }
}

std::shared_ptr<RpcConn> RpcClient::connForInstance(const twigrpc::Instance& inst) {
    std::lock_guard<std::mutex> lk(instMtx_);
    auto it = instConns_.find(inst.instance_id());
    if (it != instConns_.end() && it->second->connected()) {
        return it->second; // 复用健康连接
    }
    // 断连/不存在：替换新连接。旧连接的 shared_ptr 由在途调用持有，
    // 最后一个使用者结束后自然析构（并发安全的所有权交接）。
    auto conn = std::make_shared<RpcConn>(netlib::Endpoint(inst.ip(), inst.port()));
    instConns_[inst.instance_id()] = conn;
    if (!conn->waitConnected(500)) {
        return nullptr;
    }
    return conn;
}

} // namespace twigrpc
