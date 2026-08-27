#pragma once
#include "perfacet/backend/Backend.h"
#include "perfacet/govern/Governor.h"
#include "perfacet/health/Health.h"
#include "perfacet/ir/Request.h"
#include "perfacet/observe/Tracer.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace perfacet::ir {

struct PrincipalForge {
    static Principal make(std::string agentId, Rank level = 0, bool hasLevel = true,
                          bool admin = false, std::string levelName = {},
                          Rank grantBump = 0) {
        Principal p;
        p.agentId = std::move(agentId);
        p.level = level;
        p.hasLevel = hasLevel;
        p.admin = admin;
        p.levelName = std::move(levelName);
        p.grantBump = grantBump;
        return p;
    }
};

} // namespace perfacet::ir

namespace perfacet::test {

inline ir::Principal testWho(std::string id, ir::Rank level = 0, bool hasLevel = true,
                             bool admin = false, std::string levelName = {}) {
    return ir::PrincipalForge::make(std::move(id), level, hasLevel, admin,
                                    std::move(levelName));
}

inline ir::Request testReq(ir::Principal who) { return ir::Request{std::move(who)}; }

class NullTracer : public Tracer {
public:
    ir::TraceContext start(const ir::Request& req, const char*) override {
        ir::TraceContext t = req.trace;
        if (t.traceId.empty()) t.traceId = "t";
        if (t.spanId.empty()) t.spanId = "s";
        return t;
    }
    void set(const ir::TraceContext&, const char*, const std::string&) override {}
    void end(const ir::TraceContext&, ir::FailureClass, uint64_t) override {}
};

class StubHealth : public Health {
public:
    State st = State::Up;
    State state(const std::string&) const override { return st; }
    uint64_t latencyEwmaMs(const std::string&) const override { return 1; }
};

class CountingBackend : public Backend {
public:
    std::atomic<int> calls{0};
    ir::Response canned;
    CountingBackend() {
        canned.klass = ir::FailureClass::Ok;
        canned.body = ir::callToolText("ok", false);
    }
    void call(const ir::BackendCall&, std::function<void(ir::Response)> cb) override {
        calls.fetch_add(1);
        cb(canned);
    }
};

class CountingGovernor : public Governor {
public:
    std::atomic<int> acquires{0};
    void acquire(const ir::Principal&, const ir::ToolKey&, uint64_t,
                 std::function<void(Admit, Permit)> onAdmit) override {
        acquires.fetch_add(1);
        onAdmit(Admit::Go, Permit{});
    }

protected:
    void releaseSlot(const ir::ToolKey&, std::string_view) override {}
};

class HoldingBackend : public Backend {
public:
    void call(const ir::BackendCall&, std::function<void(ir::Response)> cb) override {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push_back(std::move(cb));
    }
    std::size_t pending() const {
        std::lock_guard<std::mutex> lk(mu_);
        return pending_.size();
    }
    void flushOk() {
        ir::Response r;
        r.klass = ir::FailureClass::Ok;
        r.body = ir::callToolText("ok", false);
        flush(std::move(r));
    }
    void flush(ir::Response r) {
        std::vector<std::function<void(ir::Response)>> cbs;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cbs.swap(pending_);
        }
        for (auto& cb : cbs) cb(r);
    }

private:
    mutable std::mutex mu_;
    std::vector<std::function<void(ir::Response)>> pending_;
};

} // namespace perfacet::test
