#include "client/TimeoutSweeper.h"

#include <cstdio>

#include "client/RpcException.h"

namespace twigrpc {

TimeoutSweeper::TimeoutSweeper(netlib::EventLoop* loop, PendingTable* table,
                               std::function<void(uint64_t)> sendCancel)
    : loop_(loop), table_(table), sendCancel_(std::move(sendCancel)) {}

TimeoutSweeper::~TimeoutSweeper() {
    stop();
}

void TimeoutSweeper::start() {
    if (running_) return;
    running_ = true;
    timerId_ = loop_->runEvery(config::kSweepIntervalSec, [this]() { sweep(); });
}

void TimeoutSweeper::stop() {
    if (!running_) return;
    running_ = false;
    loop_->cancel(timerId_);
}

void TimeoutSweeper::sweep() {
    auto now = std::chrono::steady_clock::now();
    for (uint64_t id : table_->expiredIds(now)) {
        // 原子摘除：摘到者才有权 set（迟到响应将被丢弃）
        if (auto pc = table_->take(id)) {
            // 先通知服务端停手，再失败本地 promise（CANCELLED 响应到达即被丢弃）
            if (sendCancel_) sendCancel_(id);
            pc->pr.set_exception(std::make_exception_ptr(
                RpcException(Status::TIMEOUT, "call timeout")));
        }
        // 摘不到：响应方已 set_value 或 failAll 已处理——不变量的另一半
    }
}

} // namespace twigrpc
