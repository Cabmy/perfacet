#pragma once
// 只做 HTTP ↔ IR。禁止在此编排 Policy / Governor / Circuit / Backend / InFlight / promote。
#include "perfacet/audit/JsonlAuditLog.h"
#include "perfacet/pipeline/Pipeline.h"
#include "perfacet/policy/JsonlGrantStore.h"
#include "perfacet/policy/YamlConfig.h"
#include "perfacet/policy/YamlIdentityStore.h"

#include "netlib/EventLoop.h"
#include "netlib/TcpServer.h"

#include <memory>
#include <string>

namespace perfacet {

class ProbeHealth;
class Counters;
class Catalog;
class LocalGovernor;

class HttpMcp {
public:
    HttpMcp(netlib::EventLoop* loop, const YamlConfig& cfg,
            YamlIdentityStore& identity, JsonlGrantStore& grants, Pipeline& pipeline,
            ProbeHealth* health, Counters* counters, Catalog* catalog,
            LocalGovernor* governor, JsonlAuditLog* audit);

    void startListen();
    void setStopping();
    bool stopping() const { return stopping_; }
    uint16_t port() const { return server_ ? server_->listenPort() : 0; }

private:
    void onMessage(const netlib::TcpConnectionPtr& conn, netlib::Buffer& buf);
    void onConn(const netlib::TcpConnectionPtr& conn);

    netlib::EventLoop* loop_;
    const YamlConfig* cfg_;
    YamlIdentityStore* identity_;
    JsonlGrantStore* grants_;
    Pipeline* pipeline_;
    ProbeHealth* health_;
    Counters* counters_;
    Catalog* catalog_;
    LocalGovernor* governor_;
    JsonlAuditLog* audit_;
    std::unique_ptr<netlib::TcpServer> server_;
    bool stopping_ = false;
    bool listening_ = false;
};

} // namespace perfacet
