#include "helpers.h"
#include "perfacet/govern/LocalGovernor.h"
#include "perfacet/observe/Counters.h"
#include "perfacet/policy/YamlConfig.h"
#include "detail/Time.h"

#include "netlib/EventLoop.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using perfacet::Counters;
using perfacet::Governor;
using perfacet::GovernorToolCfg;
using perfacet::LocalGovernor;
using perfacet::YamlConfig;
using perfacet::ir::Principal;
using perfacet::ir::ToolKey;

TEST_CASE("4 抢 3 FIFO 与 Permit 析构归还") {
    YamlConfig cfg;
    cfg.perToolConcurrency = 3;
    cfg.perPrincipalConcurrency = 10;
    cfg.queueWaitMs = 200;
    GovernorToolCfg tc;
    tc.maxConcurrency = 3;
    tc.queueWaitMs = 200;
    tc.status = GovernorToolCfg::Status::Active;
    cfg.governorTools["pg__query"] = tc;

    netlib::EventLoop loop;
    std::thread thr([&]() { loop.loop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Counters counters;
    LocalGovernor gov(cfg, &loop, &counters);
    Principal who = perfacet::test::testWho("bot");
    ToolKey key{"pg", "query"};

    std::vector<Governor::Permit> held;
    std::mutex mu;
    std::atomic<int> go{0}, rej{0};

    auto acquire = [&](const std::string& id) {
        Principal p = who;
        p.agentId = id;
        std::promise<void> done;
        auto fut = done.get_future();
        loop.runInLoop([&]() {
            gov.acquire(p, key, perfacet::nowMs() + 200, [&](Governor::Admit a, Governor::Permit perm) {
                if (a == Governor::Admit::Go) {
                    go++;
                    std::lock_guard<std::mutex> lk(mu);
                    held.push_back(std::move(perm));
                } else {
                    rej++;
                }
                done.set_value();
            });
        });
        fut.wait();
    };

    acquire("a");
    acquire("b");
    acquire("c");
    CHECK(go.load() == 3);

    std::promise<void> fourth;
    auto fourthFut = fourth.get_future();
    loop.runInLoop([&]() {
        Principal p = who;
        p.agentId = "d";
        gov.acquire(p, key, perfacet::nowMs() + 200, [&](Governor::Admit a, Governor::Permit) {
            if (a == Governor::Admit::Reject) rej++;
            if (a == Governor::Admit::Go) go++;
            fourth.set_value();
        });
    });
    fourthFut.wait();
    CHECK(rej.load() >= 1);

    loop.runInLoop([&]() {
        std::lock_guard<std::mutex> lk(mu);
        held.clear();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(counters.permitHeld.load() == 0);

    loop.quit();
    thr.join();
}
