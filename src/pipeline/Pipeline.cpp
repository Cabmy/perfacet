#include "perfacet/pipeline/Pipeline.h"
#include "detail/Random.h"
#include "detail/Time.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/ir/ClientCaps.h"
#include "perfacet/policy/YamlConfig.h"

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace perfacet {

namespace {

std::string paramsHashOf(const ir::Json& params) {
    if (!params.is_object()) return params.dump();
    uint64_t h = 14695981039346656037ULL;
    auto mix = [&](std::string_view s) {
        for (unsigned char c : s) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ULL;
        }
    };
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (it.key() == "_meta") continue;
        mix(it.key());
        mix(it.value().dump());
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
    return out;
}

std::string summaryOf(const ir::Json& params) {
    ir::Json p = params;
    if (p.is_object() && p.contains("_meta")) p.erase("_meta");
    auto s = p.dump();
    if (s.size() > 200) s = s.substr(0, 200) + "...";
    return s;
}

ir::Json missingTasksError() {
    return ir::jsonRpcError(
        -32021, "Missing required client capability",
        ir::Json{{"requiredCapabilities",
                  ir::Json{{"extensions", ir::Json{{ir::kTasksExt, ir::Json::object()}}}}}});
}

ir::Json unknownTool() { return ir::callToolText("unknown tool", true); }

ir::Json denyTool() { return ir::callToolText("not allowed", true); }

ir::Json createTaskResult(const Task& t) {
    return ir::Json{
        {"resultType", "task"},
        {"taskId", t.taskId},
        {"status", t.status},
        {"createdAt", iso8601Utc(t.createdAtMs)},
        {"lastUpdatedAt", iso8601Utc(t.lastUpdatedAtMs)},
        {"ttlMs", t.ttlMs},
        {"pollIntervalMs", 500},
    };
}

} // namespace

Pipeline::Pipeline(Deps d) : d_(d) {}

void Pipeline::requestStop() {
    stopping_ = true;
    for (auto& kv : live_) {
        if (auto c = kv.second.lock()) c->cancelWait();
    }
}

std::shared_ptr<Call> Pipeline::liveCall(const std::string& inflightId) {
    auto it = live_.find(inflightId);
    if (it == live_.end()) return nullptr;
    return it->second.lock();
}

void Pipeline::audit(const char* event, const ir::Request& req, ir::FailureClass k,
                     std::string_view tool) {
    if (!d_.audit) return;
    AuditEvent e;
    e.event = event;
    e.traceId = req.trace.traceId;
    e.principal = req.who.agentId;
    e.level = req.who.levelName;
    e.tool = tool.empty() ? req.name : std::string(tool);
    auto key = ir::ToolKey::parse(e.tool);
    e.server = key ? key->backend : "";
    e.status = ir::failureClassName(k);
    d_.audit->emit(std::move(e));
}

ir::Json Pipeline::listBody(const ir::Principal& who) const {
    ir::Json tools = d_.facet->listTools(who);
    uint64_t ttl = d_.cfg->listTtlMs;
    if (d_.index->cold()) ttl = 0;
    if (d_.grants) {
        const uint64_t rem = d_.grants->shortestRemainingMs(who.agentId, nowMs());
        if (rem != UINT64_MAX && rem < ttl) ttl = rem;
    }
    return ir::Json{{"tools", std::move(tools)}, {"ttlMs", ttl}, {"cacheScope", "private"}};
}

