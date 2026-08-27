#include "helpers.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/catalog/FacetView.h"
#include "perfacet/catalog/ToolIndex.h"
#include "perfacet/health/CountCircuit.h"
#include "perfacet/health/RetryPolicy.h"
#include "perfacet/ir/ClientCaps.h"
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

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

using namespace perfacet;
using namespace perfacet::test;

namespace {

struct PipeEnv {
    YamlConfig cfg;
    netlib::EventLoop* loop;
    Catalog catalog;
    ToolIndex index;
    RankPolicy policy;
    FacetView facet;
    CountingGovernor gov;
    Counters counters;
    InFlight inflight;
    CountCircuit circuit;
    StubHealth health;
    MemTaskStore tasks;
    NullTracer tracer;
    netlib::ThreadPool pool;
    JsonlAuditLog audit;
    RetryPolicy retry;
    JsonlGrantStore grants;
    Pipeline pipe;
    HoldingBackend* hold = nullptr;
    CountingBackend* count = nullptr;

    PipeEnv(netlib::EventLoop* lp, YamlConfig c, const std::string& grantPath)
        : cfg(std::move(c)), loop(lp), policy(catalog), facet(index, policy),
          inflight(&counters), circuit(cfg.circuitOpenAfter, cfg.circuitCooldownMs,
                                       cfg.halfOpenProbes, lp),
          tasks(cfg.taskMax), pool(1), audit("/tmp/pf_pipe_audit.jsonl", &pool),
          retry(cfg), grants(grantPath, &cfg.taxonomy, 0, 1000),
          pipe(Pipeline::Deps{lp, &policy, &gov, &inflight, &circuit, &health, &catalog,
                              &index, &facet, &tasks, &tracer, &audit, &counters, &retry,
                              &grants, &cfg}) {}
};

} // namespace

