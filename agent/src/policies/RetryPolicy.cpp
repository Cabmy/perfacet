#include "agent/policies/RetryPolicy.h"

#include <algorithm>
#include <utility>

namespace agent {

RetryPolicy::RetryPolicy(int maxAttempts, std::vector<twigrpc::Status> retryable)
    : maxAttempts_(maxAttempts), retryable_(std::move(retryable)) {}

bool RetryPolicy::shouldRetry(const TaskContext&, twigrpc::Status st, int attempt) {
    // attempt = 已失败次数；打满 maxAttempts_ 次即放弃
    return attempt < maxAttempts_ &&
           std::find(retryable_.begin(), retryable_.end(), st) != retryable_.end();
}

} // namespace agent