void Pipeline::handle(ir::Request req, std::function<void(ir::Response)> onDone) {
    const uint64_t t0 = nowMs();
    ir::TraceContext total;
    if (d_.tracer) {
        total = d_.tracer->start(req, "total");
        req.trace = total;
        req.trace = d_.tracer->start(req, "gateway");
    }
    auto finish = [this, t0, onDone, req, total](ir::Response r) mutable {
        r.gatewayMs = nowMs() - t0;
        if (r.upstreamId.empty()) r.upstreamId = req.upstreamId;
        if (d_.tracer) {
            d_.tracer->end(req.trace, r.klass, r.gatewayMs);
            d_.tracer->end(total, r.klass, r.gatewayMs);
        }
        onDone(std::move(r));
    };

    if (req.method == "server/discover") {
        ir::Response r;
        r.body = ir::Json{
            {"protocolVersion", ir::kProtocolVersion},
            {"capabilities",
             ir::Json{{"tools", ir::Json{{"listChanged", false}}},
                      {"extensions", ir::Json{{ir::kTasksExt, ir::Json::object()}}}}},
            {"serverInfo", ir::Json{{"name", "perfacet"}, {"version", "0.1.0"}}},
            {"ttlMs", 60000},
            {"cacheScope", "private"},
        };
        finish(std::move(r));
        return;
    }
    if (req.method == "tools/list") {
        ir::Response r;
        r.body = listBody(req.who);
        finish(std::move(r));
        return;
    }
    if (req.method == "tasks/get") {
        auto t = getTask(req.name, req.who);
        ir::Response r;
        if (!t) {
            r.isError = true;
            r.klass = ir::FailureClass::Authz;
            r.body = ir::jsonRpcError(-32000, "task not found");
            r.httpStatus = 200;
            finish(std::move(r));
            return;
        }
        ir::Json body{
            {"taskId", t->taskId},
            {"status", t->status},
            {"createdAt", iso8601Utc(t->createdAtMs)},
            {"lastUpdatedAt", iso8601Utc(t->lastUpdatedAtMs)},
            {"ttlMs", t->ttlMs},
        };
        if (t->status == "completed") body["result"] = t->resultOrError;
        if (t->status == "failed") body["error"] = t->resultOrError;
        r.body = std::move(body);
        finish(std::move(r));
        return;
    }
    if (req.method == "tasks/cancel") {
        cancelTask(req.name, req.who, std::move(finish));
        return;
    }
    if (req.method == "tools/call") {
        handleCall(std::move(req), std::move(finish));
        return;
    }
    ir::Response r;
    r.isError = true;
    r.klass = ir::FailureClass::Protocol;
    r.httpStatus = 404;
    r.body = ir::jsonRpcError(-32601, "Method not found");
    finish(std::move(r));
}

std::optional<Task> Pipeline::getTask(const std::string& taskId,
                                      const ir::Principal& who) const {
    auto t = d_.tasks->get(taskId);
    if (!t) return std::nullopt;
    if (t->agentId != who.agentId) return std::nullopt;
    return t;
}

void Pipeline::cancelTask(const std::string& taskId, const ir::Principal& who,
                          std::function<void(ir::Response)> onDone) {
    auto t = getTask(taskId, who);
    ir::Response r;
    if (!t) {
        r.isError = true;
        r.klass = ir::FailureClass::Authz;
        r.body = ir::jsonRpcError(-32000, "task not found");
        onDone(std::move(r));
        return;
    }
    d_.tasks->updateStatus(taskId, "cancelled", ir::Json(), nowMs());
    auto it = byTask_.find(taskId);
    if (it != byTask_.end()) {
        if (auto c = it->second.lock()) c->cancelWait();
    }
    r.body = ir::Json{{"taskId", taskId}, {"status", "cancelled"}};
    onDone(std::move(r));
}

void Pipeline::handleBuiltin(ir::Request req, const ir::ToolKey& key,
                             std::function<void(ir::Response)> onDone) {
    ir::Response r;
    if (key.tool == "request_elevation") {
        if (!req.who.hasLevel) {
            r.body = ir::callToolText("pure admin cannot elevate", true);
            r.klass = ir::FailureClass::Authz;
            audit("deny", req, r.klass, key.str());
            onDone(std::move(r));
            return;
        }
        ir::Json args = req.params.contains("arguments") ? req.params["arguments"] : req.params;
        const std::string bumpTo = args.value("bump_to", "");
        auto rank = d_.cfg->taxonomy.parse(bumpTo);
        if (!rank || *rank > d_.cfg->elevationMax) {
            r.body = ir::callToolText("invalid bump_to", true);
            r.klass = ir::FailureClass::Authz;
            onDone(std::move(r));
            return;
        }
        const std::string id =
            d_.grants->appendPending(req.who.agentId, bumpTo, *rank, nowMs());
        r.body = ir::callToolText(
            ir::Json{{"grantId", id}, {"status", "pending"}}.dump(), false);
        audit("ok", req, ir::FailureClass::Ok, key.str());
        onDone(std::move(r));
        return;
    }
    if (key.tool == "upstream_status") {
        if (!req.who.admin) {
            r.body = denyTool();
            r.klass = ir::FailureClass::Authz;
            audit("deny", req, r.klass, key.str());
            onDone(std::move(r));
            return;
        }
        ir::Json arr = ir::Json::array();
        for (const auto& name : d_.catalog->names()) {
            arr.push_back(ir::Json{
                {"name", name},
                {"state", healthStateName(d_.health->state(name))},
                {"latency_ewma_ms", d_.health->latencyEwmaMs(name)},
            });
        }
        r.body = ir::callToolText(arr.dump(), false);
        onDone(std::move(r));
        return;
    }
    r.body = unknownTool();
    r.klass = ir::FailureClass::Authz;
    audit("deny", req, r.klass, key.str());
    onDone(std::move(r));
}

