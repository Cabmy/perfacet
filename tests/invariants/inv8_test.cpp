#include "helpers.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/catalog/FacetView.h"
#include "perfacet/catalog/ToolIndex.h"
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

#include <doctest/doctest.h>

#include <fstream>

using namespace perfacet;
using namespace perfacet::test;

TEST_CASE("不变量8 Policy Deny 不进 Governor/InFlight/Backend") {
    YamlConfig cfg;
    cfg.taxonomy = Taxonomy(std::vector<std::string>{"l0", "l1"});
    cfg.listTtlMs = 5000;
    cfg.promoteAfterMs = 2000;
    cfg.queueWaitMs = 50;
    cfg.grantsPath = "/tmp/pf_inv8_grants.jsonl";
    std::ofstream(cfg.grantsPath, std::ios::trunc).close();

    netlib::EventLoop loop;
    Catalog catalog;
    CatalogEntry e;
    e.name = "pay";
    e.meta.level = 1;
    e.meta.secret = true;
    auto backend = std::make_unique<CountingBackend>();
    auto* be = backend.get();
    e.backend = std::move(backend);
    catalog.add(std::move(e));

    ToolIndex index;
    index.replace("pay", {IndexedTool{"x", "x", ir::Json::object()}});
    RankPolicy policy(catalog);
    FacetView facet(index, policy);
    CountingGovernor gov;
    Counters counters;
    InFlight inflight(&counters);
    CountCircuit circuit(5, 1000, 1, &loop);
    StubHealth health;
    MemTaskStore tasks;
    NullTracer tracer;
    netlib::ThreadPool pool(1);
    JsonlAuditLog audit("/tmp/pf_inv8_audit.jsonl", &pool);
    RetryPolicy retry(cfg);
    JsonlGrantStore grants(cfg.grantsPath, &cfg.taxonomy, 0, 1000);

    Pipeline pipe(Pipeline::Deps{&loop, &policy, &gov, &inflight, &circuit, &health,
                                 &catalog, &index, &facet, &tasks, &tracer, &audit,
                                 &counters, &retry, &grants, &cfg});

    ir::Request req = testReq(testWho("bot", 0, true, false, "l0"));
    req.method = "tools/call";
    req.name = "pay__x";
    req.deadlineMs = nowMs() + 5000;
    req.trace.traceId = "abc123";
    req.params = ir::Json{{"name", "pay__x"}, {"arguments", ir::Json::object()}};

    bool done = false;
    pipe.handle(req, [&](ir::Response r) {
        CHECK(static_cast<int>(r.klass) == static_cast<int>(ir::FailureClass::Authz));
        done = true;
    });
    CHECK(done);
    CHECK(gov.acquires.load() == 0);
    CHECK(be->calls.load() == 0);
    CHECK(inflight.held() == 0);
}
