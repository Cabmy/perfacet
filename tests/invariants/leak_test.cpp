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

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <thread>

using namespace perfacet;
using namespace perfacet::test;

namespace {

constexpr int kEach = 20000; // 5 种终态 × 2e4 = 1e5
constexpr int kBatch = 2000;

YamlConfig baseCfg() {
    YamlConfig cfg;
    cfg.taxonomy = Taxonomy(std::vector<std::string>{"l0", "l1"});
    cfg.listTtlMs = 5000;
    cfg.promoteAfterMs = 1;
    cfg.queueWaitMs = 5000;
    cfg.perToolConcurrency = 100000;
    cfg.perPrincipalConcurrency = 100000;
    cfg.grantsPath = "/tmp/pf_leak_grants.jsonl";
    return cfg;
}

} // namespace

TEST_CASE("泄漏对账 1e5 混合终态后 permit/inflight 归零") {
    auto cfg = baseCfg();
    std::ofstream(cfg.grantsPath, std::ios::trunc).close();
    std::ofstream("/tmp/pf_leak_audit.jsonl", std::ios::trunc).close();

    netlib::EventLoop loop;
    std::thread thr([&]() { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto onLoop = [&](std::function<void()> fn) {
        std::promise<void> done;
        auto fut = done.get_future();
        loop.runInLoop([&]() {
            fn();
            done.set_value();
        });
        fut.wait();
    };

    Catalog catalog;
    {
        CatalogEntry secret;
        secret.name = "pay";
        secret.meta.level = 1;
        secret.meta.secret = true;
        secret.backend = std::make_unique<CountingBackend>();
        catalog.add(std::move(secret));
    }
    auto echo = std::make_unique<CountingBackend>();
    CountingBackend* echoBe = echo.get();
    {
        CatalogEntry e;
        e.name = "echo";
        e.meta.level = 0;
        e.backend = std::move(echo);
        catalog.add(std::move(e));
    }
    auto hold = std::make_unique<HoldingBackend>();
    HoldingBackend* holdBe = hold.get();
    {
        CatalogEntry e;
        e.name = "slow";
        e.meta.level = 0;
        e.backend = std::move(hold);
        catalog.add(std::move(e));
    }

    ToolIndex index;
    index.replace("pay", {IndexedTool{"x", "", ir::Json::object()}});
    index.replace("echo", {IndexedTool{"ping", "", ir::Json::object()}});
    index.replace("slow", {IndexedTool{"work", "", ir::Json::object()}});
    RankPolicy policy(catalog);
    FacetView facet(index, policy);
    Counters counters;
    LocalGovernor gov(cfg, &loop, &counters);
    InFlight inflight(&counters);
    CountCircuit circuit(50, 1000, 1, &loop);
    StubHealth health;
    MemTaskStore tasks;
    NullTracer tracer;
    netlib::ThreadPool pool(2);
    JsonlAuditLog audit("/tmp/pf_leak_audit.jsonl", &pool);
    RetryPolicy retry(cfg);
    JsonlGrantStore grants(cfg.grantsPath, &cfg.taxonomy, 0, 1000);
    Pipeline pipe(Pipeline::Deps{&loop, &policy, &gov, &inflight, &circuit, &health,
                                 &catalog, &index, &facet, &tasks, &tracer, &audit,
                                 &counters, &retry, &grants, &cfg});

    const auto who = testWho("bot", 0, true, false, "l0");

    auto waitZero = [&](Counters& c, InFlight& inf) {
        for (int i = 0; i < 400; ++i) {
            if (c.permitHeld.load() == 0 && c.inflightHeld.load() == 0 && inf.held() == 0)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    // 1) Deny
    onLoop([&]() {
        for (int i = 0; i < kEach; ++i) {
            auto req = testReq(who);
            req.method = "tools/call";
            req.name = "pay__x";
            req.deadlineMs = nowMs() + 5000;
            req.params = ir::Json{{"name", "pay__x"}, {"arguments", ir::Json::object()}};
            pipe.handle(std::move(req), [](ir::Response) {});
        }
    });

    // 2) 同步成功
    onLoop([&]() {
        for (int i = 0; i < kEach; ++i) {
            auto req = testReq(who);
            req.method = "tools/call";
            req.name = "echo__ping";
            req.deadlineMs = nowMs() + 5000;
            req.params = ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json{{"i", i}}}};
            pipe.handle(std::move(req), [](ir::Response) {});
        }
    });
    CHECK(echoBe->calls.load() == kEach);
    CHECK(waitZero(counters, inflight));

    // 3) Throttled：concurrency=1 占住槽，其余 queue_wait=0 立即 Reject
    {
        YamlConfig tcfg = baseCfg();
        tcfg.perToolConcurrency = 1;
        tcfg.perPrincipalConcurrency = 1;
        tcfg.queueWaitMs = 0;
        Catalog tcat;
        auto tbe = std::make_unique<HoldingBackend>();
        HoldingBackend* tb = tbe.get();
        CatalogEntry e;
        e.name = "echo";
        e.meta.level = 0;
        e.backend = std::move(tbe);
        tcat.add(std::move(e));
        ToolIndex tidx;
        tidx.replace("echo", {IndexedTool{"ping", "", ir::Json::object()}});
        RankPolicy tpol(tcat);
        FacetView tfacet(tidx, tpol);
        Counters tcounters;
        LocalGovernor tgov(tcfg, &loop, &tcounters);
        InFlight tinf(&tcounters);
        CountCircuit tcirc(50, 1000, 1, &loop);
        StubHealth th;
        MemTaskStore ttasks;
        NullTracer ttr;
        RetryPolicy tretry(tcfg);
        JsonlGrantStore tgrants(cfg.grantsPath, &tcfg.taxonomy, 0, 1000);
        Pipeline tpipe(Pipeline::Deps{&loop, &tpol, &tgov, &tinf, &tcirc, &th, &tcat, &tidx,
                                      &tfacet, &ttasks, &ttr, &audit, &tcounters, &tretry,
                                      &tgrants, &tcfg});
        std::atomic<int> throttled{0};
        onLoop([&]() {
            auto holdReq = testReq(who);
            holdReq.method = "tools/call";
            holdReq.name = "echo__ping";
            holdReq.deadlineMs = nowMs() + 5000;
            holdReq.params =
                ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json::object()}};
            tpipe.handle(std::move(holdReq), [](ir::Response) {});
            for (int i = 0; i < kEach; ++i) {
                auto req = testReq(who);
                req.method = "tools/call";
                req.name = "echo__ping";
                req.deadlineMs = nowMs() + 5000;
                req.params =
                    ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json{{"n", i}}}};
                tpipe.handle(std::move(req), [&](ir::Response r) {
                    if (r.klass == ir::FailureClass::Throttled) throttled.fetch_add(1);
                });
            }
        });
        CHECK(throttled.load() == kEach);
        onLoop([&]() { tb->flushOk(); });
        CHECK(waitZero(tcounters, tinf));
    }

    // 4) -32021  5) 升格句柄
    auto runPromote = [&](bool tasksCap) {
        for (int off = 0; off < kEach; off += kBatch) {
            const int n = std::min(kBatch, kEach - off);
            std::atomic<int> got{0};
            onLoop([&]() {
                for (int i = 0; i < n; ++i) {
                    auto req = testReq(who);
                    req.method = "tools/call";
                    req.name = "slow__work";
                    req.deadlineMs = nowMs() + 5000;
                    req.caps.tasks = tasksCap;
                    req.params = ir::Json{{"name", "slow__work"},
                                          {"arguments", ir::Json{{"n", off + i}}}};
                    pipe.handle(std::move(req), [&](ir::Response r) {
                        if (tasksCap) {
                            if (!r.isError) got.fetch_add(1);
                        } else if (r.klass == ir::FailureClass::Capability) {
                            got.fetch_add(1);
                        }
                    });
                }
            });
            for (int w = 0; w < 200 && got.load() < n; ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            CHECK(got.load() == n);
            onLoop([&]() { holdBe->flushOk(); });
            CHECK(waitZero(counters, inflight));
        }
    };
    runPromote(false);
    runPromote(true);

    CHECK(counters.permitHeld.load() == 0);
    CHECK(counters.inflightHeld.load() == 0);
    CHECK(inflight.held() == 0);

    loop.quit();
    thr.join();
}
