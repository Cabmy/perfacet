#pragma once
// netlib::Acceptor —— 监听 + accept，产生新连接交给上层。
#include <functional>
#include <memory>

#include "netlib/Channel.h"
#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "netlib/Socket.h"

namespace netlib {

class Acceptor {
public:
    using NewConnCb = std::function<void(std::unique_ptr<Socket>, const Endpoint&)>;

    Acceptor(EventLoop* loop, const Endpoint& listenAddr);
    ~Acceptor();

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void setNewConnCb(NewConnCb cb) { newConnCb_ = std::move(cb); }

    // 停 accept：摘掉 listen 读事件。drain / GOAWAY 用。
    void pause();

    // 实际监听端口（TCP port=0 时由内核分配，bind 后可查；UDS 恒 0）
    uint16_t listenPort() const;

private:
    void acceptConnection(); // ET：循环 accept 到 EAGAIN

    EventLoop* loop_;
    Endpoint listenAddr_;
    Socket listenSock_;
    std::unique_ptr<Channel> ch_; // listen 就绪后构造（fd 在 bind/listen 后才有效）
    NewConnCb newConnCb_;
    int idleFd_ = -1; // EMFILE 时腾一个槽，把就绪连接 accept 出来立刻丢掉
};

} // namespace netlib