void Pipeline::handleCall(ir::Request req, std::function<void(ir::Response)> onDone) {
    auto key = ir::ToolKey::parse(req.name);
    if (!key) {
        ir::Response r;
        r.body = unknownTool();
        r.klass = ir::FailureClass::Authz;
        audit("deny", req, r.klass);
        onDone(std::move(r));
        return;
    }

    const Decision dec = d_.policy->authorizeCall(req.who, *key);
    if (dec != Decision::Allow) {
        ir::Response r;
        r.klass = ir::FailureClass::Authz;
        r.body = (dec == Decision::Unknown) ? unknownTool() : denyTool();
        audit("deny", req, r.klass, key->str());
        onDone(std::move(r));
        return;
    }

    if (key->backend == "perfacet") {
        handleBuiltin(std::move(req), *key, std::move(onDone));
        return;
    }

    const std::string hash = paramsHashOf(req.params);
    auto hit = d_.inflight->lookup(req.who.agentId, *key, hash);
    std::string confirm;
    if (req.meta.is_object() && req.meta.contains(ir::kConfirmKey)) {
        confirm = req.meta[ir::kConfirmKey].get<std::string>();
    }

    if (hit) {
        const bool match = !confirm.empty() && confirm == ("if_" + hit->inflightId);
        if (match) {
            if (d_.counters) d_.counters->inflightConfirm.fetch_add(1);
        } else {
            if (d_.counters) d_.counters->inflightHit.fetch_add(1);
            if (d_.tracer) d_.tracer->set(req.trace, "inflight_hit", "true");
            if (d_.tracer) d_.tracer->set(req.trace, "inflight_id", hit->inflightId);
            audit("inflight_hit", req, ir::FailureClass::Throttled, key->str());
            ir::Response r;
            if (req.caps.tasks) {
                std::string tid = hit->taskId.value_or("");
                auto orig = liveCall(hit->inflightId);
                if (tid.empty() && orig && !orig->taskId().empty()) tid = orig->taskId();
                if (tid.empty()) {
                    Task t{req.who};
                    t.taskId = "tsk_" + randomHex(8);
                    t.agentId = req.who.agentId;
                    t.key = *key;
                    t.status = "working";
                    t.createdAtMs = hit->startedAtMs;
                    t.lastUpdatedAtMs = nowMs();
                    t.ttlMs = d_.cfg->taskTtlMs;
                    t.owner = req.who;
                    if (!d_.tasks->insert(t)) {
                        r.klass = ir::FailureClass::Unavailable;
                        r.body = ir::callToolText("task store full", true);
                        onDone(std::move(r));
                        return;
                    }
                    if (orig) orig->attachTask(t.taskId);
                    else d_.inflight->setTaskId(hit->inflightId, t.taskId);
                    tid = t.taskId;
                }
                auto t = d_.tasks->get(tid);
                if (!t) {
                    r.klass = ir::FailureClass::Unavailable;
                    r.body = ir::callToolText("task not found", true);
                    onDone(std::move(r));
                    return;
                }
                if (d_.tracer) d_.tracer->set(req.trace, "task_id", tid);
                r.body = createTaskResult(*t);
                onDone(std::move(r));
                return;
            }
            r.klass = ir::FailureClass::Throttled;
            r.body = ir::callToolText(
                "in-flight call still running for " + key->str() +
                    "; params=" + hit->paramsSummary + "; elapsed_ms=" +
                    std::to_string(nowMs() - hit->startedAtMs) +
                    "; confirm with _meta[\"" + std::string(ir::kConfirmKey) +
                    "\"]=\"if_" + hit->inflightId + "\"",
                true);
            onDone(std::move(r));
            return;
        }
    }

    if (stopping_) {
        ir::Response r;
        r.klass = ir::FailureClass::Unavailable;
        r.httpStatus = 503;
        r.isError = true;
        r.body = ir::jsonRpcError(-32000, "draining");
        onDone(std::move(r));
        return;
    }

    const uint64_t waitDeadline = nowMs() + [&]() {
        auto it = d_.cfg->governorTools.find(key->str());
        if (it != d_.cfg->governorTools.end() && it->second.queueWaitMs > 0)
            return it->second.queueWaitMs;
        return d_.cfg->queueWaitMs;
    }();

    ir::Request reqCopy = req;
    ir::ToolKey k = *key;
    d_.governor->acquire(
        req.who, k, waitDeadline,
        [this, req = std::move(reqCopy), k, hash, onDone = std::move(onDone)](
            Governor::Admit admit, Governor::Permit permit) mutable {
            if (admit != Governor::Admit::Go) {
                if (d_.counters) d_.counters->throttled.fetch_add(1);
                audit("throttled", req, ir::FailureClass::Throttled, k.str());
                ir::Response r;
                r.klass = ir::FailureClass::Throttled;
                r.body = ir::callToolText("throttled", true);
                onDone(std::move(r));
                return;
            }
            const uint64_t now = nowMs();
            if (d_.circuit->isOpen(k.backend, now) ||
                d_.health->state(k.backend) == Health::State::Down) {
                if (d_.circuit->isOpen(k.backend, now)) {
                    if (d_.counters) d_.counters->circuitOpen.fetch_add(1);
                    audit("circuit_open", req, ir::FailureClass::Unavailable, k.str());
                }
                ir::Response r;
                r.klass = ir::FailureClass::Unavailable;
                r.body = ir::callToolText("upstream unavailable", true);
                onDone(std::move(r));
                return; // Permit 析构归还
            }
            const std::string iid = d_.inflight->insert(req.who.agentId, k, hash,
                                                        summaryOf(req.params), now);
            auto call = std::make_shared<Call>(this, std::move(permit), std::move(req),
                                               std::move(onDone), iid, hash);
            call->startUpstream();
        });
}

