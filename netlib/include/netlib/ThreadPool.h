#pragma once
// netlib::ThreadPool —— 通用任务池（C++17）。
// add 返回 future；禁止 std::bind，lambda 完美转发。
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

namespace netlib {

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads = 4, size_t maxQueue = 1024);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F, typename... Args>
    auto add(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            [f = std::forward<F>(f),
             tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
                return std::apply(f, std::move(tup));
            });
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stop_) {
                throw std::runtime_error("ThreadPool::add on stopped pool");
            }
            if (tasks_.size() >= maxQueue_) {
                throw std::runtime_error("ThreadPool queue full");
            }
            tasks_.emplace_back([task]() { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    size_t size() const { return workers_.size(); }
    bool full() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return tasks_.size() >= maxQueue_;
    }

private:
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    size_t maxQueue_ = 1024;
};

} // namespace netlib
