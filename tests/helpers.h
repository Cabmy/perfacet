#pragma once
#include "perfacet/backend/Backend.h"
#include "perfacet/govern/Governor.h"
#include "perfacet/health/Health.h"
#include "perfacet/observe/Tracer.h"

#include <atomic>
#include <functional>
#include <string>

namespace perfacet::test {

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

} // namespace perfacet::test
