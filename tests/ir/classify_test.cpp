#include "perfacet/ir/Request.h"

#include <doctest/doctest.h>

using perfacet::ir::FailureClass;
using perfacet::ir::Response;
using perfacet::ir::classify;
using perfacet::ir::failureClassName;

TEST_CASE("classify success") {
    Response r;
    r.klass = FailureClass::Ok;
    r.isError = false;
    CHECK(static_cast<int>(classify(r, {})) == static_cast<int>(FailureClass::Ok));
}

TEST_CASE("classify timeout errno") {
    Response r;
    CHECK(static_cast<int>(classify(r, std::make_error_code(std::errc::timed_out))) ==
          static_cast<int>(FailureClass::Timeout));
}

TEST_CASE("classify connection refused") {
    Response r;
    CHECK(static_cast<int>(classify(r, std::make_error_code(std::errc::connection_refused))) ==
          static_cast<int>(FailureClass::Unavailable));
}

TEST_CASE("classify json-rpc protocol") {
    Response r;
    r.isError = true;
    r.body = {{"code", -32601}, {"message", "x"}};
    CHECK(static_cast<int>(classify(r, {})) == static_cast<int>(FailureClass::Protocol));
}

TEST_CASE("classify 上游 -32021 是 Upstream，网关自身才是 Capability") {
    Response r;
    r.isError = true;
    r.body = {{"code", -32021}, {"message", "x"}};
    CHECK(static_cast<int>(classify(r, {})) == static_cast<int>(FailureClass::Upstream));
    CHECK(static_cast<int>(classify(r, {}, true)) ==
          static_cast<int>(FailureClass::Capability));
}

TEST_CASE("failureClassName roundtrip") {
    CHECK(std::string(failureClassName(FailureClass::Throttled)) == "Throttled");
    auto n = perfacet::ir::failureClassFromName("Authz");
    REQUIRE(n);
    CHECK(static_cast<int>(*n) == static_cast<int>(FailureClass::Authz));
}
