#include "netlib/EventLoop.h"
#include "netlib/Channel.h"

#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace netlib {

EventLoop::EventLoop() : tid_(std::this_thread::get_id()) {
    wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd_ < 0) {
        throw std::runtime_error("EventLoop: eventfd failed");
    }
    wakeupCh_ = std::make_unique<Channel>(this, wakeupFd_);
    wakeupCh_->setReadCb([this]() { handleWakeup(); });
    wakeupCh_->enableReadingLT();

    timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerFd_ < 0) {
        throw std::runtime_error("EventLoop: timerfd_create failed");
    }
    timerCh_ = std::make_unique<Channel>(this, timerFd_);
    timerCh_->setReadCb([this]() { handleTimerExpired(); });
    timerCh_->enableReadingLT();
}

EventLoop::~EventLoop() {
    wakeupCh_->disableAll();
    wakeupCh_->remove();
    timerCh_->disableAll();
    timerCh_->remove();
    ::close(wakeupFd_);
    ::close(timerFd_);
}

void EventLoop::loop() {
    {
        // loop 线程可能不是构造线程：切换 tid_（受 mtx_ 保护，与 inLoopThread 互斥）
        std::lock_guard<std::mutex> lk(mtx_);
        tid_ = std::this_thread::get_id();
    }
    looping_.store(true, std::memory_order_release);
    while (!quit_.load(std::memory_order_acquire)) {
        if (nextTimeoutMs() == 0) processExpiredTimers();
        std::vector<Channel*> active = ep_.poll(nextTimeoutMs());
        for (Channel* ch : active) {
            ch->handleEvent();
        }
        doPending();
        flushAll(); // 每圈末尾统一刷写连接的暂存帧队列
    }
}

void EventLoop::flushAll() {
    // 先整批换出再逐条执行：owner 存活的执行并保留，已析构的直接回收；
    // 执行期间新注册的条目落在换入的新容器里，无迭代器失效问题。
    // owner 的 shared_ptr 在 fn 执行期间被持有：回调内可安全使用裸 this。
    std::vector<FlushEntry> cbs;
    cbs.swap(flushCbs_);
    for (auto& e : cbs) {
        if (e.owner.lock()) {
            e.fn();
            flushCbs_.push_back(std::move(e));
        }
    }
}

void EventLoop::registerFlush(const std::weak_ptr<void>& owner, std::function<void()> fn) {
    flushCbs_.push_back(FlushEntry{owner, std::move(fn)});
}

void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    // 唤醒（可能从其他线程调用）
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::runInLoop(std::function<void()> cb) {
    // 关键：loop() 未启动时（looping_==false）不内联执行——否则回调会在
    // 调用线程上跑，与即将启动的 loop 线程竞争 epoll 资源（TSan 可测出）
    if (inLoopThread() && looping_.load(std::memory_order_acquire)) {
        cb();
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.push_back(std::move(cb));
    }
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::doPending() {
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fns.swap(pending_);
    }
    callingPending_ = true;
    for (auto& fn : fns) {
        if (fn) fn();
    }
    callingPending_ = false;
}

void EventLoop::handleWakeup() {
    uint64_t one = 0;
    ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    (void)n;
}

void EventLoop::updateChannel(Channel* ch) {
    ep_.updateChannel(ch);
}

void EventLoop::removeChannel(Channel* ch) {
    ep_.removeChannel(ch);
}

bool EventLoop::inLoopThread() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return tid_ == std::this_thread::get_id();
}

void EventLoop::assertInLoopThread() const {
    if (!inLoopThread()) {
        std::fprintf(stderr, "EventLoop: fatal, not in loop thread\n");
        std::abort();
    }
}

// ---------------- 定时器 ----------------
// 线程契约：timers_/canceled_/timerfd 只在 loop 线程操作；
// 其他线程调 runAfter/runEvery/cancel 时经 queueInLoop 投递（TSan 全绿的前提）。

TimerId EventLoop::runAfter(double sec, std::function<void()> cb) {
    Timer t;
    t.when = std::chrono::steady_clock::now() +
             std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                 std::chrono::duration<double>(sec));
    t.interval = 0;
    t.id = nextTimerId_.fetch_add(1);
    t.cb = std::move(cb);
    TimerId id = t.id;
    if (inLoopThread() && looping_.load(std::memory_order_acquire)) {
        insertTimer(std::move(t));
    } else {
        queueInLoop([this, t = std::move(t)]() mutable { insertTimer(std::move(t)); });
    }
    return id;
}

TimerId EventLoop::runEvery(double sec, std::function<void()> cb) {
    Timer t;
    t.when = std::chrono::steady_clock::now() +
             std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                 std::chrono::duration<double>(sec));
    t.interval = sec;
    t.id = nextTimerId_.fetch_add(1);
    t.cb = std::move(cb);
    TimerId id = t.id;
    if (inLoopThread() && looping_.load(std::memory_order_acquire)) {
        insertTimer(std::move(t));
    } else {
        queueInLoop([this, t = std::move(t)]() mutable { insertTimer(std::move(t)); });
    }
    return id;
}

void EventLoop::cancel(TimerId id) {
    if (inLoopThread() && looping_.load(std::memory_order_acquire)) {
        cancelInLoop(id);
    } else {
        queueInLoop([this, id]() { cancelInLoop(id); });
    }
}

void EventLoop::cancelInLoop(TimerId id) {
    canceled_.insert(id);
    timerCbs_.erase(id); // 立刻丢掉回调，避免 shared_ptr 拖到到期
}

void EventLoop::insertTimer(Timer t) {
    timerCbs_[t.id] = std::move(t.cb);
    t.cb = nullptr;
    timers_.push(std::move(t));
    resetTimerfd();
}

int EventLoop::nextTimeoutMs() const {
    if (timers_.empty()) return -1;
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  timers_.top().when - now)
                  .count();
    if (ms < 0) ms = 0;
    return static_cast<int>(ms);
}

void EventLoop::handleTimerExpired() {
    uint64_t expirations = 0;
    ssize_t n = ::read(timerFd_, &expirations, sizeof(expirations));
    (void)n;
    processExpiredTimers();
}

void EventLoop::processExpiredTimers() {
    auto now = std::chrono::steady_clock::now();
    std::vector<Timer> expired;
    while (!timers_.empty() && timers_.top().when <= now) {
        expired.push_back(timers_.top());
        timers_.pop();
    }
    for (Timer& t : expired) {
        if (canceled_.count(t.id)) {
            canceled_.erase(t.id);
            timerCbs_.erase(t.id);
            continue;
        }
        std::function<void()> cb;
        auto it = timerCbs_.find(t.id);
        if (it != timerCbs_.end()) {
            cb = std::move(it->second);
            timerCbs_.erase(it);
        }
        if (cb) cb();
        if (t.interval > 0 && !canceled_.count(t.id)) {
            t.when = now + std::chrono::duration_cast<
                              std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(t.interval));
            timerCbs_[t.id] = cb;
            timers_.push(t);
        }
    }
    resetTimerfd();
}

void EventLoop::resetTimerfd() {
    itimerspec its{};
    if (!timers_.empty()) {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      timers_.top().when - now)
                      .count();
        if (ns < 1) ns = 1; // 0/0 会解除 timerfd，堆里过期项就再也收不回来
        its.it_value.tv_sec = static_cast<time_t>(ns / 1000000000LL);
        its.it_value.tv_nsec = static_cast<long>(ns % 1000000000LL);
    }
    ::timerfd_settime(timerFd_, 0, &its, nullptr);
}

} // namespace netlib
