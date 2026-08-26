#pragma once
// twigrpc::RpcConn —— 单条复用连接：一个 IO 线程 + PendingTable + Codec 拆包状态。
// 多路复用：同一连接上并发多个在途请求（requestId 区分）。
// 取消：cancel(requestId) 发 CANCEL 帧，不摘 pending（等 RESPONSE CANCELLED 或 sweeper）。
// GOAWAY：对端排空停机信号。置位后不再接受新调用（connected()==false），
// 已在途请求照常等响应，不 failAll、不主动断连。
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "netlib/TcpClient.h"
#include "client/PendingTable.h"
#include "client/TimeoutSweeper.h"
#include "codec/Codec.h"

namespace config {
inline constexpr double kIdlePingSec = 5.0; // 空闲心跳周期
} // namespace config

namespace twigrpc {

struct CallOpts {
    int timeoutMs = 500;      // 只用于计算绝对 deadline（写入 TLV + sweeper 兜底）
    std::string hashKey;      // 一致性哈希亲和 key（空则全部命中同一节点，仅 RR 模式可留空）
    uint64_t taskId = 0;      // Agent 任务 id（rpc 不解释，仅透传）

    // 便捷构造：常用「只指定超时」与「全显式」两种，避免聚合初始化漏字段告警
    CallOpts() = default;
    explicit CallOpts(int timeout) : timeoutMs(timeout) {}
    CallOpts(int timeout, std::string key, uint64_t task)
        : timeoutMs(timeout), hashKey(std::move(key)), taskId(task) {}
};

class RpcConn {
public:
    explicit RpcConn(netlib::Endpoint addr);
    ~RpcConn();

    RpcConn(const RpcConn&) = delete;
    RpcConn& operator=(const RpcConn&) = delete;

    // 异步调用：立即返回 future。requestIdOut（可选）回填本次调用的 id，
    // 供上层在调用期间对该请求发 CANCEL。
    std::future<Frame> callAsync(const std::string& method, const std::string& body,
                                 const CallOpts& opts = CallOpts{},
                                 uint64_t* requestIdOut = nullptr);

    // 取消在途请求：IO 线程发 CANCEL 帧；promise 仍等 RESPONSE CANCELLED
    // 或 sweeper 兜底，此处不 take。
    void cancel(uint64_t requestId);

    // 等待连接就绪（供上层建立期使用）
    bool waitConnected(int timeoutMs = 500);

    // 可用 = 已连上且未收到 GOAWAY（池子/发现层不再往这条连接发新请求）
    bool connected() const {
        return state_ == State::kUp && !goaway_.load();
    }
    const netlib::Endpoint& addr() const { return addr_; }

private:
    enum class State { kDown, kUp };

    void onMessage(const netlib::TcpConnectionPtr& conn, netlib::Buffer& buf);
    void startPing();
    static uint64_t nowMs();
    static uint64_t unixMs();

    netlib::EventLoop loop_;
    std::thread ioThread_;
    netlib::TcpClient tcp_;
    PendingTable pending_;
    TimeoutSweeper sweeper_;
    // 每连接 2^32 宽度的 id 窗口（构造时从进程级计数器取基）：
    // requestId 进程内全局唯一，供上层（RpcClient 在途表）跨连接按 id 发 CANCEL
    std::atomic<uint64_t> nextId_{0};
    std::atomic<State> state_{State::kDown};
    std::atomic<bool> goaway_{false}; // 对端已声明停机：拒新调用
    netlib::Endpoint addr_;
    netlib::TimerId pingTimer_ = 0;
    std::atomic<uint64_t> lastActiveMs_{0};
};

} // namespace twigrpc
