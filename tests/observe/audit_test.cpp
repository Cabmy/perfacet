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
#include <thread>

using namespace perfacet;
using namespace perfacet::test;

TEST_CASE("不变量10 Deny 审计含 trace_id 且无 token") {
    const std::string path = "/tmp/pf_inv10_audit.jsonl";
    std::ofstream(path, std::ios::trunc).close();

    YamlConfig cfg;
    cfg.taxonomy = Taxonomy(std::vector<std::string>{"l0", "l1"});
    cfg.grantsPath = "/tmp/pf_inv10_grants.jsonl";
    std::ofstream(cfg.grantsPath, std::ios::trunc).close();

    netlib::EventLoop loop;
    Catalog catalog;
    CatalogEntry e;
    e.name = "pay";
    e.meta.level = 1;
    e.meta.secret = true;
    e.backend = std::make_unique<CountingBackend>();
    catalog.add(std::move(e));
    ToolIndex index;
    index.replace("pay", {IndexedTool{"x", "", ir::Json::object()}});
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
    JsonlAuditLog audit(path, &pool);
    RetryPolicy retry(cfg);
    JsonlGrantStore grants(cfg.grantsPath, &cfg.taxonomy, 0, 1000);

    Pipeline pipe(Pipeline::Deps{&loop, &policy, &gov, &inflight, &circuit, &health,
                                 &catalog, &index, &facet, &tasks, &tracer, &audit,
                                 &counters, &retry, &grants, &cfg});

    ir::Request req = testReq(testWho("bot", 0, true, false, "l0"));
    req.method = "tools/call";
    req.name = "pay__x";
    req.trace.traceId = "deadbeefcafebabe";
    req.deadlineMs = nowMs() + 1000;

    pipe.handle(req, [](ir::Response) {});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    CHECK(line.find("\"event\":\"deny\"") != std::string::npos);
    CHECK(line.find("deadbeefcafebabe") != std::string::npos);
    CHECK(line.find("pf_") == std::string::npos);
    CHECK(line.find("Bearer") == std::string::npos);
    CHECK(line.find("token") == std::string::npos);
}
