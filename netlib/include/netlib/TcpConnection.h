#pragma once
// netlib::TcpConnection —— 协议无关的单条连接：读写缓冲、回调注入、生命周期。
// 生命周期统一 shared_ptr + enable_shared_from_this。
// 线程安全契约：
// 1. send() 任意线程可调，内部自动投递回 IO 线程；
// 2. epoll 资源只允许所属 IO 线程操作；
// 3. 关闭顺序固定：先摘除全部回调 → 再 close fd → 最后释放对象。
#include <any>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

#include "netlib/Buffer.h"
#include "netlib/Channel.h"
#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "netlib/Socket.h"

namespace netlib {

class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using ConnCb = std::function<void(const TcpConnectionPtr&)>;
    using MsgCb = std::function<void(const TcpConnectionPtr&, Buffer&)>;
    using WriteCompleteCb = std::function<void(const TcpConnectionPtr&)>;
    using CloseCb = std::function<void(const TcpConnectionPtr&)>;

    TcpConnection(EventLoop* loop, std::unique_ptr<Socket> sock,
                  const Endpoint& local, const Endpoint& peer);
    ~TcpConnection();

    void attachToLoop(); // 创建后由所属 loop 线程调用：注册读事件 + connCb

    void send(std::string_view data);          // 任意线程安全
    void send(Buffer&& buf);                   // 任意线程安全（零拷贝接管）
    void shutdown();                           // 优雅关闭：等待输出缓冲排空后关闭连接
    void forceClose();                         // 立即关闭

    void setConnCb(ConnCb cb) { connCb_ = std::move(cb); }
    void setMsgCb(MsgCb cb) { msgCb_ = std::move(cb); }
    void setWriteCompleteCb(WriteCompleteCb cb) { writeCompleteCb_ = std::move(cb); }
    void setCloseCb(CloseCb cb) { closeCb_ = std::move(cb); } // 供 TcpServer 注入

    void setContext(std::any ctx) {
        std::lock_guard<std::mutex> lk(ctxMtx_);
        context_ = std::move(ctx);
    }
    std::any getContext() {
        std::lock_guard<std::mutex> lk(ctxMtx_);
        return context_;
    }

    EventLoop* getLoop() const { return loop_; }
    int fd() const { return sock_->getFd(); }
    const Endpoint& localAddr() const { return local_; }
    const Endpoint& peerAddr() const { return peer_; }
    bool connected() const { return state_ == State::kConnected; }

    // 帧暂存队列（IO 线程内调用）
    void queueFrame(Buffer&& frame);
    void flushQueued(); // 由 EventLoop 每圈末尾调用，writev 聚合写出

private:
    enum class State { kConnecting, kConnected, kDisconnecting, kDisconnected };

    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();
    void sendInLoop(std::string_view data);
    void sendBufferInLoop(Buffer&& buf);
    void closeInLoop();

    EventLoop* loop_;
    std::unique_ptr<Socket> sock_;
    std::unique_ptr<Channel> ch_;
    Buffer input_;
    Buffer output_;
    std::atomic<State> state_{State::kConnecting};
    bool writing_ = false;

    std::any context_;
    std::mutex ctxMtx_;

    // 帧聚合暂存（IO 线程独占访问）
    std::deque<Buffer> outQueue_;
    size_t outQueueBytes_ = 0;

    Endpoint local_;
    Endpoint peer_;

    ConnCb connCb_;
    MsgCb msgCb_;
    WriteCompleteCb writeCompleteCb_;
    CloseCb closeCb_;
};

} // namespace netlib
