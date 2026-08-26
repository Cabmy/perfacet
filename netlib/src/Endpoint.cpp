#include "netlib/Endpoint.h"

#include <sys/un.h>

#include <stdexcept>

namespace netlib {

Endpoint::Endpoint(std::string ip, uint16_t port)
    : family_(Family::Tcp), ip_(std::move(ip)), port_(port) {}

Endpoint Endpoint::unixPath(std::string path) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::runtime_error("Endpoint: unix path too long: " + path);
    }
    Endpoint e;
    e.family_ = Family::Unix;
    e.path_ = std::move(path);
    return e;
}

std::string Endpoint::toString() const {
    if (family_ == Family::Unix) {
        return "unix://" + path_;
    }
    return "tcp://" + ip_ + ":" + std::to_string(port_);
}

} // namespace netlib