TEST_CASE("不变量19 在途命中复用已有 taskId") {
    YamlConfig cfg;
    cfg.taxonomy = Taxonomy(std::vector<std::string>{"l0"});
    cfg.promoteAfterMs = 40;
    cfg.taskTtlMs = 3600000;
    cfg.taskMax = 100;
    cfg.grantsPath = "/tmp/pf_inv19_grants.jsonl";
    std::ofstream(cfg.grantsPath, std::ios::trunc).close();
    std::ofstream("/tmp/pf_pipe_audit.jsonl", std::ios::trunc).close();

    netlib::EventLoop loop;
    std::thread thr([&]() { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    PipeEnv env(&loop, cfg, cfg.grantsPath);
    auto hold = std::make_unique<HoldingBackend>();
    env.hold = hold.get();
    CatalogEntry e;
    e.name = "slow";
    e.meta.level = 0;
    e.backend = std::move(hold);
    env.catalog.add(std::move(e));
    env.index.replace("slow", {IndexedTool{"work", "", ir::Json::object()}});

    const auto who = testWho("bot", 0, true, false, "l0");
    std::string id1, id2;
    std::atomic<int> got{0};

    loop.runInLoop([&]() {
        auto req1 = testReq(who);
        req1.method = "tools/call";
        req1.name = "slow__work";
        req1.caps.tasks = true;
        req1.deadlineMs = nowMs() + 5000;
        req1.params = ir::Json{{"name", "slow__work"}, {"arguments", ir::Json{{"x", 1}}}};
        env.pipe.handle(std::move(req1), [&](ir::Response r) {
            if (r.body.is_object() && r.body.contains("taskId")) id1 = r.body["taskId"];
            got.fetch_add(1);
        });
        auto req2 = testReq(who);
        req2.method = "tools/call";
        req2.name = "slow__work";
        req2.caps.tasks = true;
        req2.deadlineMs = nowMs() + 5000;
        req2.params = ir::Json{{"name", "slow__work"}, {"arguments", ir::Json{{"x", 1}}}};
        env.pipe.handle(std::move(req2), [&](ir::Response r) {
            if (r.body.is_object() && r.body.contains("taskId")) id2 = r.body["taskId"];
            got.fetch_add(1);
        });
    });
    for (int i = 0; i < 80 && got.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(got.load() == 2);
    CHECK_FALSE(id1.empty());
    CHECK(id1 == id2);
    CHECK(env.hold->pending() == 1);

    loop.runInLoop([&]() { env.hold->flushOk(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loop.quit();
    thr.join();
}

TEST_CASE("promote 后 Timeout 不再 fireAttempt") {
    YamlConfig cfg;
    cfg.taxonomy = Taxonomy(std::vector<std::string>{"l0"});
    cfg.promoteAfterMs = 20;
    cfg.retryMaxAttempts = 3;
    cfg.grantsPath = "/tmp/pf_retry_grants.jsonl";
    std::ofstream(cfg.grantsPath, std::ios::trunc).close();

    netlib::EventLoop loop;
    std::thread thr([&]() { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    PipeEnv env(&loop, cfg, cfg.grantsPath);
    auto hold = std::make_unique<HoldingBackend>();
    env.hold = hold.get();
    CatalogEntry e;
    e.name = "echo";
    e.meta.level = 0;
    e.meta.idempotentTools = {"ping"};
    e.backend = std::move(hold);
    env.catalog.add(std::move(e));
    env.index.replace("echo", {IndexedTool{"ping", "", ir::Json::object()}});

    const auto who = testWho("bot", 0, true, false, "l0");
    std::atomic<int> got{0};
    loop.runInLoop([&]() {
        auto req = testReq(who);
        req.method = "tools/call";
        req.name = "echo__ping";
        req.caps.tasks = true;
        req.deadlineMs = nowMs() + 10000;
        req.params = ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json::object()}};
        env.pipe.handle(std::move(req), [&](ir::Response) { got.fetch_add(1); });
    });
    for (int i = 0; i < 50 && got.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(got.load() == 1);
    CHECK(env.hold->pending() == 1);
    loop.runInLoop([&]() {
        ir::Response r;
        r.isError = true;
        r.klass = ir::FailureClass::Timeout;
        r.body = ir::jsonRpcError(-32000, "timeout");
        env.hold->flush(std::move(r));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(env.hold->pending() == 0);

    loop.quit();
    thr.join();
}

TEST_CASE("OPEN 拒绝累加 circuit_open") {
    YamlConfig cfg;
    cfg.taxonomy = Taxonomy(std::vector<std::string>{"l0"});
    cfg.circuitOpenAfter = 1;
    cfg.grantsPath = "/tmp/pf_co_grants.jsonl";
    std::ofstream(cfg.grantsPath, std::ios::trunc).close();

    netlib::EventLoop loop;
    PipeEnv env(&loop, cfg, cfg.grantsPath);
    auto be = std::make_unique<CountingBackend>();
    env.count = be.get();
    CatalogEntry e;
    e.name = "echo";
    e.meta.level = 0;
    e.backend = std::move(be);
    env.catalog.add(std::move(e));
    env.index.replace("echo", {IndexedTool{"ping", "", ir::Json::object()}});
    env.circuit.onFailure("echo", nowMs());
    CHECK(env.circuit.isOpen("echo", nowMs()));

    const auto who = testWho("bot", 0, true, false, "l0");
    for (int i = 0; i < 5; ++i) {
        auto req = testReq(who);
        req.method = "tools/call";
        req.name = "echo__ping";
        req.deadlineMs = nowMs() + 1000;
        req.params = ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json{{"n", i}}}};
        env.pipe.handle(std::move(req), [](ir::Response) {});
    }
    CHECK(env.counters.circuitOpen.load() == 5);
    CHECK(env.count->calls.load() == 0);
}

TEST_CASE("带 confirm 才第二条上游") {
    YamlConfig cfg;
    cfg.taxonomy = Taxonomy(std::vector<std::string>{"l0"});
    cfg.promoteAfterMs = 60000;
    cfg.grantsPath = "/tmp/pf_conf_grants.jsonl";
    std::ofstream(cfg.grantsPath, std::ios::trunc).close();

    netlib::EventLoop loop;
    std::thread thr([&]() { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    PipeEnv env(&loop, cfg, cfg.grantsPath);
    auto hold = std::make_unique<HoldingBackend>();
    env.hold = hold.get();
    CatalogEntry e;
    e.name = "echo";
    e.meta.level = 0;
    e.backend = std::move(hold);
    env.catalog.add(std::move(e));
    env.index.replace("echo", {IndexedTool{"ping", "", ir::Json::object()}});

    const auto who = testWho("bot", 0, true, false, "l0");
    std::string confirm;
    std::atomic<int> hits{0};
    loop.runInLoop([&]() {
        auto req = testReq(who);
        req.method = "tools/call";
        req.name = "echo__ping";
        req.deadlineMs = nowMs() + 5000;
        req.params = ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json{{"n", 1}}}};
        env.pipe.handle(std::move(req), [](ir::Response) {});
        auto req2 = testReq(who);
        req2.method = "tools/call";
        req2.name = "echo__ping";
        req2.deadlineMs = nowMs() + 5000;
        req2.params = ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json{{"n", 1}}}};
        env.pipe.handle(std::move(req2), [&](ir::Response r) {
            if (r.body.is_object() && r.body.contains("content")) {
                auto t = r.body["content"][0]["text"].get<std::string>();
                auto p = t.find("if_");
                if (p != std::string::npos) confirm = t.substr(p, 3 + 32);
            }
            hits.fetch_add(1);
        });
    });
    for (int i = 0; i < 40 && hits.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(env.hold->pending() == 1);
    CHECK(env.counters.inflightHit.load() >= 1);
    CHECK_FALSE(confirm.empty());

    loop.runInLoop([&]() {
        auto req3 = testReq(who);
        req3.method = "tools/call";
        req3.name = "echo__ping";
        req3.deadlineMs = nowMs() + 5000;
        req3.params = ir::Json{{"name", "echo__ping"}, {"arguments", ir::Json{{"n", 1}}}};
        req3.meta = ir::Json{{ir::kConfirmKey, confirm}};
        env.pipe.handle(std::move(req3), [](ir::Response) {});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(env.hold->pending() == 2);
    CHECK(env.counters.inflightConfirm.load() >= 1);

    loop.runInLoop([&]() { env.hold->flushOk(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loop.quit();
    thr.join();
}
