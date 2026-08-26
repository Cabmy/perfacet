#pragma once
// netlib::TcpClient —— 客户端侧单连接（重连由 rpc 层驱动）。
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "netlib/Channel.h"
#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "netlib/Socket.h"
#include "netlib/TcpConnection.h"

namespace netlib {

class TcpClient {
public:
    // loop 由外部拥有并运行（RpcConn/RpcClient 提供 IO 线程）
    TcpClient(EventLoop* loop);
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    // 发起非阻塞连接（TCP 或 UDS Endpoint）；成功后 connCb 在 loop 线程回调
    void connect(const Endpoint& serverAddr);
    void disconnect();

    void setConnCb(TcpConnection::ConnCb cb) { connCb_ = std::move(cb); }
    void setMsgCb(TcpConnection::MsgCb cb) { msgCb_ = std::move(cb); }
    void setCloseCb(TcpConnection::CloseCb cb) { closeCb_ = std::move(cb); }

    TcpConnectionPtr connection() const;
    bool connected() const;

private:
    void connecting(const Endpoint& addr);
    void handleWritable();     // connect 完成判定
    void retry(const Endpoint& addr);
    void newConnection(int fd);

    EventLoop* loop_;
    std::unique_ptr<Socket> sock_;
    std::unique_ptr<Channel> ch_;
    mutable std::mutex mtx_;
    TcpConnectionPtr conn_;
    Endpoint serverAddr_{};
    bool connectStarted_ = false;
    std::atomic<bool> retryEnabled_{true};

    TcpConnection::ConnCb connCb_;
    TcpConnection::MsgCb msgCb_;
    TcpConnection::CloseCb closeCb_;
};

} // namespace netlib
