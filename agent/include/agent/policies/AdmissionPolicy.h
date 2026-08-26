#pragma once
// agent::AdmissionPolicy —— 最大并发准入插件（libagent_policies，不进 libagent）。
// evaluate 占槽（inFlight+1，超限回滚并 Reject 快速失败）；onComplete 释放。
// 槽是进程级全局配额；按任务/方法细分请自行扩展，框架不内置。
#include <atomic>

#include "agent/IPolicy.h"

namespace agent {

class AdmissionPolicy : public IPolicy {
public:
    explicit AdmissionPolicy(int maxConcurrent) : max_(maxConcurrent) {}

    Decision evaluate(const TaskContext& ctx, const RpcRequest& req) override;
    void onComplete(const TaskContext& ctx, twigrpc::Status st) override;

    int inFlight() const { return inFlight_.load(); }
    int max() const { return max_; }

private:
    int max_;
    std::atomic<int> inFlight_{0};
};

} // namespace agent
