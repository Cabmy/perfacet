#pragma once
// 一次加载整份 YAML。校验失败抛带名字的错误，进程以非 0 退出。
#include "perfacet/ir/Request.h"
#include "perfacet/policy/Taxonomy.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace perfacet {

struct AgentCfg {
    std::string id;
    std::string token;
    ir::Rank level = 0;
    bool hasLevel = false;
    bool admin = false;
    std::string levelName;
};

struct BackendCfg {
    std::string name;
    std::string url;
    ir::BackendMeta meta;
    std::string levelName;
};

struct GovernorToolCfg {
    int maxConcurrency = -1;     // <0 表示用 default
    uint64_t queueWaitMs = 0;    // 0 表示用 default
    enum class Status { Unvalidated, Active, Invalid } status =
        Status::Unvalidated;
};

struct YamlConfig {
    std::string listen = "127.0.0.1:8741";
    int workers = 4;
    uint64_t drainTimeoutMs = 3000;
    std::size_t httpMaxBodyBytes = 1 << 20;
    std::size_t workerQueueMax = 256;
    std::string grantsPath = "grants.jsonl";
    uint64_t grantRefreshMs = 100;
    uint64_t listTtlMs = 5000;
    std::vector<std::string> originAllowlist{"*"};

    Taxonomy taxonomy{std::vector<std::string>{"default"}};
    ir::Rank elevationMax = 0;
    std::string elevationMaxName;
    uint64_t elevationTtlMs = 900000;
    bool elevationConfigured = false;

    std::vector<AgentCfg> agents;
    std::vector<BackendCfg> backends;

    uint64_t healthIntervalMs = 5000;
    uint64_t degradedLatencyMs = 1000;
    int downAfterFailures = 3;

    int circuitOpenAfter = 5;
    uint64_t circuitCooldownMs = 30000;
    int halfOpenProbes = 1;

    int retryMaxAttempts = 2;
    std::vector<ir::FailureClass> retryable{
        ir::FailureClass::Timeout, ir::FailureClass::Unavailable,
        ir::FailureClass::Upstream};
    std::vector<ir::FailureClass> neverRetry{
        ir::FailureClass::Authz, ir::FailureClass::Cancelled,
        ir::FailureClass::Protocol, ir::FailureClass::Throttled,
        ir::FailureClass::Capability};

    int perToolConcurrency = 4;
    int perPrincipalConcurrency = 10;
    uint64_t queueWaitMs = 5000;
    int ratePerSec = 20; // YAML 占位，M1 不实现令牌桶
    std::unordered_map<std::string, GovernorToolCfg> governorTools;

    uint64_t promoteAfterMs = 2000;
    uint64_t taskTtlMs = 3600000;
    std::size_t taskMax = 10000;

    std::string otelEndpoint = "http://127.0.0.1:4318/v1/traces";
    std::string otelService = "perfacet";
    std::size_t otelQueueMax = 1024;

    std::string auditPath = "audit.jsonl";

    static YamlConfig load(const std::string& path);
};

} // namespace perfacet
