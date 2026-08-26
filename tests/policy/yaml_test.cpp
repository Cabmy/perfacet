#include "perfacet/policy/YamlConfig.h"

#include <doctest/doctest.h>

#include <fstream>
#include <stdexcept>

namespace {

std::string writeTmp(const std::string& name, const std::string& body) {
    const std::string p = "/tmp/" + name;
    std::ofstream out(p);
    out << body;
    return p;
}

} // namespace

TEST_CASE("一档省略 level 可启动") {
    auto p = writeTmp("pf_one.yaml", R"(
listen: "127.0.0.1:0"
access:
  levels: [default]
agents:
  cursor: { token: "t1" }
backends:
  - name: echo
    url: "http://127.0.0.1:9/mcp"
)");
    auto c = perfacet::YamlConfig::load(p);
    CHECK(c.agents.at(0).hasLevel);
    CHECK(c.agents.at(0).level == 0);
}

TEST_CASE("拼错 level 拒绝启动") {
    auto p = writeTmp("pf_bad_level.yaml", R"(
access:
  levels: [a, b]
agents:
  cursor: { token: "t1", level: nope }
backends:
  - name: echo
    url: "http://127.0.0.1:9/mcp"
    level: a
)");
    CHECK_THROWS_AS(perfacet::YamlConfig::load(p), std::runtime_error);
}

TEST_CASE("多档缺 level 拒绝启动") {
    auto p = writeTmp("pf_missing.yaml", R"(
access:
  levels: [a, b]
agents:
  cursor: { token: "t1" }
backends:
  - name: echo
    url: "http://127.0.0.1:9/mcp"
    level: a
)");
    CHECK_THROWS_AS(perfacet::YamlConfig::load(p), std::runtime_error);
}

TEST_CASE("backend 名含 __ 拒绝启动") {
    auto p = writeTmp("pf_dunder.yaml", R"(
access:
  levels: [default]
agents:
  cursor: { token: "t1" }
backends:
  - name: "echo__x"
    url: "http://127.0.0.1:9/mcp"
)");
    CHECK_THROWS_AS(perfacet::YamlConfig::load(p), std::runtime_error);
}

TEST_CASE("纯 admin 省略 level 允许") {
    auto p = writeTmp("pf_admin.yaml", R"(
access:
  levels: [a, b]
agents:
  ops: { token: "ta", admin: true }
  user: { token: "tu", level: a }
backends:
  - name: echo
    url: "http://127.0.0.1:9/mcp"
    level: a
)");
    auto c = perfacet::YamlConfig::load(p);
    bool found = false;
    for (const auto& a : c.agents) {
        if (a.id == "ops") {
            found = true;
            CHECK(a.admin);
            CHECK_FALSE(a.hasLevel);
        }
    }
    CHECK(found);
}
