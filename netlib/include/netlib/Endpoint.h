#pragma once
// netlib::Endpoint —— 地址值对象，不可变，同时承载 TCP 回环与 UDS。
// netlib 层只依赖 POSIX 与标准库，禁止出现任何 RPC 的概念。
// TCP：ip + port（port=0 由内核分配）；UDS：文件系统路径（不做抽象名）。
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace netlib {

enum class Family { Tcp, Unix };

class Endpoint {
public:
    // 缺省：未知 TCP 地址（accept 回填前占位）
    Endpoint() = default;
    // TCP 端点（ip 合法性在 socket 系统调用时校验）
    Endpoint(std::string ip, uint16_t port);
    // UDS 端点（路径长度须 < sizeof(sockaddr_un::sun_path)，bind 时 unlink 旧文件）
    static Endpoint unixPath(std::string path);

    Family family() const { return family_; }
    const std::string& ip() const { return ip_; }
    uint16_t port() const { return port_; }
    const std::string& path() const { return path_; }

    // "tcp://ip:port" / "unix:///abs/path"
    std::string toString() const;

private:
    Family family_ = Family::Tcp;
    std::string ip_;
    uint16_t port_ = 0;
    std::string path_;
};

// 主机名 → IPv4 字符串（已是合法 IP 则原样返回；容器内 registry 为 DNS 名）
inline std::string resolveHost(const std::string& host) {
    struct in_addr a;
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) return host;
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        throw std::runtime_error("resolve failed: " + host);
    }
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
              buf, sizeof(buf));
    freeaddrinfo(res);
    return buf;
}

// 解析 "host:port"（host 可为 DNS 名），解析失败抛 runtime_error。
// 集中处理冒号缺失 / 端口非法，避免各入口各自 rfind 且漏校验。
inline Endpoint parseHostPort(const std::string& spec) {
    auto colon = spec.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == spec.size()) {
        throw std::runtime_error("bad address (expect host:port): " + spec);
    }
    const int p = std::atoi(spec.substr(colon + 1).c_str());
    if (p <= 0 || p > 65535) {
        throw std::runtime_error("bad port: " + spec);
    }
    return Endpoint(resolveHost(spec.substr(0, colon)),
                    static_cast<uint16_t>(p));
}

// 本机访问 target 时的出口 IP：UDP connect 探测（不发送任何包），
// 供服务自注册推导 advertise 地址；失败回退 127.0.0.1。
inline std::string localIpFor(const Endpoint& target) {
    if (target.family() != Family::Tcp) return "127.0.0.1";
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "127.0.0.1";
    sockaddr_in peer {};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(target.port());
    ::inet_pton(AF_INET, target.ip().c_str(), &peer.sin_addr);
    std::string out = "127.0.0.1";
    if (::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0) {
        sockaddr_in local {};
        socklen_t len = sizeof(local);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            char buf[INET_ADDRSTRLEN];
            if (::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) {
                out = buf;
            }
        }
    }
    ::close(fd);
    return out;
}

} // namespace netlib
