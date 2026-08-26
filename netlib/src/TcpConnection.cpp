#include "netlib/TcpConnection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <sys/uio.h>

#include <deque>
#include <mutex>

namespace netlib {

TcpConnection::TcpConnection(EventLoop* loop, std::unique_ptr<Socket> sock,
                             const Endpoint& local, const Endpoint& peer)
    : loop_(loop), sock_(std::move(sock)), local_(local), peer_(peer) {
    ch_ = std::make_unique<Channel>(loop_, sock_->getFd());
    ch_->setReadCb([this]() { handleRead(); });
    ch_->setWriteCb([this]() { handleWrite(); });
    ch_->setCloseCb([this]() { handleClose(); });
    ch_->setErrorCb([this]() { handleError(); });
    sock_->setNoDelay();
    sock_->setKeepAlive();
}

TcpConnection::~TcpConnection() {
    // fd 由 Socket 析构关闭
}

void TcpConnection::attachToLoop() {
    loop_->assertInLoopThread();
    state_ = State::kConnected;
    ch_->tie(shared_from_this()); // handleEvent 期间保活
    ch_->enableReadingET();
    // 注册帧聚合刷写——loop 每圈末尾统一 writev（IO 线程执行）。
    // 以 weak_ptr 持有：连接析构后条目由 flushAll 自动回收
    loop_->registerFlush(shared_from_this(), [this]() { flushQueued(); });
    if (connCb_) connCb_(shared_from_this());
}

void TcpConnection::send(std::string_view data) {
    if (state_ != State::kConnected) return;
    if (loop_->inLoopThread()) {
        sendInLoop(data);
    } else {
        // 拷贝一份投递回 IO 线程（string_view 不拥有数据）
        loop_->runInLoop(
            [self = shared_from_this(), s = std::string(data)]() {
                self->sendInLoop(s);
            });
    }
}

void TcpConnection::send(Buffer&& buf) {
    if (state_ != State::kConnected) return;
    if (loop_->inLoopThread()) {
        sendBufferInLoop(std::move(buf));
    } else {
        loop_->runInLoop(
            [self = shared_from_this(), b = std::move(buf)]() mutable {
                self->sendBufferInLoop(std::move(b));
            });
    }
}

void TcpConnection::sendInLoop(std::string_view data) {
    Buffer buf;
    buf.append(data);
    sendBufferInLoop(std::move(buf));
}

void TcpConnection::shutdown() {
    State expect = State::kConnected;
    if (state_.compare_exchange_strong(expect, State::kDisconnecting)) {
        loop_->runInLoop([self = shared_from_this()]() { self->closeInLoop(); });
    }
}

void TcpConnection::forceClose() {
    if (state_ == State::kDisconnected) return;
    state_ = State::kDisconnecting;
    loop_->runInLoop([self = shared_from_this()]() { self->closeInLoop(); });
}

void TcpConnection::handleRead() {
    loop_->assertInLoopThread();
    int savedErrno = 0;
    // ET 模式：必须循环读到 EAGAIN，否则内核残留数据不再触发边沿。
    // 单次 readFd 上限约 66KB（writable + extrabuf），大帧/批量场景下
    // 只读一次会导致剩余数据滞留内核，请求只能等超时（大报文压测定位出的缺陷）。
    while (true) {
        ssize_t n = input_.readFd(sock_->getFd(), &savedErrno);
        if (n > 0) {
            if (msgCb_) msgCb_(shared_from_this(), input_);
        } else if (n == 0) {
            handleClose(); // EOF：对端关闭
            return;
        } else {
            if (savedErrno == EINTR) continue;
            if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) break; // 本轮读尽
            handleError();
            return;
        }
    }
}

