#include "perfacet/ir/ToolKey.h"

#include <doctest/doctest.h>

using perfacet::ir::ToolKey;

TEST_CASE("ToolKey first __ split a__b__c") {
    auto k = ToolKey::parse("a__b__c");
    REQUIRE(k);
    CHECK(k->backend == "a");
    CHECK(k->tool == "b__c");
    CHECK(k->str() == "a__b__c");
}

TEST_CASE("ToolKey missing separator") {
    CHECK_FALSE(ToolKey::parse("echo"));
    CHECK_FALSE(ToolKey::parse(""));
    CHECK_FALSE(ToolKey::parse("__x"));
    CHECK_FALSE(ToolKey::parse("x__"));
}

TEST_CASE("builtin perfacet__request_elevation") {
    auto k = ToolKey::parse("perfacet__request_elevation");
    REQUIRE(k);
    CHECK(k->backend == "perfacet");
    CHECK(k->tool == "request_elevation");
}
