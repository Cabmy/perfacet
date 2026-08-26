#include "perfacet/observe/OtlpHttpJsonTracer.h"
#include "detail/Random.h"
#include "detail/Time.h"
#include "perfacet/ir/ClientCaps.h"

#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace perfacet {

namespace {

std::pair<std::string, int> hostPortPath(const std::string& url, std::string& path) {
    std::string rest = url;
    if (rest.compare(0, 7, "http://") == 0) rest = rest.substr(7);
    auto slash = rest.find('/');
    std::string hp = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/v1/traces" : rest.substr(slash);
    auto colon = hp.rfind(':');
    if (colon == std::string::npos) return {hp, 80};
    return {hp.substr(0, colon), std::stoi(hp.substr(colon + 1))};
}

} // namespace

OtlpHttpJsonTracer::OtlpHttpJsonTracer(const YamlConfig& cfg, netlib::ThreadPool* pool,
                                       Counters* counters)
    : endpoint_(cfg.otelEndpoint), service_(cfg.otelService), queueMax_(cfg.otelQueueMax),
      pool_(pool), counters_(counters) {}

OtlpHttpJsonTracer::~OtlpHttpJsonTracer() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
            q_.clear();
            if (!draining_) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

ir::TraceContext OtlpHttpJsonTracer::start(const ir::Request& req, const char* spanName) {
    ir::TraceContext t = req.trace;
    if (t.traceId.empty()) t.traceId = randomHex(16);
    t.parentSpanId = t.spanId;
    t.spanId = randomHex(8);
    SpanAcc acc;
    acc.principal = req.who.agentId;
    acc.level = req.who.levelName;
    acc.tool = req.name;
    acc.name = spanName ? spanName : "gateway";
    acc.startMs = nowMs();
    live_[t.spanId] = std::move(acc);
    return t;
}

void OtlpHttpJsonTracer::set(const ir::TraceContext& t, const char* key,
                             const std::string& value) {
    auto it = live_.find(t.spanId);
    if (it == live_.end()) return;
    it->second.attrs[key] = value;
}

void OtlpHttpJsonTracer::end(const ir::TraceContext& t, ir::FailureClass k,
                             uint64_t latencyMs) {
    auto it = live_.find(t.spanId);
    if (it == live_.end()) return;
    SpanAcc acc = std::move(it->second);
    live_.erase(it);
    const uint64_t startNs = acc.startMs * 1000000ULL;
    const uint64_t endNs = startNs + latencyMs * 1000000ULL;
    ir::Json attrs = ir::Json::array();
    auto add = [&](const char* k, const std::string& v) {
        if (v.empty()) return;
        attrs.push_back(ir::Json{{"key", k}, {"value", ir::Json{{"stringValue", v}}}});
    };
    add("service.name", service_);
    add("principal", acc.principal);
    add("level", acc.level);
    add("tool", acc.tool);
    auto key = ir::ToolKey::parse(acc.tool);
    if (key) add("server", key->backend);
    add("status", ir::failureClassName(k));
    for (const auto& kv : acc.attrs) add(kv.first.c_str(), kv.second);
    attrs.push_back(ir::Json{{"key", "latency_ms"},
                             {"value", ir::Json{{"intValue", std::to_string(latencyMs)}}}});
    ir::Json span{
        {"traceId", t.traceId},
        {"spanId", t.spanId},
        {"name", acc.name},
        {"kind", 1},
        {"startTimeUnixNano", std::to_string(startNs)},
        {"endTimeUnixNano", std::to_string(endNs)},
        {"attributes", attrs},
        {"status",
         ir::Json{{"code", k == ir::FailureClass::Ok ? 1 : 2}, {"message", ir::failureClassName(k)}}},
    };
    if (!t.parentSpanId.empty()) span["parentSpanId"] = t.parentSpanId;
    ir::Json payload{
        {"resourceSpans",
         ir::Json::array({ir::Json{
             {"resource",
              ir::Json{{"attributes",
                        ir::Json::array({ir::Json{{"key", "service.name"},
                                                  {"value", ir::Json{{"stringValue", service_}}}}})}}},
             {"scopeSpans", ir::Json::array({ir::Json{{"spans", ir::Json::array({span})}}})}}})}};
    enqueue(payload.dump());
}

void OtlpHttpJsonTracer::enqueue(std::string json) {
    bool kick = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_ || q_.size() >= queueMax_) {
            if (!closed_ && counters_) counters_->otlpDropped.fetch_add(1);
            return;
        }
        q_.push_back(std::move(json));
        if (!draining_) {
            draining_ = true;
            kick = true;
        }
    }
    if (kick) {
        pool_->add([this]() { drain(); });
    }
}

void OtlpHttpJsonTracer::drain() {
    for (;;) {
        std::string item;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (closed_ || q_.empty()) {
                draining_ = false;
                return;
            }
            item = std::move(q_.front());
            q_.pop_front();
        }
        postOne(item);
    }
}

void OtlpHttpJsonTracer::postOne(const std::string& json) {
    // worker 上 POST；IO loop 禁止走这里
    std::string path;
    auto hp = hostPortPath(endpoint_, path);
    httplib::Client cli(hp.first, hp.second);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(2, 0);
    (void)cli.Post(path, json, "application/json");
}

} // namespace perfacet
