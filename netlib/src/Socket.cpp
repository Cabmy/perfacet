#include "netlib/Socket.h"
#include "netlib/Config.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace netlib {

Socket::Socket(Family family) : family_(family) {
    int af = (family == Family::Unix) ? AF_UNIX : AF_INET;
    fd_ = ::socket(af, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        throw std::runtime_error("Socket: socket() failed");
    }
}

Socket::Socket(int fd, Family family) : fd_(fd), family_(family) {}

Socket::~Socket() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_), family_(other.family_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        family_ = other.family_;
        other.fd_ = -1;
    }
    return *this;
}

// TCP：Endpoint → sockaddr_in（ip 非法在此抛出）
static sockaddr_in toSockaddrIn(const Endpoint& addr) {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(addr.port());
    if (::inet_pton(AF_INET, addr.ip().c_str(), &sin.sin_addr) != 1) {
        throw std::runtime_error("Socket: invalid ipv4 address: " + addr.ip());
    }
    return sin;
}

void Socket::bind(const Endpoint& addr) {
    if (addr.family() == Family::Unix) {
        // 残留 socket 文件会让 bind 失败：先 unlink（不存在时忽略）
        ::unlink(addr.path().c_str());
        sockaddr_un un{};
        un.sun_family = AF_UNIX;
        std::strncpy(un.sun_path, addr.path().c_str(), sizeof(un.sun_path) - 1);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&un), sizeof(un)) < 0) {
            std::perror("Socket::bind(unix)");
            throw std::runtime_error("Socket: bind() unix failed");
        }
        return;
    }
    sockaddr_in sin = toSockaddrIn(addr);
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin)) < 0) {
        std::perror("Socket::bind");
        throw std::runtime_error("Socket: bind() failed");
    }
}

void Socket::listen(int backlog) {
    if (::listen(fd_, backlog) < 0) {
        std::perror("Socket::listen");
        throw std::runtime_error("Socket: listen() failed");
    }
}

std::unique_ptr<Socket> Socket::accept(Endpoint& peerAddr, int* errOut) {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    int connfd = -1;
    do {
        connfd = ::accept4(fd_, reinterpret_cast<sockaddr*>(&ss), &len,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (connfd < 0 && errno == EINTR);
    if (connfd < 0) {
        if (errOut) *errOut = errno;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::perror("Socket::accept");
        }
        return nullptr;
    }
    if (errOut) *errOut = 0;
    Family fam = (ss.ss_family == AF_INET) ? Family::Tcp : Family::Unix;
    auto conn = std::make_unique<Socket>(connfd, fam);
    if (fam == Family::Tcp) {
        auto* sin = reinterpret_cast<const sockaddr_in*>(&ss);
        char buf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        peerAddr = Endpoint(buf, ntohs(sin->sin_port));
    } else {
        // UDS：对端未绑定路径时 sun_path 为空（无名 socket）
        auto* un = reinterpret_cast<const sockaddr_un*>(&ss);
        if (len > offsetof(sockaddr_un, sun_path) && un->sun_path[0] != '\0') {
            peerAddr = Endpoint::unixPath(un->sun_path);
        } else {
            peerAddr = Endpoint::unixPath("");
        }
    }
    return conn;
}

int Socket::connect(const Endpoint& addr) {
    if (addr.family() == Family::Unix) {
        sockaddr_un un{};
        un.sun_family = AF_UNIX;
        std::strncpy(un.sun_path, addr.path().c_str(), sizeof(un.sun_path) - 1);
        return ::connect(fd_, reinterpret_cast<const sockaddr*>(&un), sizeof(un));
    }
    sockaddr_in sin = toSockaddrIn(addr);
    return ::connect(fd_, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));
}

void Socket::setNoDelay() {
    if (family_ != Family::Tcp) return; // UDS 无 Nagle，空操作
    int on = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

void Socket::setReuseAddr() {
    int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
}

void Socket::setReusePort() {
    int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
}

void Socket::setKeepAlive() {
    int on = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
}

void Socket::shutdownWrite() {
    ::shutdown(fd_, SHUT_WR);
}

} // namespace netlib
