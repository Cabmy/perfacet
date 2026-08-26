#include "perfacet/health/RetryPolicy.h"
#include "detail/Time.h"

#include <algorithm>

namespace perfacet {

RetryPolicy::RetryPolicy(const YamlConfig& cfg)
    : maxAttempts_(cfg.retryMaxAttempts), retryable_(cfg.retryable), never_(cfg.neverRetry) {}

bool RetryPolicy::shouldRetry(ir::FailureClass klass, const ir::ToolKey& key,
                              const ir::BackendMeta& meta, int attempt, uint64_t deadlineMs,
                              bool circuitOpen, bool inflightAttemptOutstanding) const {
    if (attempt >= maxAttempts_) return false;
    if (circuitOpen) return false;
    if (deadlineMs > 0 && nowMs() >= deadlineMs) return false;
    for (auto n : never_) {
        if (n == klass) return false;
    }
    bool listed = false;
    for (auto r : retryable_) {
        if (r == klass) {
            listed = true;
            break;
        }
    }
    if (!listed) return false;
    if (klass == ir::FailureClass::Timeout) {
        if (inflightAttemptOutstanding) return false;
        const auto& idemp = meta.idempotentTools;
        if (std::find(idemp.begin(), idemp.end(), key.tool) == idemp.end()) return false;
    }
    return true;
}

} // namespace perfacet
