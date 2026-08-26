#include "client/RpcConn.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>

#include "client/RpcException.h"

namespace twigrpc {

using netlib::Buffer;
using netlib::TcpConnectionPtr;

// 连接级 id 窗口基址：每连接独占 2^32 宽度，requestId 进程内不重号
namespace {
std::atomic<uint64_t> g_connIdBase{1};
} // namespace

RpcConn::RpcConn(netlib::Endpoint addr)
    : tcp_(&loop_), sweeper_(&loop_, &pending_,
                             [this](uint64_t id) { cancel(id); }),
      addr_(std::move(addr)) {
    nextId_.store(g_connIdBase.fetch_add(1ULL << 32));
    tcp_.setMsgCb([this](const TcpConnectionPtr& c, Buffer& b) { onMessage(c, b); });
    tcp_.setConnCb([this](const TcpConnectionPtr&) {
        state_ = State::kUp;
        lastActiveMs_ = nowMs();
    });
    tcp_.setCloseCb([this](const TcpConnectionPtr&) {
        state_ = State::kDown;
        // 断连：所有在途请求失败（恰好 set 一次由 PendingTable 保证）
        pending_.failAll(std::make_exception_ptr(
            RpcException(Status::CONN_CLOSED, "connection closed")));
    });
    ioThread_ = std::thread([this]() { loop_.loop(); });
    tcp_.connect(addr_);
    sweeper_.start();
    startPing();
}

RpcConn::~RpcConn() {
    sweeper_.stop();
    loop_.cancel(pingTimer_);
    tcp_.disconnect();
    loop_.quit();
    if (ioThread_.joinable()) ioThread_.join();
}

uint64_t RpcConn::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t RpcConn::unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool RpcConn::waitConnected(int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (state_ == State::kUp) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return state_ == State::kUp;
}

std::future<Frame> RpcConn::callAsync(const std::string& method,
                                      const std::string& body,
                                      const CallOpts& opts,
                                      uint64_t* requestIdOut) {
    // 入口拒绝：对端已 GOAWAY 或连接不可用，不占在途表、不发帧，
    // 立即以异常 future 返回（换连接重打是上层策略，不在本层）。
    if (goaway_.load() || state_.load() != State::kUp) {
        if (requestIdOut) *requestIdOut = 0;
        std::promise<Frame> p;
        p.set_exception(std::make_exception_ptr(RpcException(
            Status::CONN_CLOSED, goaway_.load() ? "goaway" : "not connected")));
        return p.get_future();
    }
    uint64_t id = nextId_.fetch_add(1);
    if (requestIdOut) *requestIdOut = id;
    Deadline dl = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(opts.timeoutMs);
    auto fut = pending_.insert(id, dl);

    lastActiveMs_ = nowMs();

    // 组帧并投递到 IO 线程发送（deadline 为绝对 Unix 毫秒，写入 TLV）
    Frame req;
    req.msgType = MsgType::REQUEST;
    req.requestId = id;
    req.method = method;
    req.body = body;
    req.deadlineUnixMs = unixMs() + static_cast<uint64_t>(opts.timeoutMs);
    req.taskId = opts.taskId;
    Buffer out;
    Codec::encode(req, out);
    std::string wire(out.peek(), out.readableBytes());

    loop_.runInLoop([this, wire = std::move(wire), id]() {
        auto conn = tcp_.connection();
        if (conn && conn->connected()) {
            conn->send(wire);
        } else {
            // 连接尚未建立/已断开：请求发不出去，立即失败
            // （不静默丢帧等 sweeper 超时——那是假超时）
            if (auto pc = pending_.take(id)) {
                pc->pr.set_exception(std::make_exception_ptr(
                    RpcException(Status::CONN_CLOSED, "not connected")));
            }
        }
    });
    return fut;
}

void RpcConn::cancel(uint64_t requestId) {
    // 任意线程可调：投递到 IO 线程发帧。不摘 pending——promise 由
    // RESPONSE CANCELLED 或 sweeper 兜底完成。
    loop_.runInLoop([this, requestId]() {
        auto conn = tcp_.connection();
        if (!conn || !conn->connected()) return;
        Frame c;
        c.msgType = MsgType::CANCEL;
        c.requestId = requestId;
        Buffer out;
        Codec::encode(c, out);
        conn->send(std::string(out.peek(), out.readableBytes()));
    });
}

void RpcConn::onMessage(const TcpConnectionPtr& conn, Buffer& buf) {
    // IO 线程：循环解帧
    while (true) {
        auto r = Codec::decode(buf);
        if (r.kind == Codec::DecodeResult::NEED_MORE) break;
        if (r.kind == Codec::DecodeResult::ERROR) {
            std::fprintf(stderr, "[RpcConn] protocol error, closing\n");
            conn->forceClose();
            return;
        }
        lastActiveMs_ = nowMs();
        const Frame& f = r.frame;
        if (f.msgType == MsgType::RESPONSE) {
            // 原子摘除：摘到者 set_value；摘不到说明迟到（已超时/已断连），丢弃
            if (auto pc = pending_.take(f.requestId)) {
                pc->pr.set_value(f);
            } else {
                std::fprintf(stderr, "[RpcConn] late response id=%llu dropped\n",
                             static_cast<unsigned long long>(f.requestId));
            }
        } else if (f.msgType == MsgType::GOAWAY) {
            // 对端排空停机：本连接不再发新调用；在途请求照常等 RESPONSE，
            // 不 failAll、不断连（由对端在排空后收尾）。
            goaway_.store(true);
        } else if (f.msgType == MsgType::PONG) {
            // 心跳回包：不入 pending 表
        }
    }
}

void RpcConn::startPing() {
    pingTimer_ = loop_.runEvery(config::kIdlePingSec, [this]() {
        uint64_t now = nowMs();
        if (now - lastActiveMs_.load() <
            static_cast<uint64_t>(config::kIdlePingSec * 1000)) {
            return; // 活跃连接不发心跳
        }
        Frame ping;
        ping.msgType = MsgType::PING;
        ping.requestId = 0; // 心跳不入表
        Buffer out;
        Codec::encode(ping, out);
        std::string wire(out.peek(), out.readableBytes());
        auto conn = tcp_.connection();
        if (conn && conn->connected()) {
            conn->send(wire);
        }
    });
}

} // namespace twigrpc
