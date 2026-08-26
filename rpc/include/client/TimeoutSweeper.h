#pragma once
// twigrpc::TimeoutSweeper —— 周期扫描 PendingTable 过期项。
// 到期：先对未摘除 id 发 CANCEL 帧（让服务端停手），再 take + set_exception(timeout)。
// deadline 是唯一时钟：绝对期限写在请求 TLV 里，本表只负责本地兜底失败。
#include <functional>
#include <memory>

#include "netlib/EventLoop.h"
#include "client/PendingTable.h"

namespace config {
inline constexpr double kSweepIntervalSec = 0.2; // 扫描周期
} // namespace config

namespace twigrpc {

class TimeoutSweeper {
public:
    // sendCancel：到期项的 CANCEL 发送入口（RpcConn::cancel），可为空
    TimeoutSweeper(netlib::EventLoop* loop, PendingTable* table,
                   std::function<void(uint64_t)> sendCancel = nullptr);
    ~TimeoutSweeper();

    void start();
    void stop();

private:
    void sweep();

    netlib::EventLoop* loop_;
    PendingTable* table_;
    std::function<void(uint64_t)> sendCancel_;
    netlib::TimerId timerId_ = 0;
    bool running_ = false;
};

} // namespace twigrpc
