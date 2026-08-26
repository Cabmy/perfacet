#include "netlib/TcpClient.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <sys/socket.h>

namespace netlib {

TcpClient::TcpClient(EventLoop* loop) : loop_(loop) {}

TcpClient::~TcpClient() {
    disconnect();
}

void TcpClient::connect(const Endpoint& serverAddr) {
    retryEnabled_ = true; // 重新使能重试（disconnect 会关闭它）
    loop_->runInLoop([this, serverAddr]() {
        connectStarted_ = true;
        serverAddr_ = serverAddr;
        connecting(serverAddr);
    });
}

void TcpClient::connecting(const Endpoint& addr) {
    loop_->assertInLoopThread();
    // disconnect 后已排定的 retry 定时器到期会走到这里：直接放弃，不再重连
    if (!retryEnabled_.load()) return;
    sock_ = std::make_unique<Socket>(addr.family());
    int r = sock_->connect(addr);
    if (r == 0) {
        // 本机直连可能立即成功
        newConnection(sock_->releaseFd());
        return;
    }
    if (errno == EINPROGRESS) {
        // 等可写后判定
        ch_ = std::make_unique<Channel>(loop_, sock_->getFd());
        ch_->setWriteCb([this]() { handleWritable(); });
        ch_->setErrorCb([this]() { handleWritable(); });
        ch_->enableWriting();
        return;
    }
    std::fprintf(stderr, "[TcpClient] connect errno=%d, retry\n", errno);
    retry(addr);
}

void TcpClient::handleWritable() {
    loop_->assertInLoopThread();
    // disconnect 后半连接仍可能变可写：必须先看 retryEnabled_，
    // 否则 EINPROGRESS 期间 disconnect 仍会走 newConnection。
    if (!retryEnabled_.load()) {
        if (ch_) {
            ch_->disableAll();
            ch_->remove();
            ch_.reset();
        }
        sock_.reset();
        return;
    }
    // connect 结果判定
    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(sock_->getFd(), SOL_SOCKET, SO_ERROR, &err, &len);
    ch_->disableAll();
    ch_->remove();
    ch_.reset();
    if (err == 0) {
        newConnection(sock_->releaseFd());
    } else {
        std::fprintf(stderr, "[TcpClient] connect failed SO_ERROR=%d\n", err);
        sock_.reset();
        retry(serverAddr_);
    }
}

void TcpClient::retry(const Endpoint& addr) {
    if (!retryEnabled_.load()) {
        return;
    }
    loop_->runAfter(0.2, [this, addr]() { connecting(addr); });
}

void TcpClient::newConnection(int fd) {
    loop_->assertInLoopThread();
    auto sock = std::make_unique<Socket>(fd, serverAddr_.family());
    auto conn = std::make_shared<TcpConnection>(loop_, std::move(sock),
                                                Endpoint(), serverAddr_);
    conn->setConnCb(connCb_);
    conn->setMsgCb(msgCb_);
    conn->setCloseCb([this](const TcpConnectionPtr& c) {
        if (closeCb_) closeCb_(c);
        std::lock_guard<std::mutex> lk(mtx_);
        if (conn_ == c) conn_.reset();
    });
    {
        std::lock_guard<std::mutex> lk(mtx_);
        conn_ = conn;
    }
    conn->attachToLoop();
}

TcpConnectionPtr TcpClient::connection() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return conn_;
}

bool TcpClient::connected() const {
    auto c = connection();
    return c && c->connected();
}

void TcpClient::disconnect() {
    retryEnabled_ = false;
    loop_->runInLoop([this]() {
        auto c = connection();
        if (c) c->forceClose();
    });
}

} // namespace netlib
