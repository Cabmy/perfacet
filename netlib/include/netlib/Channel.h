#pragma once
// netlib::Channel —— fd 与事件回调的绑定，每类事件独立回调。
// 不拥有 fd（生命周期归 Socket/TcpConnection）。
#include <cstdint>
#include <functional>
#include <memory>

#include "netlib/EventLoop.h"

namespace netlib {

class Channel {
public:
    using EventCallback = std::function<void()>;

    Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

    // 绑定 owner 生命周期：handleEvent 期间持有 owner 的 shared_ptr，
    // 防止回调（closeCb）释放连接后事件分发继续访问已析构对象
    void tie(const std::shared_ptr<void>& owner) {
        tie_ = owner;
        tied_ = true;
    }

    void handleEvent();

    void enableReadingET();   // EPOLLIN | EPOLLET | EPOLLRDHUP
    void enableReadingLT();
    void enableWriting();
    void disableWriting();
    void disableAll();

    void setReadCb(EventCallback cb) { readCb_ = std::move(cb); }
    void setWriteCb(EventCallback cb) { writeCb_ = std::move(cb); }
    void setCloseCb(EventCallback cb) { closeCb_ = std::move(cb); }
    void setErrorCb(EventCallback cb) { errorCb_ = std::move(cb); }

    int getFd() const { return fd_; }
    uint32_t getEvents() const { return events_; }
    void setRevents(uint32_t r) { revents_ = r; }
    bool getInEpoll() const { return inEpoll_; }
    void setInEpoll(bool in = true) { inEpoll_ = in; }

    EventLoop* ownerLoop() const { return loop_; }
    void remove(); // 从 epoll 摘除自己（由 owner loop 执行）

private:
    void update();

    EventLoop* loop_;
    int fd_;
    uint32_t events_ = 0;
    uint32_t revents_ = 0;
    bool inEpoll_ = false;

    EventCallback readCb_;
    EventCallback writeCb_;
    EventCallback closeCb_;
    EventCallback errorCb_;

    std::weak_ptr<void> tie_;
    bool tied_ = false;
};

} // namespace netlib