void TcpConnection::sendBufferInLoop(Buffer&& buf) {
    loop_->assertInLoopThread();
    if (state_ == State::kDisconnected) return;
    ssize_t nwrote = 0;
    size_t remaining = buf.readableBytes();
    if (!writing_ && output_.readableBytes() == 0 && outQueue_.empty()) {
        // 输出缓冲空闲：尝试直写
        nwrote = ::write(sock_->getFd(), buf.peek(), remaining);
        while (nwrote < 0 && errno == EINTR) {
            nwrote = ::write(sock_->getFd(), buf.peek(), remaining);
        }
        if (nwrote >= 0) {
            remaining -= static_cast<size_t>(nwrote);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::perror("TcpConnection::sendBufferInLoop write");
            return;
        }
    }
    if (remaining > 0) {
        output_.append(buf.peek() + (buf.readableBytes() - remaining), remaining);
        if (!writing_) {
            writing_ = true;
            ch_->enableWriting();
        }
    } else {
        if (writeCompleteCb_) writeCompleteCb_(shared_from_this());
    }
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (!writing_) return;
    // ET 模式：循环写到 EAGAIN 或排空，否则部分写后不再有边沿，输出缓冲滞留
    while (output_.readableBytes() > 0) {
        ssize_t n = ::write(sock_->getFd(), output_.peek(), output_.readableBytes());
        if (n > 0) {
            output_.retrieve(static_cast<size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            std::perror("TcpConnection::handleWrite");
            return;
        } else {
            return; // EAGAIN：内核缓冲满，等下一个可写事件
        }
    }
    writing_ = false;
    ch_->disableWriting();
    if (state_ == State::kDisconnecting) {
        closeInLoop(); // shutdown 后排空，现在关闭
    }
    if (writeCompleteCb_) writeCompleteCb_(shared_from_this());
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    if (state_ == State::kDisconnected) return;
    // 关闭顺序固定（不许改）：
    // 1) 先 disableAll 并把 Channel 回调摘除，防止 closeCb 执行期间事件再进来；
    // 2) 再标记断开；
    // 3) 回调 closeCb_（由 TcpServer/TcpClient 摘除连接、释放 shared_ptr）；
    // 4) fd 由 Socket 析构关闭，对象随后释放。
    ch_->disableAll();
    ch_->remove();
    // 摘除自身回调，防止回调执行中对象析构
    ch_->setReadCb(nullptr);
    ch_->setWriteCb(nullptr);
    ch_->setCloseCb(nullptr);
    ch_->setErrorCb(nullptr);
    state_ = State::kDisconnected;
    if (closeCb_) closeCb_(shared_from_this());
}

void TcpConnection::handleError() {
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(sock_->getFd(), SOL_SOCKET, SO_ERROR, &err, &len);
    std::fprintf(stderr, "TcpConnection::handleError fd=%d SO_ERROR=%d\n",
                 sock_->getFd(), err);
    handleClose();
}

void TcpConnection::closeInLoop() {
    loop_->assertInLoopThread();
    if (output_.readableBytes() == 0 && outQueue_.empty()) {
        handleClose();
    } else {
        // 等待写完（handleWrite 排空后关闭）
        state_ = State::kDisconnecting;
    }
}

// ---------------- 帧聚合 ----------------

void TcpConnection::queueFrame(Buffer&& frame) {
    // 仅 IO 线程调用（Responder 末尾经 runInLoop 投回）
    outQueueBytes_ += frame.readableBytes();
    outQueue_.push_back(std::move(frame));
}

void TcpConnection::flushQueued() {
    // EventLoop 每圈末尾（doPending 之后）调用；只在本连接所属 loop 线程执行
    if (outQueue_.empty()) return;
    if (state_ == State::kDisconnected) {
        // 已断开：残留帧直接丢弃；kDisconnecting 仍需排空（优雅关闭语义）
        outQueue_.clear();
        outQueueBytes_ = 0;
        return;
    }
    // 1) 先把 output_ 里未写完的部分也并入队首（v1 简化：直接先写 output_）
    // 组 iovec：每帧一个段（上限 IOV_MAX）
    const size_t kMaxIov = 64;
    iovec iov[kMaxIov];
    size_t nseg = 0;
    size_t total = 0;
    for (auto& f : outQueue_) {
        if (nseg >= kMaxIov) break;
        iov[nseg].iov_base = const_cast<char*>(f.peek());
        iov[nseg].iov_len = f.readableBytes();
        total += f.readableBytes();
        ++nseg;
    }
    ssize_t n = ::writev(sock_->getFd(), iov, static_cast<int>(nseg));
    while (n < 0 && errno == EINTR) {
        n = ::writev(sock_->getFd(), iov, static_cast<int>(nseg));
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::perror("TcpConnection::flushQueued writev");
            handleClose();
            return;
        }
        n = 0; // 内核缓冲满：挂到 output_，开写事件
    }
    size_t written = static_cast<size_t>(n);
    // 把已写字节从队列中消费掉
    while (written > 0 && !outQueue_.empty()) {
        Buffer& f = outQueue_.front();
        size_t len = f.readableBytes();
        if (written >= len) {
            written -= len;
            outQueueBytes_ -= len;
            outQueue_.pop_front();
        } else {
            f.retrieve(written);
            outQueueBytes_ -= written;
            written = 0;
        }
    }
    if (!outQueue_.empty()) {
        // 剩余合并进 output_，靠写事件排空
        while (!outQueue_.empty()) {
            output_.append(outQueue_.front().peek(), outQueue_.front().readableBytes());
            outQueue_.pop_front();
        }
        outQueueBytes_ = 0;
        if (!writing_) {
            writing_ = true;
            ch_->enableWriting();
        }
    } else {
        outQueueBytes_ = 0;
    }
}

} // namespace netlib
