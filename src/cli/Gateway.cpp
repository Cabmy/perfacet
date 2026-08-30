#include "perfacet/Gateway.h"
#include "perfacet/backend/HttpMcpBackend.h"

#include <functional>
#include <stdexcept>

namespace perfacet {

Gateway::Gateway(netlib::EventLoop* loop, YamlConfig cfg)
    : cfg_(std::move(cfg)), loop_(loop),
      workers_(static_cast<size_t>(cfg_.workers), cfg_.workerQueueMax),
      identity_(cfg_), grants_(cfg_.grantsPath, &cfg_.taxonomy, cfg_.elevationMax,
                              cfg_.elevationTtlMs),
      policy_(catalog_), facet_(index_, policy_), governor_(cfg_, loop, &counters_),
      circuit_(cfg_.circuitOpenAfter, cfg_.circuitCooldownMs, cfg_.halfOpenProbes, loop),
      retry_(cfg_), tasks_(cfg_.taskMax), inflight_(&counters_),
      tracer_(cfg_, &workers_, &counters_),
      audit_(cfg_.auditPath, &workers_),
      health_(catalog_, loop, &workers_, cfg_.healthIntervalMs, cfg_.degradedLatencyMs,
              cfg_.downAfterFailures),
      refresher_(index_),
      pipeline_(Pipeline::Deps{loop, &policy_, &governor_, &inflight_, &circuit_, &health_,
                               &catalog_, &index_, &facet_, &tasks_, &tracer_, &audit_,
                               &counters_, &retry_, &grants_, &cfg_}) {
    grants_.setPost([this](std::function<void()> f) {
        if (workers_.full()) throw std::runtime_error("worker queue full");
        workers_.add(std::move(f));
    });
    for (const auto& b : cfg_.backends) {
        CatalogEntry e;
        e.name = b.name;
        e.url = b.url;
        e.meta = b.meta;
        e.backend = std::make_unique<HttpMcpBackend>(b.url, loop_, &workers_);
        catalog_.add(std::move(e));
    }
    refresher_.setOnListed(
        [this](const std::string& backend, const std::vector<std::string>& tools) {
            governor_.onToolsListed(backend, tools);
        });
    health_.setCallback([this](const std::string& server, Health::State st,
                               std::optional<ir::Json> tools) {
        refresher_.onProbeResult(server, st, tools);
        if (tools.has_value()) circuit_.onProbeSuccess(server);
    });
    grants_.setOnExpire([this](const GrantRecord& g) {
        AuditEvent e;
        e.event = "grant_expire";
        e.principal = g.agent;
        e.level = g.bumpTo;
        e.status = "expired";
        audit_.emit(std::move(e));
    });
    http_ = std::make_unique<HttpMcp>(loop_, cfg_, identity_, grants_, pipeline_, &health_,
                                      &counters_, &catalog_, &governor_, &audit_);
}

Gateway::~Gateway() = default;

void Gateway::start() {
    grants_.refreshOnWorker();
    health_.probeAllBlocking();
    http_->startListen();
    health_.startTimer();
    const double gsec = static_cast<double>(cfg_.grantRefreshMs) / 1000.0;
    grantTimer_ = loop_->runEvery(gsec <= 0 ? 0.1 : gsec, [this]() {
        try {
            workers_.add([this]() { grants_.refreshOnWorker(); });
        } catch (...) {
        }
    });
}

void Gateway::requestStop() {
    http_->pauseAccept();
    http_->setStopping();
    pipeline_.requestStop();
    governor_.rejectAllQueued();
    const double sec = static_cast<double>(cfg_.drainTimeoutMs) / 1000.0;
    drainTimer_ = loop_->runAfter(sec, [this]() { loop_->quit(); });
}

} // namespace perfacet
