#include "server/RpcServer.h"
#include "codec/Codec.h"
#include "codec/Protocol.h"
#include "heartbeat/Provider.h"
#include "netlib/Buffer.h"
#include "netlib/TcpServer.h"
#include "netlib/ThreadPool.h"
#include "stats/AdminEndpoint.h"
#include "stats/Collector.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

namespace {
size_t shardOfCurrentThread() {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}
} // namespace

namespace twigrpc {

using netlib::Buffer;
using netlib::TcpConnectionPtr;

// PerConnState 完整定义（从 RpcServer.h 移至此处，避免头文件 include netlib/Buffer.h）
struct RpcServer::PerConnState {
    // 拆包状态机：Codec 无状态，Buffer 即状态（每连接独立）
    netlib::Buffer pendingInput; // 未解完的半帧数据
    std::atomic<int> inFlight{0};
    // requestId → CancelToken。只在连接所属 IO 线程读写
    // （handleFrame 与 worker 完成后的 runInLoop 同线程），无需锁。
    std::unordered_map<uint64_t, std::shared_ptr<CancelToken>> inflight;
};

RpcServer::RpcServer(netlib::EventLoop* mainLoop, netlib::Endpoint listenAddr,
                     int ioThreads, int workers, uint16_t adminPort)
    : listenAddr_(listenAddr) {
    tcp_ = std::make_unique<netlib::TcpServer>(mainLoop, listenAddr, ioThreads);
    workers_ = std::make_unique<netlib::ThreadPool>(static_cast<size_t>(workers));
    // 分片覆盖 IO 拒答与 worker 完成两条热路径，避免多线程挤同一 shard
    size_t shards = static_cast<size_t>(std::max(ioThreads, workers));
    collector_ = std::make_unique<stats::Collector>(shards);
    // 注意：registry_ 先声明（handler 表），workers_ 后声明（析构时先 join）
    tcp_->setMessageCb([this](const TcpConnectionPtr& c, Buffer& b) { onMessage(c, b); });
    tcp_->setConnCb([](const TcpConnectionPtr& c) {
        // 连接建立：挂每连接状态（拆包 Buffer + inFlight 计数 + 取消表）
        c->setContext(std::make_shared<RpcServer::PerConnState>());
        std::fprintf(stderr, "[RpcServer] conn from %s\n",
                     c->peerAddr().toString().c_str());
    });
    if (adminPort != 0) {
        admin_ = std::make_unique<stats::AdminEndpoint>(mainLoop, collector_.get(),
                                                        adminPort);
    }
}

RpcServer::~RpcServer() {
    stop();
}

uint16_t RpcServer::listenPort() const {
    return tcp_->listenPort();
}

void RpcServer::serve() {
    if (started_) return;
    started_ = true;
    // 指标预注册：方法表在此后只读，record 永不修改 map 结构（TSan 约束）
    for (const auto& n : registry_.names()) collector_->preRegister(n);
    collector_->setReady(true); // /healthz 与 ready gauge 同源
    tcp_->start();
    if (admin_) admin_->start(); // 管理端口（/metrics /healthz /stats）
    if (provider_) provider_->start(); // 注册中心心跳组件
}

void RpcServer::stop() {
    if (!started_) return;
    started_ = false; // 重入保护
    stopping_ = true; // 先拒新，再通知存量连接
    collector_->setReady(false); // 排空期 /healthz=503，admin 先留着让 scrape 仍能看见 in_flight
    if (provider_) provider_->stop();
    // 排空：向所有连接广播 GOAWAY（各自 IO 线程内组帧发送），
    // 等在途 handler 全部回包（上限 kDrainTimeoutMs，超时不再等）。
    tcp_->forEachConnection(
        [this](const TcpConnectionPtr& c) { sendGoaway(c); });
    auto drainDeadline = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(config::kDrainTimeoutMs);
    while (collector_->inFlight() > 0 &&
           std::chrono::steady_clock::now() < drainDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // 超时兑底：强制掐断残余连接（forceClose + join），收掉尾部
    tcp_->stop();
    if (admin_) admin_->stop();
}

void RpcServer::withRegistry(netlib::Endpoint registryAddr, const std::string& service,
                              const std::string& instanceId,
                              const std::string& advertiseIp) {
    twigrpc::Instance self;
    self.set_service(service);
    // 默认实例名：hostname-pid。容器内进程都是 PID 1，单用 pid 会令多实例
    // 在注册中心互相覆盖；拼 hostname（docker 默认每服务唯一、裸机即机器名）
    // 跨实例唯一，无需部署方显式传 --instance。
    if (instanceId.empty()) {
        char host[256] = {};
        ::gethostname(host, sizeof(host) - 1);
        self.set_instance_id(std::string(host) + "-" + std::to_string(::getpid()));
    } else {
        self.set_instance_id(instanceId);
    }
    // advertise 地址：显式指定优先；缺省从「本机→注册中心」的出口 IP 自动
    // 推导（UDP 探测不发包），跨机部署无需手动配置（多网卡/NAT 时才需显式指定）
    self.set_ip(advertiseIp.empty() ? netlib::localIpFor(registryAddr)
                                    : advertiseIp);
    self.set_port(listenPort()); // serve() 前调用时 listenPort 已可用（构造即 bind）
    self.set_healthy(true);
    provider_ = std::make_unique<Provider>(std::move(registryAddr), std::move(self));
}

void RpcServer::onMessage(const TcpConnectionPtr& conn, Buffer& buf) {
    // IO 线程：新数据并入连接级 pending，循环解帧
    auto state = std::any_cast<std::shared_ptr<PerConnState>>(conn->getContext());
    if (!state) return;
    state->pendingInput.append(buf.peek(), buf.readableBytes());
    buf.retrieveAll();

    while (true) {
        auto r = Codec::decode(state->pendingInput);
        if (r.kind == Codec::DecodeResult::NEED_MORE) break;
        if (r.kind == Codec::DecodeResult::ERROR) {
            // 协议错位不可恢复：GOAWAY + 断连
            std::fprintf(stderr, "[RpcServer] protocol error from %s, goaway\n",
                         conn->peerAddr().toString().c_str());
            sendGoaway(conn);
            conn->forceClose();
            return;
        }
        handleFrame(conn, r.frame);
    }
}

void RpcServer::handleFrame(const TcpConnectionPtr& conn, const Frame& req) {
    auto state = std::any_cast<std::shared_ptr<PerConnState>>(conn->getContext());

    if (req.msgType == MsgType::PING) {
        // 心跳：IO 线程直接回 PONG（不进 worker 池）
        Frame pong;
        pong.msgType = MsgType::PONG;
        pong.requestId = req.requestId;
        Buffer out;
        Codec::encode(pong, out);
        conn->send(std::string_view(out.peek(), out.readableBytes()));
        return;
    }

    if (req.msgType == MsgType::CANCEL) {
        // 取消在途请求：置位 token + 通知钩子（树递归取消下游由钩子完成）
        if (state) {
            auto it = state->inflight.find(req.requestId);
            if (it != state->inflight.end()) {
                it->second->cancel();
                cancelHook_.invoke(req.requestId, it->second->taskId());
            }
            // 查不到：请求尚未入表或已完成，丢弃
        }
        return;
    }

    if (req.msgType != MsgType::REQUEST) return; // 服务端忽略其他类型

    // 停机排空期：新请求一律 BUSY 快速失败（不进 worker，不计在途）
    if (stopping_.load()) {
        collector_->recordReject(shardOfCurrentThread(), req.method,
                                 static_cast<int>(Status::BUSY));
        sendResponse(conn, req.requestId, Status::BUSY, "", "draining");
        return;
    }

    // BUSY 背压：快速失败（不进 inflight 表）
    if (state && state->inFlight.load() >= config::kMaxInFlightPerConn) {
        collector_->recordReject(shardOfCurrentThread(), req.method,
                                 static_cast<int>(Status::BUSY));
        sendResponse(conn, req.requestId, Status::BUSY, "", "in-flight limit");
        return;
    }
    if (state) state->inFlight.fetch_add(1);
    collector_->addInFlight(1); // 全局在途：排空停机等它归零，/metrics 同源

    auto token = std::make_shared<CancelToken>(req.deadlineUnixMs, req.taskId);
    if (state) state->inflight[req.requestId] = token;
    if (req.taskId != 0) requestHook_.invoke(req.taskId, req.deadlineUnixMs);

    uint64_t id = req.requestId;
    auto connWeak = std::weak_ptr<netlib::TcpConnection>(conn);
    std::string method = req.method; // 指标用
    auto t0 = std::chrono::steady_clock::now(); // 慢调用计时起点
    // 投递 worker 池：dispatch + Responder 打包
    workers_->add([this, connWeak, req, id, method = std::move(method), t0,
                  token]() {
        std::string respBody;
        Status st;
        std::string errDetail;
        registry_.dispatch(req, *token, &respBody, &st, &errDetail);

        // 指标：耗时（微秒）+ 状态，按当前线程分片（无锁累加）
        double us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        collector_->record(shardOfCurrentThread(), method, static_cast<int>(st), us);

        // 慢调用日志：超过 P99 阈值时 WARN（只记方法/耗时/requestId，不记 payload）
        if (us > collector_->slowThresholdUs()) {
            std::fprintf(stderr,
                         "[SLOW] method=%s cost=%.0fus requestId=%llu (p99=%.0fus)\n",
                         method.c_str(), us, static_cast<unsigned long long>(id),
                         collector_->p99());
        }

        if (auto c = connWeak.lock()) {
            // Responder：回 IO 线程组帧发送（必须回 IO 线程），末尾清理取消表
            c->getLoop()->runInLoop([c, id, st, respBody = std::move(respBody),
                                     errDetail = std::move(errDetail), this, token]() {
                // 已取消：无论 handler 结果如何都回 CANCELLED（只回这一帧）
                Status final = token->cancelled() ? Status::CANCELLED : st;
                sendResponse(c, id, final, respBody, errDetail);
                auto s = std::any_cast<std::shared_ptr<PerConnState>>(c->getContext());
                if (s) {
                    s->inflight.erase(id);
                    s->inFlight.fetch_sub(1);
                }
                collector_->addInFlight(-1);
                if (token->taskId() != 0) doneHook_.invoke(token->taskId());
            });
        } else {
            // 连接已断：无需回包，但全局在途与入站任务出口仍要收尾
            collector_->addInFlight(-1);
            if (token->taskId() != 0) doneHook_.invoke(token->taskId());
        }
    });
}

void RpcServer::sendResponse(const TcpConnectionPtr& conn, uint64_t requestId,
                              Status st, const std::string& body,
                              const std::string& errDetail) {
    // IO 线程执行（runInLoop 保证）
    Frame resp;
    resp.msgType = MsgType::RESPONSE;
    resp.status = st;
    resp.requestId = requestId;
    resp.body = (st == Status::OK) ? body : errDetail;
    Buffer out;
    Codec::encode(resp, out);
    // T31：帧进暂存队列，loop 每圈末尾 flushQueued 统一 writev
    conn->queueFrame(std::move(out));
}

void RpcServer::sendGoaway(const TcpConnectionPtr& conn) {
    // GOAWAY：通知对端本端即将停机、别再发新请求；在途请求照常回包。
    // 只发帧不断连，断连时机交给调用方（排空超时后 forceClose）
    Frame goaway;
    goaway.msgType = MsgType::GOAWAY;
    Buffer out;
    Codec::encode(goaway, out);
    conn->send(std::string_view(out.peek(), out.readableBytes()));
}

} // namespace twigrpc
