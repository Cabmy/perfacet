#pragma once
// twigrpc::RpcException —— 客户端调用异常（携带协议 status）。
#include <stdexcept>
#include <string>

#include "codec/Protocol.h"

namespace twigrpc {

class RpcException : public std::runtime_error {
public:
    RpcException(Status st, const std::string& detail)
        : std::runtime_error(std::string(statusName(st)) + ": " + detail), status_(st) {}
    Status status() const { return status_; }

private:
    Status status_;
};

} // namespace twigrpc
