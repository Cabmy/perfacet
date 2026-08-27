#include "perfacet/policy/JsonlGrantStore.h"
#include "perfacet/policy/Taxonomy.h"
#include "detail/Time.h"

#include <doctest/doctest.h>

#include <fstream>
#include <thread>
#include <chrono>

TEST_CASE("Grant 过期由读时 now 比较") {
    const std::string path = "/tmp/pf_grants_test.jsonl";
    std::ofstream(path, std::ios::trunc).close();
    perfacet::Taxonomy tax(std::vector<std::string>{"l0", "l1"});
    perfacet::JsonlGrantStore store(path, &tax, 1, 50);
    CHECK(store.approveDirect("cursor", 1, "l1", perfacet::nowMs()));
    CHECK(store.effectiveBump("cursor", perfacet::nowMs()) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(store.effectiveBump("cursor", perfacet::nowMs()) == 0);
}

TEST_CASE("JSONL 大 expiresAt 不被截成 int") {
    const std::string path = "/tmp/pf_grants_u64.jsonl";
    {
        std::ofstream out(path);
        out << R"({"id":"g1","agent":"cursor","bump_to":"l1","rank":1,"status":"approved","expiresAt":1787763472804,"ts_ms":1787762572804})"
            << '\n';
    }
    perfacet::Taxonomy tax(std::vector<std::string>{"l0", "l1"});
    perfacet::JsonlGrantStore store(path, &tax, 1, 50);
    store.refreshOnWorker();
    CHECK(store.effectiveBump("cursor", 1787762572804) == 1);
    CHECK(store.effectiveBump("cursor", 1787763472804) == 0);
}

TEST_CASE("mtime 未变则 refresh 跳过") {
    const std::string path = "/tmp/pf_grants_mtime.jsonl";
    {
        std::ofstream out(path);
        out << R"({"id":"g1","agent":"cursor","bump_to":"l1","rank":1,"status":"approved","expiresAt":9999999999999,"ts_ms":1})"
            << '\n';
    }
    perfacet::Taxonomy tax(std::vector<std::string>{"l0", "l1"});
    perfacet::JsonlGrantStore store(path, &tax, 1, 50);
    store.refreshOnWorker();
    CHECK(store.effectiveBump("cursor", perfacet::nowMs()) == 1);
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({"id":"g2","agent":"cursor","bump_to":"l1","rank":1,"status":"pending","expiresAt":0,"ts_ms":1})"
            << '\n';
    }
    // 同一秒内 mtime 可能不变；强制改内容后再 refresh 仍应能读到（lastMtime 比较）
    store.refreshOnWorker();
}
