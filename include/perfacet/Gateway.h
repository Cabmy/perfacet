#pragma once
#include "perfacet/audit/JsonlAuditLog.h"
#include "perfacet/backend/HttpMcpBackend.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/catalog/FacetView.h"
#include "perfacet/catalog/IndexRefresher.h"
#include "perfacet/catalog/ToolIndex.h"
#include "perfacet/frontend/HttpMcp.h"
#include "perfacet/govern/LocalGovernor.h"
#include "perfacet/health/CountCircuit.h"
#include "perfacet/health/ProbeHealth.h"
#include "perfacet/health/RetryPolicy.h"
#include "perfacet/observe/Counters.h"
#include "perfacet/observe/OtlpHttpJsonTracer.h"
#include "perfacet/pipeline/InFlight.h"
#include "perfacet/pipeline/Pipeline.h"
#include "perfacet/policy/JsonlGrantStore.h"
#include "perfacet/policy/RankPolicy.h"
#include "perfacet/policy/YamlConfig.h"
#include "perfacet/policy/YamlIdentityStore.h"
#include "perfacet/task/MemTaskStore.h"

#include "netlib/EventLoop.h"
#include "netlib/ThreadPool.h"

#include <memory>

namespace perfacet {

class Gateway {
public:
    Gateway(netlib::EventLoop* loop, YamlConfig cfg);
    ~Gateway();

    void start(); // 首轮 probe 后再 listen
    void requestStop();

    uint16_t port() const { return http_ ? http_->port() : 0; }
    const YamlConfig& config() const { return cfg_; }
    YamlIdentityStore& identity() { return identity_; }
    JsonlGrantStore& grants() { return grants_; }
    Counters& counters() { return counters_; }

private:
    YamlConfig cfg_;
    netlib::EventLoop* loop_;
    netlib::ThreadPool workers_;
    YamlIdentityStore identity_;
    JsonlGrantStore grants_;
    Catalog catalog_;
    ToolIndex index_;
    RankPolicy policy_;
    FacetView facet_;
    Counters counters_;
    LocalGovernor governor_;
    CountCircuit circuit_;
    RetryPolicy retry_;
    MemTaskStore tasks_;
    InFlight inflight_;
    OtlpHttpJsonTracer tracer_;
    JsonlAuditLog audit_;
    ProbeHealth health_;
    IndexRefresher refresher_;
    Pipeline pipeline_;
    std::unique_ptr<HttpMcp> http_;
    netlib::TimerId grantTimer_ = 0;
    netlib::TimerId drainTimer_ = 0;
};

} // namespace perfacet
