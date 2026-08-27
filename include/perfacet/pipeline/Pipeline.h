#pragma once
#include "perfacet/audit/JsonlAuditLog.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/catalog/FacetView.h"
#include "perfacet/catalog/IndexRefresher.h"
#include "perfacet/catalog/ToolIndex.h"
#include "perfacet/govern/Governor.h"
#include "perfacet/health/CountCircuit.h"
#include "perfacet/health/Health.h"
#include "perfacet/health/RetryPolicy.h"
#include "perfacet/observe/Counters.h"
#include "perfacet/observe/Tracer.h"
#include "perfacet/pipeline/InFlight.h"
#include "perfacet/policy/JsonlGrantStore.h"
#include "perfacet/policy/Policy.h"
#include "perfacet/task/MemTaskStore.h"

#include "netlib/EventLoop.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace perfacet {

class Call;

class Pipeline {
public:
    struct Deps {
        netlib::EventLoop* loop = nullptr;
        Policy* policy = nullptr;
        Governor* governor = nullptr;
        InFlight* inflight = nullptr;
        CountCircuit* circuit = nullptr;
        Health* health = nullptr;
        Catalog* catalog = nullptr;
        ToolIndex* index = nullptr;
        FacetView* facet = nullptr;
        MemTaskStore* tasks = nullptr;
        Tracer* tracer = nullptr;
        JsonlAuditLog* audit = nullptr;
        Counters* counters = nullptr;
        RetryPolicy* retry = nullptr;
        JsonlGrantStore* grants = nullptr;
        const class YamlConfig* cfg = nullptr;
    };

    explicit Pipeline(Deps d);

    void handle(ir::Request req, std::function<void(ir::Response)> onDone);

    void cancelTask(const std::string& taskId, const ir::Principal& who,
                    std::function<void(ir::Response)> onDone);
    std::optional<Task> getTask(const std::string& taskId,
                                const ir::Principal& who) const;

    void requestStop();

private:
    friend class Call;
    void handleCall(ir::Request req, std::function<void(ir::Response)> onDone);
    void handleBuiltin(ir::Request req, const ir::ToolKey& key,
                       std::function<void(ir::Response)> onDone);
    ir::Json listBody(const ir::Principal& who) const;
    void audit(const char* event, const ir::Request& req, ir::FailureClass k,
               std::string_view tool = {});
    std::shared_ptr<Call> liveCall(const std::string& inflightId);

    Deps d_;
    std::unordered_map<std::string, std::weak_ptr<Call>> byTask_;
    std::unordered_map<std::string, std::weak_ptr<Call>> live_;
    bool stopping_ = false;
};

class Call : public std::enable_shared_from_this<Call> {
public:
    Call(Pipeline* p, Governor::Permit permit, ir::Request req,
         std::function<void(ir::Response)> onDone, std::string inflightId,
         std::string paramsHash);
    ~Call();

    void startUpstream();
    void cancelWait();
    void attachTask(std::string taskId);
    const std::string& taskId() const { return taskId_; }

private:
    void respond(ir::Response r);
    void onUpstream(ir::Response r);
    void onPromote();
    void armPromote();
    void fireAttempt();
    ir::BackendCall makeBackendCall() const;

    Pipeline* p_;
    Governor::Permit permit_;
    ir::Request req_;
    std::function<void(ir::Response)> onDone_;
    ir::TraceContext trace_;
    ir::TraceContext upTrace_;
    bool upSpanLive_ = false;
    bool responded_ = false;
    bool upstreamDone_ = false;
    bool cancelled_ = false;
    netlib::TimerId promoteTimer_ = 0;
    std::string inflightId_;
    std::string paramsHash_;
    std::string taskId_;
    ir::ToolKey key_;
    int attempt_ = 0;
    uint64_t t0_ = 0;
};

} // namespace perfacet
