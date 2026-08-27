#include "helpers.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/catalog/FacetView.h"
#include "perfacet/catalog/ToolIndex.h"
#include "perfacet/policy/RankPolicy.h"

#include <doctest/doctest.h>

using namespace perfacet;
using namespace perfacet::test;

TEST_CASE("secret 不够格是 Unknown 不是 Deny") {
    Catalog catalog;
    CatalogEntry e;
    e.name = "pay";
    e.meta.level = 1;
    e.meta.secret = true;
    catalog.add(std::move(e));
    RankPolicy policy(catalog);
    auto who = testWho("intern", 0, true, false, "l0");
    CHECK(policy.authorizeCall(who, ir::ToolKey{"pay", "x"}) == Decision::Unknown);
    CatalogEntry vis;
    vis.name = "pg";
    vis.meta.level = 1;
    vis.meta.secret = false;
    catalog.add(std::move(vis));
    CHECK(policy.authorizeCall(who, ir::ToolKey{"pg", "q"}) == Decision::Deny);
}

TEST_CASE("纯 admin 无业务工具") {
    Catalog catalog;
    CatalogEntry e;
    e.name = "echo";
    e.meta.level = 0;
    catalog.add(std::move(e));
    ToolIndex index;
    index.replace("echo", {IndexedTool{"ping", "", ir::Json::object()}});
    RankPolicy policy(catalog);
    FacetView facet(index, policy);
    auto admin = testWho("ops", 0, false, true, "");
    auto tools = facet.listTools(admin);
    bool hasEcho = false, hasStatus = false, hasElev = false;
    for (const auto& t : tools) {
        const auto n = t["name"].get<std::string>();
        if (n == "echo__ping") hasEcho = true;
        if (n == "perfacet__upstream_status") hasStatus = true;
        if (n == "perfacet__request_elevation") hasElev = true;
    }
    CHECK_FALSE(hasEcho);
    CHECK(hasStatus);
    CHECK_FALSE(hasElev);
    CHECK(policy.authorizeCall(admin, ir::ToolKey{"echo", "ping"}) == Decision::Deny);
}

TEST_CASE("一档全集") {
    Catalog catalog;
    CatalogEntry a;
    a.name = "echo";
    a.meta.level = 0;
    catalog.add(std::move(a));
    CatalogEntry b;
    b.name = "pg";
    b.meta.level = 0;
    catalog.add(std::move(b));
    ToolIndex index;
    index.replace("echo", {IndexedTool{"ping", "", ir::Json::object()}});
    index.replace("pg", {IndexedTool{"query", "", ir::Json::object()}});
    RankPolicy policy(catalog);
    FacetView facet(index, policy);
    auto who = testWho("bot", 0, true, false, "default");
    auto tools = facet.listTools(who);
    bool echo = false, pg = false, elev = false;
    for (const auto& t : tools) {
        const auto n = t["name"].get<std::string>();
        if (n == "echo__ping") echo = true;
        if (n == "pg__query") pg = true;
        if (n == "perfacet__request_elevation") elev = true;
    }
    CHECK(echo);
    CHECK(pg);
    CHECK(elev);
}
