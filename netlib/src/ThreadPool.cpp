#include "netlib/ThreadPool.h"

namespace netlib {

ThreadPool::ThreadPool(size_t numThreads, size_t maxQueue) : maxQueue_(maxQueue == 0 ? 1 : maxQueue) {
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this]() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lk(mtx_);
                    cv_.wait(lk, [this]() { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

} // namespace netlib
