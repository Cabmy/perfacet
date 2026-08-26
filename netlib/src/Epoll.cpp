#include "netlib/Epoll.h"
#include "netlib/Channel.h"
#include "netlib/Config.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <stdexcept>

namespace netlib {

Epoll::Epoll() : events_(config::kEpollInitEvents) {
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        throw std::runtime_error("Epoll: epoll_create1 failed");
    }
}

Epoll::~Epoll() {
    if (epfd_ >= 0) {
        ::close(epfd_);
        epfd_ = -1;
    }
}

void Epoll::updateChannel(Channel* ch) {
    epoll_event ev{};
    ev.data.ptr = ch;
    ev.events = ch->getEvents();
    if (!ch->getInEpoll()) {
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, ch->getFd(), &ev) < 0) {
            std::perror("Epoll::updateChannel add");
            return;
        }
        ch->setInEpoll();
    } else {
        if (::epoll_ctl(epfd_, EPOLL_CTL_MOD, ch->getFd(), &ev) < 0) {
            std::perror("Epoll::updateChannel mod");
        }
    }
}

void Epoll::removeChannel(Channel* ch) {
    if (ch->getInEpoll()) {
        if (::epoll_ctl(epfd_, EPOLL_CTL_DEL, ch->getFd(), nullptr) < 0) {
            std::perror("Epoll::removeChannel");
        }
        ch->setInEpoll(false);
    }
}

std::vector<Channel*> Epoll::poll(int timeoutMs) {
    int n = ::epoll_wait(epfd_, events_.data(),
                         static_cast<int>(events_.size()), timeoutMs);
    while (n < 0 && errno == EINTR) {
        n = ::epoll_wait(epfd_, events_.data(),
                         static_cast<int>(events_.size()), timeoutMs);
    }
    if (n < 0) {
        std::perror("Epoll::poll");
        return {};
    }
    if (static_cast<size_t>(n) == events_.size()) {
        events_.resize(events_.size() * 2); // 不够用时翻倍
    }
    std::vector<Channel*> active;
    active.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto* ch = static_cast<Channel*>(events_[i].data.ptr);
        ch->setRevents(events_[i].events);
        active.push_back(ch);
    }
    return active;
}

} // namespace netlib
