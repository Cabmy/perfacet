// 网关附加延迟：total − upstream。本机 instant backend，不走 HTTP。
#include "helpers.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/catalog/FacetView.h"
#include "perfacet/catalog/ToolIndex.h"
#include "perfacet/govern/LocalGovernor.h"
#include "perfacet/health/CountCircuit.h"
#include "perfacet/health/RetryPolicy.h"
#include "perfacet/pipeline/InFlight.h"
#include "perfacet/pipeline/Pipeline.h"
#include "perfacet/policy/JsonlGrantStore.h"
#include "perfacet/policy/RankPolicy.h"
#include "perfacet/policy/YamlConfig.h"
#include "perfacet/task/MemTaskStore.h"
#include "detail/Time.h"

#include "netlib/EventLoop.h"
#include "netlib/ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <vector>

using namespace perfacet;
using namespace perfacet::test;

static uint64_t pct(std::vector<uint64_t> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t i =
        std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1)));
    return v[i];
}

int main() {
    YamlConfig cfg;
    cfg.taxonomy = Taxonomy(std::vector<std::string>{"l0"});
    cfg.listTtlMs = 5000;
    cfg.promoteAfterMs = 60000;
    cfg.queueWaitMs = 5000;
    cfg.perToolConcurrency = 64;
    cfg.perPrincipalConcurrency = 64;
    cfg.grantsPath = "/tmp/pf_bench_grants.jsonl";
    std::ofstream(cfg.grantsPath, std::ios::trunc).close();
    std::ofstream("/tmp/pf_bench_audit.jsonl", std::ios::trunc).close();

    netlib::EventLoop loop;
    Catalog catalog;
    CatalogEntry e;
    e.name = "echo";
    e.meta.level = 0;
    e.backend = std::make_unique<CountingBackend>();
    catalog.add(std::move(e));
    ToolIndex index;
    index.replace("echo", {IndexedTool{"ping", "", ir::Json::object()}});
    RankPolicy policy(catalog);
    FacetView facet(index, policy);
    Counters counters;
    LocalGovernor gov(cfg, &loop, &counters);
    InFlight inflight(&counters);
    CountCircuit circuit(50, 1000, 1, &loop);
    StubHealth health;
    MemTaskStore tasks;
    NullTracer tracer;
    netlib::ThreadPool pool(1);
    JsonlAuditLog audit("/tmp/pf_bench_audit.jsonl", &pool);
    RetryPolicy retry(cfg);
    JsonlGrantStore grants(cfg.grantsPath, &cfg.taxonomy, 0, 1000);
    Pipeline pipe(Pipeline::Deps{&loop, &policy, &gov, &inflight, &circuit, &health,
                                 &catalog, &index, &facet, &tasks, &tracer, &audit,
                                 &counters, &retry, &grants, &cfg});

    const auto who = testWho("bot", 0, true, false, "l0");
    constexpr int N = 50000;
    std::vector<uint64_t> addMs;
    addMs.reserve(N);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        auto req = testReq(who);
        req.method = "tools/call";
        req.name = "echo__ping";
        req.deadlineMs = nowMs() + 5000;
        req.params = ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json{{"i", i}}}};
        pipe.handle(std::move(req), [&](ir::Response r) {
            const uint64_t add = r.gatewayMs > r.upstreamMs ? r.gatewayMs - r.upstreamMs
                                                            : r.gatewayMs;
            addMs.push_back(add);
        });
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double sec = std::chrono::duration<double>(t1 - t0).count();
    const double rps = static_cast<double>(N) / sec;

    std::printf("perfacet_bench in-process\n");
    std::printf("  n=%d  wall_s=%.4f  rps=%.0f\n", N, sec, rps);
    std::printf("  gateway_minus_upstream_ms  p50=%llu  p99=%llu  max=%llu\n",
                static_cast<unsigned long long>(pct(addMs, 0.50)),
                static_cast<unsigned long long>(pct(addMs, 0.99)),
                static_cast<unsigned long long>(pct(addMs, 1.00)));
    return 0;
}
