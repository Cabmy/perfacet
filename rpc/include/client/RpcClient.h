#pragma once
// twigrpc::RpcClient —— 对用户的门面（Facade）：ConnPool + 单次调用。
// call = asyncCall + get（异常转 RpcException）。失败即抛，无内置重试——
// 重试只存在于 agent 层 IRetry 扩展点（禁止第二套策略双轨）。
// 在途表：requestId → 连接，供上层（Runtime 任务树）对在途请求发 CANCEL。
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "netlib/Endpoint.h"
#include "common/AtomicHook.h"
#include "client/ConnPool.h"
#include "client/RpcConn.h"
#include "client/RpcException.h"
#include "client/RegistryWatcher.h"

namespace config {
inline constexpr int kClientConns = 4; // 连接池大小
} // namespace config

namespace twigrpc {

// CallOpts 定义在 RpcConn.h（本头文件已 include）

class RpcClient {
public:
    // 直连模式
    explicit RpcClient(netlib::Endpoint addr, size_t conns = config::kClientConns);
    // 服务发现模式：RegistryWatcher + 按实例惰性建连
    RpcClient(netlib::Endpoint registryAddr, const std::string& service,
              RegistryWatcher::BalancerType type = RegistryWatcher::BalancerType::RoundRobin);
    ~RpcClient();

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    // 同步调用（单次；失败抛 RpcException）
    template <typename Resp, typename Req>
    Resp call(const std::string& method, const Req& req,
              const CallOpts& opts = CallOpts{}) {
        Frame f = callFrame(method, req.SerializeAsString(), opts);
        if (f.status != Status::OK) {
            throw RpcException(f.status, f.body);
        }
        Resp resp;
        if (!resp.ParseFromString(f.body)) {
            throw RpcException(Status::DECODE_ERROR, "response parse failed");
        }
        return resp;
    }

    // 异步调用（future 语义）。注意：每次调用经 std::async 起一个 OS 线程，
    // 适合低并发场景；热路径并发请用 ConnPool×RpcConn::callAsync（IO 线程多路复用）。
    template <typename Resp, typename Req>
    std::future<Resp> asyncCall(const std::string& method, const Req& req,
                                const CallOpts& opts = CallOpts{}) {
        std::string body = req.SerializeAsString();
        return std::async(std::launch::async,
                          [this, method = std::string(method),
                           body = std::move(body), opts]() {
            Frame f = callFrame(method, body, opts);
            if (f.status != Status::OK) {
                throw RpcException(f.status, f.body);
            }
            Resp resp;
            if (!resp.ParseFromString(f.body)) {
                throw RpcException(Status::DECODE_ERROR, "response parse failed");
            }
            return resp;
        });
    }

    void waitConnected(int timeoutMs = 2000) {
        if (pool_) pool_->waitConnected(timeoutMs);
    }
    // 服务发现模式：等首次快照就绪
    bool waitDiscovered(int timeoutMs = 3000);

    // 可选：请求发出后同步回调（调用线程内，callFrame 返回前）。
    // Runtime 借此把 requestId 挂到任务树上。调用线程与装配线程可以不同，
    // 槽位原子（见 AtomicHook）。
    void setDispatchHook(std::function<void(uint64_t requestId, uint64_t taskId)> cb) {
        dispatchHook_.set(std::move(cb));
    }

    // 对在途请求发 CANCEL（查在途表；不在表内则忽略）
    void cancelInflight(uint64_t requestId);

    // 原始帧单次调用（带 taskId/deadline；供 agent Runtime 使用）
    Frame callFrame(const std::string& method, const std::string& body,
                    const CallOpts& opts);

private:
    Frame callFrameDirect(const std::string& method, const std::string& body,
                          const CallOpts& opts);
    Frame callFrameDiscovery(const std::string& method, const std::string& body,
                             const CallOpts& opts);
    // 单次等待 + 异常归一（含在途表登记/摘除）
    Frame waitFrame(std::future<Frame>& fut,
                    const CallOpts& opts, uint64_t requestId);
    // 惰性建连（加锁）。返回 shared_ptr：在途调用期间连接可能被替换，
    // 共享所有权保证旧连接在最后一个使用者结束后才析构。
    std::shared_ptr<RpcConn> connForInstance(const twigrpc::Instance& inst);
    void trackInflight(uint64_t requestId, std::shared_ptr<RpcConn> conn);
    void untrackInflight(uint64_t requestId);

    // 直连模式
    std::unique_ptr<ConnPool> pool_;
    // 服务发现模式
    std::unique_ptr<netlib::EventLoop> wloop_; // watcher 专属 loop
    std::unique_ptr<std::thread> wth_;
    std::unique_ptr<RegistryWatcher> watcher_;
    std::map<std::string, std::shared_ptr<RpcConn>> instConns_;
    std::mutex instMtx_;

    // 在途请求表（requestId → 连接）：CANCEL 发送入口
    std::mutex inflightMtx_;
    std::unordered_map<uint64_t, std::shared_ptr<RpcConn>> inflight_;
    AtomicHook<void(uint64_t, uint64_t)> dispatchHook_;
};

} // namespace twigrpc
