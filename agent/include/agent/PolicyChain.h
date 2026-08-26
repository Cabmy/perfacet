#pragma once
// agent::PolicyChain —— 有序策略链：短路 Reject，结束记账扇出。
#include <memory>
#include <vector>

#include "agent/IPolicy.h"

namespace agent {

class PolicyChain {
public:
    void add(std::unique_ptr<IPolicy> p) { chain_.push_back(std::move(p)); }

    Decision evaluate(const TaskContext& ctx, const RpcRequest& req) {
        for (size_t i = 0; i < chain_.size(); ++i) {
            Decision d = chain_[i]->evaluate(ctx, req);
            if (!d.admit) {
                // 短路 Reject：为前面已 Admit（可能占槽）的策略补记账，
                // 维持「每次 Admit 后必然恰有一次 onComplete」不变量。
                for (size_t j = 0; j < i; ++j) {
                    chain_[j]->onComplete(ctx, twigrpc::Status::REJECTED);
                }
                return d;
            }
        }
        return Decision::Admit();
    }

    void onComplete(const TaskContext& ctx, twigrpc::Status st) {
        for (auto& p : chain_) p->onComplete(ctx, st);
    }

private:
    std::vector<std::unique_ptr<IPolicy>> chain_;
};

} // namespace agent
