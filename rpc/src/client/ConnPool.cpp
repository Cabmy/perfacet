#include "client/ConnPool.h"

namespace twigrpc {

ConnPool::ConnPool(netlib::Endpoint addr, size_t k) {
    for (size_t i = 0; i < k; ++i) {
        conns_.push_back(std::make_shared<RpcConn>(addr));
    }
}

std::shared_ptr<RpcConn> ConnPool::pick() {
    size_t idx = next_.fetch_add(1) % conns_.size();
    return conns_[idx];
}

bool ConnPool::allConnected() const {
    for (auto& c : conns_) {
        if (!c->connected()) return false;
    }
    return true;
}

void ConnPool::waitConnected(int timeoutMs) {
    for (auto& c : conns_) {
        c->waitConnected(timeoutMs);
    }
}

} // namespace twigrpc
