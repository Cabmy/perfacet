#pragma once
// Timeout 必须命中 idempotent_tools；在途未完成禁止当可重试 Timeout。
#include "perfacet/ir/Request.h"
#include "perfacet/policy/YamlConfig.h"

namespace perfacet {

class RetryPolicy {
public:
    explicit RetryPolicy(const YamlConfig& cfg);

    bool shouldRetry(ir::FailureClass klass, const ir::ToolKey& key,
                     const ir::BackendMeta& meta, int attempt,
                     uint64_t deadlineMs, bool circuitOpen,
                     bool inflightAttemptOutstanding) const;

    int maxAttempts() const { return maxAttempts_; }

private:
    int maxAttempts_;
    std::vector<ir::FailureClass> retryable_;
    std::vector<ir::FailureClass> never_;
};

} // namespace perfacet
