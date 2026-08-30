#include "perfacet/cli/main_cli.h"
#include "detail/Time.h"
#include "perfacet/Gateway.h"

#include <CLI/CLI.hpp>
#include <httplib.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace perfacet {
namespace {

Gateway* gGw = nullptr;
netlib::EventLoop* gLoop = nullptr;

void onSig(int) {
    if (gLoop && gGw) {
        gLoop->queueInLoop([]() {
            if (gGw) gGw->requestStop();
        });
    }
}

int runServe(const std::string& yaml) {
    YamlConfig cfg;
    try {
        cfg = YamlConfig::load(yaml);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[perfacet] 配置失败: %s\n", e.what());
        return 1;
    }
    netlib::EventLoop loop;
    Gateway gw(&loop, std::move(cfg));
    gGw = &gw;
    gLoop = &loop;
    std::signal(SIGINT, onSig);
    std::signal(SIGTERM, onSig);
    gw.start();
    loop.loop();
    gGw = nullptr;
    gLoop = nullptr;
    return 0;
}

int runGrantApprove(const std::string& yaml, const std::string& id, const std::string& agent,
                    const std::string& bump, uint64_t ttlOverride) {
    YamlConfig cfg;
    try {
        cfg = YamlConfig::load(yaml);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[perfacet] 配置失败: %s\n", e.what());
        return 1;
    }
    JsonlGrantStore store(cfg.grantsPath, &cfg.taxonomy, cfg.elevationMax, cfg.elevationTtlMs);
    store.refreshOnWorker();
    const uint64_t now = nowMs();
    bool ok = false;
    std::string outAgent = agent, outBump = bump, outId = id;
    if (!id.empty()) {
        ok = store.approveById(id, now, ttlOverride);
        auto snap = store.snapshot();
        auto it = snap->byId.find(id);
        if (it != snap->byId.end()) {
            outAgent = it->second.agent;
            outBump = it->second.bumpTo;
        }
    } else {
        auto r = cfg.taxonomy.parse(bump);
        if (!r || *r > cfg.elevationMax) {
            std::fprintf(stderr, "[perfacet] bump 非法或超过 max_level\n");
            return 1;
        }
        ok = store.approveDirect(agent, *r, bump, now);
    }
    if (!ok) {
        std::fprintf(stderr, "[perfacet] approve 失败\n");
        return 1;
    }
    nlohmann::json line{{"ts_ms", now},
                        {"event", "grant_approve"},
                        {"trace_id", ""},
                        {"principal", outAgent},
                        {"level", outBump},
                        {"tool", ""},
                        {"server", ""},
                        {"status", "approved"}};
    std::ofstream out(cfg.auditPath, std::ios::app);
    out << line.dump() << '\n';
    std::fprintf(stderr, "[perfacet] grant approved agent=%s bump=%s\n", outAgent.c_str(),
                 outBump.c_str());
    return 0;
}

int runStatus(const std::string& yaml) {
    YamlConfig cfg;
    try {
        cfg = YamlConfig::load(yaml);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[perfacet] 配置失败: %s\n", e.what());
        return 1;
    }
    YamlIdentityStore ident(cfg);
    const AgentCfg* admin = ident.adminAgent();
    if (!admin) {
        std::fprintf(stderr, "[perfacet] 配置中没有 admin: true 的 agent\n");
        return 1;
    }
    netlib::Endpoint ep = netlib::parseHostPort(cfg.listen);
    httplib::Client cli(ep.ip(), ep.port());
    httplib::Headers hdr{
        {"Authorization", std::string("Bearer ") + admin->token}, // PERFACET_LAYER_ALLOW
        {"Accept", "application/json"},
    };
    auto res = cli.Get("/upstreams", hdr);
    if (!res) {
        std::fprintf(stderr, "[perfacet] status 无法连接网关\n");
        return 1;
    }
    std::cout << res->body << std::endl;
    return res->status == 200 ? 0 : 1;
}

} // namespace

int runCli(int argc, char** argv) {
    CLI::App app{"Perfacet —— Multi-Agent Tool Gateway"};
    app.require_subcommand(1);
    int rc = 0;

    std::string yaml = "examples/perfacet.yaml";
    auto* serve = app.add_subcommand("serve", "前台跑网关");
    serve->add_option("-c,--config", yaml, "YAML 配置")->required();
    serve->callback([&]() { rc = runServe(yaml); });

    auto* grant = app.add_subcommand("grant", "提权审批");
    auto* approve = grant->add_subcommand("approve", "批准 pending Grant");
    std::string gid, agent, bump;
    uint64_t ttlOverride = 0;
    approve->add_option("-c,--config", yaml, "YAML 配置")->required();
    approve->add_option("--id", gid, "grantId");
    approve->add_option("--agent", agent, "agent id");
    approve->add_option("--bump", bump, "目标档位名");
    approve->add_option("--ttl-ms", ttlOverride, "覆盖 elevation.ttl_ms（演示到期用）");
    approve->callback([&]() {
        if (gid.empty() && (agent.empty() || bump.empty())) {
            rc = 1;
            std::fprintf(stderr, "[perfacet] 需要 --id 或 --agent + --bump\n");
            return;
        }
        rc = runGrantApprove(yaml, gid, agent, bump, ttlOverride);
    });

    auto* status = app.add_subcommand("status", "GET /upstreams");
    status->add_option("-c,--config", yaml, "YAML 配置")->required();
    status->callback([&]() { rc = runStatus(yaml); });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }
    return rc;
}

} // namespace perfacet
