#include "perfacet/policy/YamlConfig.h"

#include <yaml-cpp/yaml.h>

#include <stdexcept>
#include <unordered_set>

namespace perfacet {

namespace {

ir::FailureClass mustKlass(const std::string& name, const std::string& ctx) {
    auto k = ir::failureClassFromName(name);
    if (!k) throw std::runtime_error(ctx + " 未知 FailureClass: " + name);
    return *k;
}

} // namespace

YamlConfig YamlConfig::load(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("无法读取配置 ") + path + ": " + e.what());
    }
    if (!root || !root.IsMap()) {
        throw std::runtime_error("配置根必须是 mapping: " + path);
    }

    YamlConfig c;
    if (root["listen"]) c.listen = root["listen"].as<std::string>();
    if (root["workers"]) c.workers = root["workers"].as<int>();
    if (root["worker_queue_max"]) c.workerQueueMax = root["worker_queue_max"].as<std::size_t>();
    if (root["drain_timeout_ms"]) c.drainTimeoutMs = root["drain_timeout_ms"].as<uint64_t>();
    if (root["grants_path"]) c.grantsPath = root["grants_path"].as<std::string>();
    if (root["grant_refresh_ms"]) c.grantRefreshMs = root["grant_refresh_ms"].as<uint64_t>();
    if (root["list_ttl_ms"]) c.listTtlMs = root["list_ttl_ms"].as<uint64_t>();
    if (root["http"]) {
        auto h = root["http"];
        if (h["max_body_bytes"]) c.httpMaxBodyBytes = h["max_body_bytes"].as<std::size_t>();
        if (h["origin_allowlist"]) {
            c.originAllowlist.clear();
            for (const auto& n : h["origin_allowlist"]) {
                c.originAllowlist.push_back(n.as<std::string>());
            }
            if (c.originAllowlist.empty()) {
                throw std::runtime_error("http.origin_allowlist 不能为空");
            }
        }
    }

    if (!root["access"] || !root["access"]["levels"] || !root["access"]["levels"].IsSequence()) {
        throw std::runtime_error("缺少 access.levels");
    }
    std::vector<std::string> levels;
    for (const auto& n : root["access"]["levels"]) {
        levels.push_back(n.as<std::string>());
    }
    c.taxonomy = Taxonomy(std::move(levels));
    c.elevationMax = c.taxonomy.maxRank();
    c.elevationMaxName = c.taxonomy.nameOf(c.elevationMax);
    if (root["access"]["elevation"]) {
        c.elevationConfigured = true;
        auto el = root["access"]["elevation"];
        if (el["ttl_ms"]) c.elevationTtlMs = el["ttl_ms"].as<uint64_t>();
        if (el["max_level"]) {
            c.elevationMaxName = el["max_level"].as<std::string>();
            auto r = c.taxonomy.parse(c.elevationMaxName);
            if (!r) {
                throw std::runtime_error("elevation.max_level 不在 levels 中: " +
                                         c.elevationMaxName);
            }
            c.elevationMax = *r;
        }
    }

    if (!root["agents"] || !root["agents"].IsMap()) {
        throw std::runtime_error("缺少 agents");
    }
    const bool singleLevel = c.taxonomy.size() == 1;
    std::unordered_set<std::string> tokens;
    for (auto it = root["agents"].begin(); it != root["agents"].end(); ++it) {
        AgentCfg a;
        a.id = it->first.as<std::string>();
        auto node = it->second;
        if (!node["token"]) throw std::runtime_error("agent " + a.id + " 缺少 token");
        a.token = node["token"].as<std::string>();
        if (a.token.empty()) throw std::runtime_error("agent " + a.id + " token 为空");
        if (!tokens.insert(a.token).second) {
            throw std::runtime_error("重复 token（agent=" + a.id + "）");
        }
        a.admin = node["admin"] ? node["admin"].as<bool>() : false;
        const bool hasLevelKey = static_cast<bool>(node["level"]);
        if (a.admin && !hasLevelKey) {
            a.hasLevel = false;
        } else if (!hasLevelKey) {
            if (singleLevel) {
                a.hasLevel = true;
                a.level = 0;
                a.levelName = c.taxonomy.nameOf(0);
            } else {
                throw std::runtime_error("多档时 agent " + a.id + " 必须写 level");
            }
        } else {
            a.levelName = node["level"].as<std::string>();
            auto r = c.taxonomy.parse(a.levelName);
            if (!r) {
                throw std::runtime_error("agent " + a.id + " 的 level 不在 levels 中: " +
                                         a.levelName);
            }
            a.level = *r;
            a.hasLevel = true;
        }
        c.agents.push_back(std::move(a));
    }
    if (c.agents.empty()) throw std::runtime_error("agents 不能为空");

    if (!root["backends"] || !root["backends"].IsSequence()) {
        throw std::runtime_error("缺少 backends");
    }
    std::unordered_set<std::string> bnames;
    for (const auto& node : root["backends"]) {
        BackendCfg b;
        if (!node["name"]) throw std::runtime_error("backend 缺少 name");
        b.name = node["name"].as<std::string>();
        if (b.name.empty()) throw std::runtime_error("backend name 为空");
        if (b.name.find("__") != std::string::npos) {
            throw std::runtime_error("backend 名禁止含 __: " + b.name);
        }
        if (!bnames.insert(b.name).second) {
            throw std::runtime_error("backend 名重复: " + b.name);
        }
        if (!node["url"]) throw std::runtime_error("backend " + b.name + " 缺少 url");
        b.url = node["url"].as<std::string>();
        const bool hasLevelKey = static_cast<bool>(node["level"]);
        if (!hasLevelKey) {
            if (singleLevel) {
                b.meta.level = 0;
                b.levelName = c.taxonomy.nameOf(0);
            } else {
                throw std::runtime_error("多档时 backend " + b.name + " 必须写 level");
            }
        } else {
            b.levelName = node["level"].as<std::string>();
            auto r = c.taxonomy.parse(b.levelName);
            if (!r) {
                throw std::runtime_error("backend " + b.name + " 的 level 不在 levels 中: " +
                                         b.levelName);
            }
            b.meta.level = *r;
        }
        b.meta.secret = node["secret"] ? node["secret"].as<bool>() : false;
        if (node["idempotent_tools"]) {
            for (const auto& t : node["idempotent_tools"]) {
                b.meta.idempotentTools.push_back(t.as<std::string>());
            }
        }
        c.backends.push_back(std::move(b));
    }
    if (c.backends.empty()) throw std::runtime_error("backends 不能为空");

    if (root["health"]) {
        auto h = root["health"];
        if (h["interval_ms"]) c.healthIntervalMs = h["interval_ms"].as<uint64_t>();
        if (h["degraded_latency_ms"]) c.degradedLatencyMs = h["degraded_latency_ms"].as<uint64_t>();
        if (h["down_after_failures"]) c.downAfterFailures = h["down_after_failures"].as<int>();
    }
    if (root["circuit"]) {
        auto x = root["circuit"];
        if (x["open_after"]) c.circuitOpenAfter = x["open_after"].as<int>();
        if (x["cooldown_ms"]) c.circuitCooldownMs = x["cooldown_ms"].as<uint64_t>();
        if (x["half_open_probes"]) c.halfOpenProbes = x["half_open_probes"].as<int>();
    }
    if (root["retry"]) {
        auto r = root["retry"];
        if (r["max_attempts"]) c.retryMaxAttempts = r["max_attempts"].as<int>();
        if (r["retryable"]) {
            c.retryable.clear();
            for (const auto& n : r["retryable"]) {
                c.retryable.push_back(mustKlass(n.as<std::string>(), "retry.retryable"));
            }
        }
        if (r["never"]) {
            c.neverRetry.clear();
            for (const auto& n : r["never"]) {
                c.neverRetry.push_back(mustKlass(n.as<std::string>(), "retry.never"));
            }
        }
    }
    if (root["governor"]) {
        auto g = root["governor"];
        if (g["default"]) {
            auto d = g["default"];
            if (d["per_tool_concurrency"]) c.perToolConcurrency = d["per_tool_concurrency"].as<int>();
            if (d["per_principal_concurrency"]) {
                c.perPrincipalConcurrency = d["per_principal_concurrency"].as<int>();
            }
            if (d["queue_wait_ms"]) c.queueWaitMs = d["queue_wait_ms"].as<uint64_t>();
            if (d["rate_per_sec"]) c.ratePerSec = d["rate_per_sec"].as<int>();
        }
        if (g["tools"] && g["tools"].IsMap()) {
            for (auto it = g["tools"].begin(); it != g["tools"].end(); ++it) {
                const std::string key = it->first.as<std::string>();
                auto parsed = ir::ToolKey::parse(key);
                if (!parsed) {
                    throw std::runtime_error("governor.tools 键不是 ToolKey: " + key);
                }
                GovernorToolCfg tc;
                auto n = it->second;
                if (n["max_concurrency"]) tc.maxConcurrency = n["max_concurrency"].as<int>();
                if (n["queue_wait_ms"]) tc.queueWaitMs = n["queue_wait_ms"].as<uint64_t>();
                c.governorTools.emplace(key, tc);
            }
        }
    }
    if (root["tasks"]) {
        auto t = root["tasks"];
        if (t["promote_after_ms"]) c.promoteAfterMs = t["promote_after_ms"].as<uint64_t>();
        if (t["ttl_ms"]) c.taskTtlMs = t["ttl_ms"].as<uint64_t>();
        if (t["max"]) c.taskMax = t["max"].as<std::size_t>();
    }
    if (root["otel"]) {
        auto o = root["otel"];
        if (o["endpoint"]) c.otelEndpoint = o["endpoint"].as<std::string>();
        if (o["service_name"]) c.otelService = o["service_name"].as<std::string>();
        if (o["queue_max"]) c.otelQueueMax = o["queue_max"].as<std::size_t>();
    }
    if (root["audit"] && root["audit"]["path"]) {
        c.auditPath = root["audit"]["path"].as<std::string>();
    }
    return c;
}

} // namespace perfacet
