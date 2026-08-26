#include "netlib/Channel.h"

namespace netlib {

// 事件分发顺序固定，不许改：
// 1) EPOLLERR/EPOLLHUP 优先且立即返回——连接已坏，读写字节无意义；
// 2) EPOLLRDHUP/EPOLLIN——对端关闭或数据到达；
// 3) EPOLLOUT——可写。
// 注意：EPOLLRDHUP 必须在 events 中显式注册才会上报；
//       EPOLLERR/EPOLLHUP 是 epoll 默认事件，无需注册。
void Channel::handleEvent() {
    // 生命周期保护：tie 的 owner（TcpConnection）在本函数栈期间被持有，
    // 即使 closeCb 释放了最后的 shared_ptr，本函数后续也不会访问已析构对象
    std::shared_ptr<void> guard;
    if (tied_) {
        guard = tie_.lock();
        if (!guard) return; // owner 已析构：丢弃事件
    }
    if (revents_ & (EPOLLERR | EPOLLHUP)) {
        if (errorCb_) errorCb_();
        return;
    }
    if (revents_ & (EPOLLRDHUP | EPOLLIN)) {
        if (readCb_) readCb_();
    }
    if (revents_ & EPOLLOUT) {
        if (writeCb_) writeCb_();
    }
}

void Channel::enableReadingET() {
    events_ |= EPOLLIN | EPOLLET | EPOLLRDHUP;
    update();
}

void Channel::enableReadingLT() {
    events_ |= EPOLLIN | EPOLLRDHUP;
    update();
}

void Channel::enableWriting() {
    events_ |= EPOLLOUT;
    update();
}

void Channel::disableWriting() {
    events_ &= ~EPOLLOUT;
    update();
}

void Channel::disableAll() {
    events_ = 0;
    update();
}

void Channel::update() {
    loop_->updateChannel(this);
}

void Channel::remove() {
    loop_->removeChannel(this);
}

} // namespace netlib
