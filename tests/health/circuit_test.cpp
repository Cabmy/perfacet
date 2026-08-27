#include "perfacet/health/CountCircuit.h"
#include "detail/Time.h"

#include "netlib/EventLoop.h"

#include <doctest/doctest.h>

using perfacet::CountCircuit;

TEST_CASE("失败探活与 OPEN 不得合闸") {
    netlib::EventLoop loop;
    CountCircuit c(1, 10000, 1, &loop);
    const uint64_t now = perfacet::nowMs();
    CHECK_FALSE(c.isOpen("echo", now));
    CHECK(c.onFailure("echo", now));
    CHECK(c.isOpen("echo", now));
    c.onProbeSuccess("echo");
    CHECK(c.isOpen("echo", now));
    CHECK(c.state("echo", now) == CountCircuit::CState::Open);
}

TEST_CASE("HALF_OPEN 探活成功才合闸") {
    netlib::EventLoop loop;
    CountCircuit c(1, 1, 1, &loop);
    const uint64_t now = perfacet::nowMs();
    CHECK(c.onFailure("echo", now));
    CHECK(c.isOpen("echo", now));
    CHECK(c.state("echo", now + 2) == CountCircuit::CState::HalfOpen);
    c.onProbeSuccess("echo");
    CHECK_FALSE(c.isOpen("echo", now + 2));
    CHECK(c.state("echo", now + 2) == CountCircuit::CState::Closed);
}
