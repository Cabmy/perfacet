#include "perfacet/health/RetryPolicy.h"
#include "perfacet/ir/Request.h"
#include "detail/Time.h"

#include <doctest/doctest.h>

using perfacet::RetryPolicy;
using perfacet::YamlConfig;
using perfacet::ir::BackendMeta;
using perfacet::ir::FailureClass;
using perfacet::ir::ToolKey;

namespace {

RetryPolicy makePolicy(int maxAttempts = 2) {
    YamlConfig cfg;
    cfg.retryMaxAttempts = maxAttempts;
    return RetryPolicy(cfg);
}

} // namespace

TEST_CASE("RetryPolicy never 列表绝对不重试") {
    auto rp = makePolicy();
    ToolKey k{"pg", "query"};
    BackendMeta meta;
    const uint64_t dl = perfacet::nowMs() + 10000;
    CHECK_FALSE(rp.shouldRetry(FailureClass::Authz, k, meta, 1, dl, false, false));
    CHECK_FALSE(rp.shouldRetry(FailureClass::Cancelled, k, meta, 1, dl, false, false));
    CHECK_FALSE(rp.shouldRetry(FailureClass::Protocol, k, meta, 1, dl, false, false));
    CHECK_FALSE(rp.shouldRetry(FailureClass::Throttled, k, meta, 1, dl, false, false));
    CHECK_FALSE(rp.shouldRetry(FailureClass::Capability, k, meta, 1, dl, false, false));
}

TEST_CASE("RetryPolicy Timeout 必须命中 idempotent_tools") {
    auto rp = makePolicy();
    BackendMeta meta;
    meta.idempotentTools = {"search"};
    const uint64_t dl = perfacet::nowMs() + 10000;
    CHECK_FALSE(rp.shouldRetry(FailureClass::Timeout, ToolKey{"pg", "query"}, meta, 1, dl,
                               false, false));
    CHECK(rp.shouldRetry(FailureClass::Timeout, ToolKey{"pg", "search"}, meta, 1, dl, false,
                         false));
}

TEST_CASE("RetryPolicy 在途未完成禁止当可重试 Timeout") {
    auto rp = makePolicy();
    BackendMeta meta;
    meta.idempotentTools = {"search"};
    const uint64_t dl = perfacet::nowMs() + 10000;
    CHECK_FALSE(rp.shouldRetry(FailureClass::Timeout, ToolKey{"pg", "search"}, meta, 1, dl,
                               false, true));
}

TEST_CASE("RetryPolicy 电路 OPEN 或过 deadline 或耗尽 attempt 不重试") {
    auto rp = makePolicy(2);
    ToolKey k{"echo", "ping"};
    BackendMeta meta;
    const uint64_t dl = perfacet::nowMs() + 10000;
    CHECK_FALSE(rp.shouldRetry(FailureClass::Unavailable, k, meta, 1, dl, true, false));
    CHECK_FALSE(rp.shouldRetry(FailureClass::Unavailable, k, meta, 1, 1, false, false));
    CHECK(rp.shouldRetry(FailureClass::Unavailable, k, meta, 1, dl, false, false));
    CHECK_FALSE(rp.shouldRetry(FailureClass::Unavailable, k, meta, 2, dl, false, false));
}

TEST_CASE("RetryPolicy Upstream 默认可重试") {
    auto rp = makePolicy();
    ToolKey k{"echo", "ping"};
    BackendMeta meta;
    const uint64_t dl = perfacet::nowMs() + 10000;
    CHECK(rp.shouldRetry(FailureClass::Upstream, k, meta, 1, dl, false, false));
    CHECK_FALSE(rp.shouldRetry(FailureClass::Ok, k, meta, 1, dl, false, false));
}
