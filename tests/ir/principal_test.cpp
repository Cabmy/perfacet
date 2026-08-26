#include "helpers.h"
#include "perfacet/policy/YamlConfig.h"
#include "perfacet/policy/YamlIdentityStore.h"

#include <doctest/doctest.h>

#include <fstream>

TEST_CASE("authenticate 才构造 AuthenticatedPrincipal") {
    const char* path = "/tmp/pf_principal.yaml";
    {
        std::ofstream out(path);
        out << R"(
listen: "127.0.0.1:0"
access:
  levels: [default]
agents:
  cursor: { token: "t1" }
  ops: { token: "t-admin", admin: true }
backends:
  - name: echo
    url: "http://127.0.0.1:9/mcp"
)";
    }
    auto cfg = perfacet::YamlConfig::load(path);
    perfacet::YamlIdentityStore store(cfg);
    CHECK_FALSE(store.authenticate("nope"));
    auto who = store.authenticate("t1");
    REQUIRE(who);
    CHECK(who->agentId == "cursor");
    CHECK(who->hasLevel);
    CHECK_FALSE(who->admin);

    auto ops = store.authenticate("t-admin");
    REQUIRE(ops);
    CHECK(ops->admin);
    CHECK_FALSE(ops->hasLevel);

    auto forged = perfacet::test::testWho("bot", 0, true);
    CHECK(forged.agentId == "bot");
    perfacet::ir::Request req{forged};
    CHECK(req.who.agentId == "bot");
}
