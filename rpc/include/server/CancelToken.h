#pragma once
// twigrpc::CancelToken —— 服务端协作取消原语（叶子传输层，不知任务树）。
// CANCEL 帧到达时由 RpcServer 置位；慢 handler 轮询 cancelled() 提前返回。
// 不杀线程：不轮询的 handler 只保证「响应变成 CANCELLED、结果丢弃」。
// taskId/deadline 来自请求 TLV，rpc 不解释，仅供上层（CancelHook）取用。
#include <atomic>
#include <cstdint>

namespace twigrpc {

class CancelToken {
public:
    CancelToken() = default;
    CancelToken(uint64_t deadlineUnixMs, uint64_t taskId)
        : deadlineUnixMs_(deadlineUnixMs), taskId_(taskId) {}

    void cancel() { cancelled_.store(true, std::memory_order_release); }
    bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }

    uint64_t deadlineUnixMs() const { return deadlineUnixMs_; } // 0 = 无期限
    uint64_t taskId() const { return taskId_; }                // 0 = 非 Agent 调用

private:
    std::atomic<bool> cancelled_{false};
    uint64_t deadlineUnixMs_ = 0;
    uint64_t taskId_ = 0;
};

} // namespace twigrpc
