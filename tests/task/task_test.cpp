#include "perfacet/task/MemTaskStore.h"
#include "helpers.h"
#include "detail/Time.h"

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

TEST_CASE("MemTaskStore TTL 回收且满则拒") {
    perfacet::MemTaskStore store(2);
    auto who = perfacet::test::testWho("bot");
    perfacet::Task a{who};
    a.taskId = "tsk_a";
    a.createdAtMs = perfacet::nowMs();
    a.ttlMs = 20;
    CHECK(store.insert(a));
    perfacet::Task b{who};
    b.taskId = "tsk_b";
    b.createdAtMs = perfacet::nowMs();
    b.ttlMs = 3600000;
    CHECK(store.insert(b));
    perfacet::Task c{who};
    c.taskId = "tsk_c";
    c.createdAtMs = perfacet::nowMs();
    c.ttlMs = 3600000;
    CHECK_FALSE(store.insert(c));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(store.get("tsk_a"));
    CHECK(store.insert(c));
}
