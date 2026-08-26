#pragma once
// twigrpc::RpcServer —— 组装者：只做装配与线程编排，不含业务逻辑。
// 数据面：Sub-Reactor 收完整帧 → 查注册表 → 投递 worker 池 → handler 执行
//        → runInLoop 回 IO 线程组响应帧（writev 聚合写）。
// 每连接 inFlight 超限立即回 BUSY 帧，快速失败。
// 取消：REQUEST 到达时建 CancelToken 入 inflight 表；CANCEL 帧置位 token，
// handler 协作轮询，worker 结束时若已取消则回 CANCELLED。
// 停机：stop() 先拒新请求并向所有连接广播 GOAWAY，等在途处理完
//        （上限 kDrainTimeoutMs），再强制收尾残余连接。

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

// 前向声明（瘦头文件：完整定义在 RpcServer.cpp 中 include）
namespace netlib { class Buffer; class EventLoop; class TcpConnection; class TcpServer; class ThreadPool; }
namespace twigrpc {
    class CancelToken;
    class Provider;
    enum class MsgType : uint8_t;
    enum class Status : uint8_t;
    struct Frame;
    namespace stats { class AdminEndpoint; }
    namespace stats { class Collector; }
}

#include "netlib/Endpoint.h"
#include "common/AtomicHook.h"
#include "dispatcher/MethodRegistry.h"

namespace config {
// RpcServer 可调参数（集中在 config 命名空间）
inline constexpr int kMaxInFlightPerConn = 256;      // 每连接在途上限（BUSY 背压）
inline constexpr int kWorkerThreads = 4;             // handler 执行线程数
inline constexpr int kServerIoThreads = 2;           // sub-reactor 数
inline constexpr int kDrainTimeoutMs = 3000;         // 停机排空上限，超时强制收尾
} // namespace config

namespace twigrpc {

class RpcServer {
public:
    // listenAddr：TCP（port=0 时由内核分配，测试用）或 UDS Endpoint。
    // adminPort：管理端口。默认 0 = 不启（避免测试多实例冲突）；
    // 生产进程显式传 config::kAdminPort。
    explicit RpcServer(netlib::EventLoop* mainLoop,
                       netlib::Endpoint listenAddr,
                       int ioThreads = config::kServerIoThreads,
                       int workers = config::kWorkerThreads,
                       uint16_t adminPort = 0);
    ~RpcServer();

    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;

    template <typename Req, typename Resp>
    void bind(const std::string& name,
              std::function<Resp(const Req&, const CancelToken&)> f) {
        registry_.bind<Req, Resp>(name, std::move(f));
    }

    // 可选钩子（给任务树，rpc 不 include agent；均为原始数值）。
    // 装配方在任意线程 set，IO/worker 线程在请求路径上读——槽位原子，
    // serve() 之后挂钩、运行中摘钩都安全（见 AtomicHook）。
    // CANCEL 帧到达 IO 线程、token->cancel() 之后调用。
    using CancelHook = std::function<void(uint64_t requestId, uint64_t taskId)>;
    void setCancelHook(CancelHook cb) { cancelHook_.set(std::move(cb)); }
    // REQUEST 帧到达 IO 线程、建 token 之后调用（taskId=0 不调用）——
    // 供 Runtime adopt 入站任务。
    using RequestHook = std::function<void(uint64_t taskId, uint64_t deadlineUnixMs)>;
    void setRequestHook(RequestHook cb) { requestHook_.set(std::move(cb)); }
    // 入站请求收尾时调用：响应已发出（或连接已断无需回包），taskId=0 不调——
    // 供 Runtime 结束 adopt 的任务（与 RequestHook 对称）。
    using DoneHook = std::function<void(uint64_t taskId)>;
    void setDoneHook(DoneHook cb) { doneHook_.set(std::move(cb)); }

    void serve();       // 启动 accept（须在 mainLoop 线程或之前调用）
    void stop();        // 排空停机：拒新 + GOAWAY → 等在途（上限 kDrainTimeoutMs）→ 强收
    // 直接查 Acceptor（构造即 bind/listen，serve() 前也可用）；UDS 恒 0
    uint16_t listenPort() const;
    const netlib::Endpoint& listenAddr() const { return listenAddr_; }

    // 可选：接入注册中心（自注册 + 周期心跳）。serve() 前调用。
    // advertiseIp：向注册中心上报的本机 IP（多网卡/NAT 后面时显式指定）；
    // 缺省自动推导：UDP 探测本机访问 registryAddr 的出口 IP。
    void withRegistry(netlib::Endpoint registryAddr, const std::string& service,
                      const std::string& instanceId = "",
                      const std::string& advertiseIp = "");

private:
    struct PerConnState; // 定义在 .cpp 中（含 netlib::Buffer 值成员）

    void onMessage(const std::shared_ptr<netlib::TcpConnection>& conn, netlib::Buffer& buf);
    void handleFrame(const std::shared_ptr<netlib::TcpConnection>& conn, const Frame& req);
    void sendResponse(const std::shared_ptr<netlib::TcpConnection>& conn, uint64_t requestId,
                      Status st, const std::string& body,
                      const std::string& errDetail);
    // 向单条连接发 GOAWAY 帧（在连接所属 IO 线程内调用），不主动断连
    void sendGoaway(const std::shared_ptr<netlib::TcpConnection>& conn);

    // 声明顺序=构造顺序，析构逆序：workers_ 最后声明 → 最先析构（join 线程），
    // 保证 registry_/tcp_/collector_ 等 worker 任务执行期间引用的成员
    // 全部在 workers_ 之后析构（handler 可能仍在执行，如慢调用）。
    MethodRegistry registry_;
    netlib::Endpoint listenAddr_;
    std::unique_ptr<netlib::TcpServer> tcp_;
    std::unique_ptr<stats::Collector> collector_;         // 指标/就绪/在途（排空也读这里）
    std::unique_ptr<Provider> provider_;            // 可选：注册中心心跳
    std::unique_ptr<stats::AdminEndpoint> admin_;   // 可选：管理端口
    std::unique_ptr<netlib::ThreadPool> workers_;          // 最先析构：join 所有 handler

    AtomicHook<void(uint64_t, uint64_t)> cancelHook_;
    AtomicHook<void(uint64_t, uint64_t)> requestHook_;
    AtomicHook<void(uint64_t)> doneHook_;
    bool started_ = false;
    std::atomic<bool> stopping_{false}; // 停机中：拒新请求
};

} // namespace twigrpc
