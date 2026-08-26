#pragma once
// agent::RetryPolicy —— 固定次数重试插件（libagent_policies）。
// 可重试状态默认 {CONN_CLOSED, TIMEOUT, BUSY}（瞬态故障）；
// CANCELLED / NO_METHOD / HANDLER_EXCEPTION 永不重试（结果确定，重打无意义）。
#include <vector>

#include "agent/IRetry.h"

namespace agent {

class RetryPolicy : public IRetry {
public:
    explicit RetryPolicy(
        int maxAttempts,
        std::vector<twigrpc::Status> retryable = {twigrpc::Status::CONN_CLOSED,
                                                  twigrpc::Status::TIMEOUT,
                                                  twigrpc::Status::BUSY});

    bool shouldRetry(const TaskContext& ctx, twigrpc::Status st, int attempt) override;

private:
    int maxAttempts_;
    std::vector<twigrpc::Status> retryable_;
};

} // namespace agent
