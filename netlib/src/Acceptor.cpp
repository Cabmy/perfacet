#include "netlib/Acceptor.h"
#include "netlib/Config.h"

#include <cerrno>
#include <cstdio>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace netlib {

Acceptor::Acceptor(EventLoop* loop, const Endpoint& listenAddr)
    : loop_(loop), listenAddr_(listenAddr),
      listenSock_(listenAddr.family()) {
    listenSock_.setReuseAddr();
    listenSock_.setReusePort();
    listenSock_.bind(listenAddr);
    listenSock_.listen(config::kBacklog);
    idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    ch_ = std::make_unique<Channel>(loop_, listenSock_.getFd());
    ch_->setReadCb([this]() { acceptConnection(); });
    ch_->enableReadingET();
}

Acceptor::~Acceptor() {
    if (idleFd_ >= 0) {
        ::close(idleFd_);
        idleFd_ = -1;
    }
}

uint16_t Acceptor::listenPort() const {
    if (listenAddr_.family() != Family::Tcp) return 0;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(listenSock_.getFd(), reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        return ntohs(addr.sin_port);
    }
    return 0;
}

void Acceptor::acceptConnection() {
    loop_->assertInLoopThread();
    // ET 模式：必须循环 accept 直到 EAGAIN，否则丢连接
    while (true) {
        Endpoint peer;
        int err = 0;
        std::unique_ptr<Socket> conn = listenSock_.accept(peer, &err);
        if (!conn) {
            if (err == EAGAIN || err == EWOULDBLOCK) break;
            // fd 耗尽：腾 idle 槽把就绪连接取出立刻丢掉，避免内核队列饿死
            if ((err == EMFILE || err == ENFILE) && idleFd_ >= 0) {
                ::close(idleFd_);
                idleFd_ = -1;
                Endpoint discarded;
                auto drop = listenSock_.accept(discarded);
                (void)drop; // ~Socket 关 fd
                idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                continue;
            }
            break;
        }
        if (newConnCb_) {
            newConnCb_(std::move(conn), peer);
        }
    }
}

} // namespace netlib
