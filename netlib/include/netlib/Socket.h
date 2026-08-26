#pragma once
// netlib::Socket —— fd 的 RAII 封装与 socket 选项。
// 按 Endpoint 族创建 AF_INET / AF_UNIX 字节流 socket；UDS bind 前 unlink 旧路径文件。
// 只依赖 POSIX；不知道 epoll/EventLoop 的存在。
#include <cstdint>
#include <memory>

#include "netlib/Endpoint.h"

namespace netlib {

class Socket {
public:
    // 创建非阻塞字节流 socket（默认 TCP），失败抛 std::runtime_error
    explicit Socket(Family family = Family::Tcp);
    // 接管已有 fd（accept/connect 产物；UDS fd 须显式传族）
    Socket(int fd, Family family = Family::Tcp);
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    void bind(const Endpoint& addr);
    void listen(int backlog);
    // accept4 返回非阻塞 + CLOEXEC 的新连接；无新连接时返回 nullptr。
    // EINTR 在内部重试。失败时若 errOut 非空则写入 errno，供上层区分
    // EAGAIN（队列耗尽）与 EMFILE/ENFILE（fd 耗尽）。
    // peer 回填对端 Endpoint（UDS 对端未绑定路径时为空路径）
    std::unique_ptr<Socket> accept(Endpoint& peerAddr, int* errOut = nullptr);
    // 非阻塞 connect：0 成功；-1 且 errno==EINPROGRESS 表示进行中
    int connect(const Endpoint& addr);

    void setNoDelay(); // 仅 TCP 有效，UDS 上为空操作
    void setReuseAddr();
    void setReusePort();
    void setKeepAlive();

    Family family() const { return family_; }
    int getFd() const { return fd_; }
    int releaseFd() {
        int f = fd_;
        fd_ = -1;
        return f;
    }
    void shutdownWrite();

private:
    int fd_ = -1;
    Family family_ = Family::Tcp;
};

} // namespace netlib