Call::Call(Pipeline* p, Governor::Permit permit, ir::Request req,
           std::function<void(ir::Response)> onDone, std::string inflightId,
           std::string paramsHash)
    : p_(p), permit_(std::move(permit)), req_(std::move(req)), onDone_(std::move(onDone)),
      inflightId_(std::move(inflightId)), paramsHash_(std::move(paramsHash)), t0_(nowMs()) {
    auto k = ir::ToolKey::parse(req_.name);
    if (k) key_ = *k;
    trace_ = req_.trace;
}

Call::~Call() {
    if (promoteTimer_ && p_->d_.loop) p_->d_.loop->cancel(promoteTimer_);
    if (upSpanLive_ && p_->d_.tracer) {
        p_->d_.tracer->end(upTrace_, ir::FailureClass::Cancelled, nowMs() - t0_);
        upSpanLive_ = false;
    }
    if (!responded_) {
        ir::Response r;
        r.klass = ir::FailureClass::Cancelled;
        r.body = ir::callToolText("cancelled; remote may still run", true);
        respond(std::move(r));
    }
    if (p_->d_.inflight && !inflightId_.empty()) p_->d_.inflight->erase(inflightId_);
    if (!taskId_.empty()) p_->byTask_.erase(taskId_);
    if (!inflightId_.empty()) p_->live_.erase(inflightId_);
}

void Call::attachTask(std::string id) {
    taskId_ = std::move(id);
    p_->byTask_[taskId_] = shared_from_this();
    p_->d_.inflight->setTaskId(inflightId_, taskId_);
    if (p_->d_.tracer) p_->d_.tracer->set(trace_, "task_id", taskId_);
}

void Call::respond(ir::Response r) {
    if (responded_) return;
    responded_ = true;
    r.gatewayMs = nowMs() - t0_;
    if (!r.isError && r.klass == ir::FailureClass::Ok && req_.method == "tools/call") {
        p_->audit("ok", req_, ir::FailureClass::Ok, key_.str().empty() ? req_.name : key_.str());
    }
    if (onDone_) onDone_(std::move(r));
}

void Call::cancelWait() {
    cancelled_ = true;
    if (!responded_) {
        ir::Response r;
        r.klass = ir::FailureClass::Cancelled;
        r.body = ir::callToolText("cancelled; remote may still run", true);
        respond(std::move(r));
    }
    if (!taskId_.empty()) {
        p_->d_.tasks->updateStatus(taskId_, "cancelled", ir::Json(), nowMs());
    }
}

ir::BackendCall Call::makeBackendCall() const {
    ir::BackendCall bc;
    bc.method = "tools/call";
    bc.name = key_.tool;
    ir::Json params = req_.params.is_object() ? req_.params : ir::Json::object();
    params.erase("_meta");
    params["name"] = key_.tool;
    bc.params = std::move(params);
    ir::Json meta = req_.meta.is_object() ? req_.meta : ir::Json::object();
    meta.erase(ir::kConfirmKey);
    meta[ir::kMetaProtocol] = ir::kProtocolVersion;
    if (!meta.contains(ir::kMetaCaps)) meta[ir::kMetaCaps] = ir::Json::object();
    bc.meta = std::move(meta);
    bc.deadlineMs = req_.deadlineMs;
    bc.trace = trace_;
    return bc;
}

