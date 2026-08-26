#pragma once
// twigrpc::ConnPool —— 连接池：round-robin 取用。
// 持 shared_ptr：RpcClient 在途表需跨线程共享连接所有权。
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "netlib/Endpoint.h"
#include "client/RpcConn.h"

namespace twigrpc {

class ConnPool {
public:
    // 同一 Endpoint 建 k 条连接
    ConnPool(netlib::Endpoint addr, size_t k = 1);

    // round-robin 取连接
    std::shared_ptr<RpcConn> pick();

    size_t size() const { return conns_.size(); }
    bool allConnected() const;

    void waitConnected(int timeoutMs = 2000);

private:
    // 连接表建池后只读；游标原子：pick 可被多个调用线程并发进入
    std::vector<std::shared_ptr<RpcConn>> conns_;
    std::atomic<size_t> next_{0};
};

} // namespace twigrpc
