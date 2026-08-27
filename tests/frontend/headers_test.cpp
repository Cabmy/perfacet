#include "perfacet/frontend/HttpMcp.h"

#include <doctest/doctest.h>

#include <map>

TEST_CASE("Accept 缺 json 为 406") {
    CHECK_FALSE(perfacet::acceptIncludesJson("text/event-stream"));
    CHECK(perfacet::acceptIncludesJson("application/json"));
    CHECK(perfacet::acceptIncludesJson("application/json, text/event-stream"));
}

TEST_CASE("Mcp-Session-Id / Last-Event-ID 为禁止头") {
    std::map<std::string, std::string> h;
    CHECK_FALSE(perfacet::hasForbiddenSessionHeader(h));
    h["mcp-session-id"] = "abc";
    CHECK(perfacet::hasForbiddenSessionHeader(h));
    h.clear();
    h["last-event-id"] = "1";
    CHECK(perfacet::hasForbiddenSessionHeader(h));
}

TEST_CASE("缺协议版本头可识别") {
    CHECK_FALSE(perfacet::acceptIncludesJson(""));
}