void Call::armPromote() {
    uint64_t ms = p_->d_.cfg->promoteAfterMs;
    if (p_->d_.health->state(key_.backend) == Health::State::Degraded) ms /= 2;
    if (ms == 0) ms = 1;
    promoteTimer_ = p_->d_.loop->runAfter(static_cast<double>(ms) / 1000.0,
                                          [self = shared_from_this()]() { self->onPromote(); });
}

void Call::onPromote() {
    if (responded_ || cancelled_) return;
    if (req_.caps.tasks) {
        if (taskId_.empty()) {
            Task t{req_.who};
            t.taskId = "tsk_" + randomHex(8);
            t.agentId = req_.who.agentId;
            t.key = key_;
            t.status = "working";
            t.createdAtMs = t0_;
            t.lastUpdatedAtMs = nowMs();
            t.ttlMs = p_->d_.cfg->taskTtlMs;
            t.owner = req_.who;
            if (!p_->d_.tasks->insert(t)) {
                ir::Response r;
                r.klass = ir::FailureClass::Unavailable;
                r.body = ir::callToolText("task store full", true);
                respond(std::move(r));
                return;
            }
            attachTask(t.taskId);
        }
        auto t = p_->d_.tasks->get(taskId_);
        if (!t) {
            ir::Response r;
            r.klass = ir::FailureClass::Unavailable;
            r.body = ir::callToolText("task not found", true);
            respond(std::move(r));
            return;
        }
        ir::Response r;
        r.body = createTaskResult(*t);
        respond(std::move(r));
        return;
    }
    ir::Response r;
    r.isError = true;
    r.klass = ir::FailureClass::Capability;
    r.httpStatus = 400;
    r.body = missingTasksError();
    respond(std::move(r));
}

void Call::startUpstream() {
    p_->live_[inflightId_] = shared_from_this();
    fireAttempt();
}

void Call::fireAttempt() {
    auto* be = p_->d_.catalog->backend(key_.backend);
    if (!be) {
        ir::Response r;
        r.klass = ir::FailureClass::Unavailable;
        r.body = ir::callToolText("upstream unavailable", true);
        onUpstream(std::move(r));
        return;
    }
    attempt_++;
    if (attempt_ == 1) armPromote();
    if (p_->d_.tracer) {
        ir::Request tmp = req_;
        tmp.trace = trace_;
        upTrace_ = p_->d_.tracer->start(tmp, "upstream");
        upSpanLive_ = true;
    }
    be->call(makeBackendCall(), [self = shared_from_this()](ir::Response r) {
        self->onUpstream(std::move(r));
    });
}

void Call::onUpstream(ir::Response r) {
    if (upSpanLive_ && p_->d_.tracer) {
        p_->d_.tracer->end(upTrace_, r.klass, r.upstreamMs);
        upSpanLive_ = false;
    }
    if (cancelled_) {
        upstreamDone_ = true;
        return;
    }
    auto* entry = p_->d_.catalog->find(key_.backend);
    ir::BackendMeta meta = entry ? entry->meta : ir::BackendMeta{};
    const bool open = p_->d_.circuit->isOpen(key_.backend, nowMs());
    if (r.klass == ir::FailureClass::Ok && !r.isError) {
        p_->d_.circuit->onSuccess(key_.backend);
    } else {
        const bool becameOpen = p_->d_.circuit->onFailure(key_.backend, nowMs());
        if (becameOpen) {
            p_->audit("circuit_open", req_, ir::FailureClass::Unavailable, key_.str());
        }
    }
    const bool outstanding = responded_ || cancelled_ ||
                             r.klass == ir::FailureClass::Timeout ||
                             r.klass == ir::FailureClass::Cancelled;
    if (!responded_ && !cancelled_ &&
        p_->d_.retry->shouldRetry(r.klass, key_, meta, attempt_, req_.deadlineMs, open,
                                  outstanding)) {
        fireAttempt();
        return;
    }
    upstreamDone_ = true;
    if (promoteTimer_) {
        p_->d_.loop->cancel(promoteTimer_);
        promoteTimer_ = 0;
    }
    if (!taskId_.empty()) {
        const char* st = (r.klass == ir::FailureClass::Ok && !r.isError) ? "completed" : "failed";
        p_->d_.tasks->updateStatus(taskId_, st, r.body, nowMs());
    }
    if (!responded_) {
        if (r.klass != ir::FailureClass::Ok) {
            if (!r.body.is_object() || !r.body.contains("content")) {
                r.body = ir::callToolText(ir::failureClassName(r.klass), true);
            }
        }
        respond(std::move(r));
    }
}

} // namespace perfacet
