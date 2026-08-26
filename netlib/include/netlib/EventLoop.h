#pragma once
// netlib::EventLoop —— 单线程事件循环 + 跨线程唤醒(eventfd) + 定时器(timerfd)。
// 线程安全契约：
// 1. epoll 资源只允许所属 IO 线程操作；其他线程一律 runInLoop/queueInLoop 投递；
// 2. runInLoop 保证回调在本 loop 线程执行。
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "netlib/Epoll.h"

namespace netlib {

class Channel;

// 定时器值对象
struct Timer {
    std::chrono::steady_clock::time_point when;
    double interval = 0;            // >0 表示周期任务
    uint64_t id = 0;
    std::function<void()> cb;
};

using TimerId = uint64_t;

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();          // 阻塞运行，直到 quit()
    void quit();

    // 跨线程安全
    void runInLoop(std::function<void()> cb);
    void queueInLoop(std::function<void()> cb);

    // 定时器（在 loop 线程或投递后调用）
    TimerId runAfter(double sec, std::function<void()> cb);
    TimerId runEvery(double sec, std::function<void()> cb);
    void cancel(TimerId id);

    // 供 Channel 调用（须在本 loop 线程）
    void updateChannel(Channel* ch);
    void removeChannel(Channel* ch);

    bool inLoopThread() const;
    void assertInLoopThread() const;

    // 帧聚合刷写——连接以 owner 弱引用注册回调，loop 每圈末尾统一 writev；
    // owner 析构后条目在 flushAll 中自动回收（不会随连接历史无限增长）。
    // 回调执行期间 owner 被 flushAll 持活，可安全使用裸 this。
    void registerFlush(const std::weak_ptr<void>& owner, std::function<void()> fn);

    // poll 超时计算：距最近到期定时器的毫秒数，无定时器则 -1
    int nextTimeoutMs() const;

private:
    void doPending();
    void flushAll();
    void handleWakeup();
    void handleTimerExpired();
    void processExpiredTimers();
    void resetTimerfd();
    void insertTimer(Timer t);
    void cancelInLoop(TimerId id);

    Epoll ep_;
    int wakeupFd_ = -1;
    std::unique_ptr<Channel> wakeupCh_;

    int timerFd_ = -1;
    std::unique_ptr<Channel> timerCh_;
    struct TimerCmp {
        bool operator()(const Timer& a, const Timer& b) const { return a.when > b.when; }
    };
    std::priority_queue<Timer, std::vector<Timer>, TimerCmp> timers_;
    std::unordered_set<uint64_t> canceled_;
    std::unordered_map<uint64_t, std::function<void()>> timerCbs_;
    std::atomic<uint64_t> nextTimerId_{1};

    std::vector<std::function<void()>> pending_;
    // 帧聚合刷写回调（IO 线程注册/调用；owner 过期的条目在 flushAll 回收）
    struct FlushEntry {
        std::weak_ptr<void> owner;
        std::function<void()> fn;
    };
    std::vector<FlushEntry> flushCbs_;
    mutable std::mutex mtx_;
    std::atomic<bool> quit_{false};
    std::thread::id tid_;      // 受 mtx_ 保护：loop() 启动时切换到 loop 线程
    std::atomic<bool> looping_{false}; // loop() 是否已进入（防止启动前内联执行）
    bool callingPending_ = false;
};

} // namespace netlib
