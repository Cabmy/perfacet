#include "perfacet/pipeline/InFlight.h"
#include "perfacet/observe/Counters.h"
#include "detail/Time.h"

#include <doctest/doctest.h>

using perfacet::Counters;
using perfacet::InFlight;
using perfacet::ir::ToolKey;

TEST_CASE("同 params 命中；不同 agent 不合流") {
    Counters c;
    InFlight inf(&c);
    ToolKey k{"pg", "query"};
    auto id = inf.insert("a1", k, "{\"x\":1}", "{x:1}", perfacet::nowMs());
    auto hit = inf.lookup("a1", k, "{\"x\":1}");
    REQUIRE(hit);
    CHECK(hit->inflightId == id);
    CHECK_FALSE(inf.lookup("a2", k, "{\"x\":1}"));
    inf.erase(id);
    CHECK_FALSE(inf.lookup("a1", k, "{\"x\":1}"));
    CHECK(c.inflightHeld.load() == 0);
}
