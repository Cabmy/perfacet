#include "perfacet/health/ProbeHealth.h"
#include "detail/Time.h"
#include "perfacet/backend/HttpMcpBackend.h"
#include "perfacet/ir/ClientCaps.h"

#include <cstdio>

namespace perfacet {

ProbeHealth::ProbeHealth(Catalog& catalog, netlib::EventLoop* loop,
                         netlib::ThreadPool* pool, uint64_t intervalMs,
                         uint64_t degradedMs, int downAfter)
    : catalog_(&catalog), loop_(loop), pool_(pool), intervalMs_(intervalMs),
      degradedMs_(degradedMs), downAfter_(downAfter) {}

void ProbeHealth::applyResult(const std::string& name, bool ok, uint64_t latencyMs,
                              std::optional<ir::Json> tools) {
    auto& s = slots_[name];
    if (ok) {
        s.fails = 0;
        if (!s.ewmaInit) {
            s.ewma = latencyMs;
            s.ewmaInit = true;
        } else {
            s.ewma = (s.ewma * 7 + latencyMs) / 8;
        }
        s.state = (s.ewma >= degradedMs_) ? State::Degraded : State::Up;
    } else {
        s.fails++;
        if (s.fails >= downAfter_) s.state = State::Down;
    }
    if (cb_) cb_(name, s.state, ok ? tools : std::nullopt);
}

void ProbeHealth::probeAllBlocking() {
    for (const auto& name : catalog_->names()) {
        auto* e = catalog_->find(name);
        if (!e || !e->backend) continue;
        ir::BackendCall bc;
        bc.method = "tools/list";
        bc.params = ir::Json::object();
        bc.deadlineMs = nowMs() + 3000;
        const uint64_t t0 = nowMs();
        ir::Response r = static_cast<HttpMcpBackend*>(e->backend.get())->callBlocking(bc);
        applyResult(name, !r.isError && r.klass == ir::FailureClass::Ok, nowMs() - t0,
                    r.isError ? std::nullopt : std::optional<ir::Json>(r.body));
        std::fprintf(stderr, "[perfacet] probe %s %s (%s)\n", name.c_str(),
                     (!r.isError && r.klass == ir::FailureClass::Ok) ? "ok"
                                                                    : "fail",
                     ir::failureClassName(r.klass));
    }
}

void ProbeHealth::startTimer() {
    const double sec = static_cast<double>(intervalMs_) / 1000.0;
    timerId_ = loop_->runEvery(sec, [this]() {
        for (const auto& name : catalog_->names()) {
            auto* e = catalog_->find(name);
            if (!e || !e->backend) continue;
            ir::BackendCall bc;
            bc.method = "tools/list";
            bc.params = ir::Json::object();
            bc.deadlineMs = nowMs() + intervalMs_;
            e->backend->call(bc, [this, name](ir::Response r) {
                applyResult(name, !r.isError && r.klass == ir::FailureClass::Ok,
                            r.upstreamMs, r.isError ? std::nullopt
                                                    : std::optional<ir::Json>(r.body));
            });
        }
    });
}

Health::State ProbeHealth::state(const std::string& server) const {
    auto it = slots_.find(server);
    return it == slots_.end() ? State::Down : it->second.state;
}

uint64_t ProbeHealth::latencyEwmaMs(const std::string& server) const {
    auto it = slots_.find(server);
    return it == slots_.end() ? 0 : it->second.ewma;
}

std::vector<ProbeHealth::Snapshot> ProbeHealth::all() const {
    std::vector<Snapshot> out;
    for (const auto& kv : slots_) {
        Snapshot s;
        s.name = kv.first;
        s.state = kv.second.state;
        s.latencyEwmaMs = kv.second.ewma;
        out.push_back(s);
    }
    return out;
}

} // namespace perfacet
