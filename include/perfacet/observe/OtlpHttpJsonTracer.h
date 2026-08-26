#pragma once
// 手写 OTLP/HTTP JSON。loop 只入队；满则丢。禁止 opentelemetry-cpp。
#include "perfacet/observe/Counters.h"
#include "perfacet/observe/Tracer.h"
#include "perfacet/policy/YamlConfig.h"

#include "netlib/ThreadPool.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace perfacet {

class OtlpHttpJsonTracer : public Tracer {
public:
    OtlpHttpJsonTracer(const YamlConfig& cfg, netlib::ThreadPool* pool,
                       Counters* counters);
    ~OtlpHttpJsonTracer();

    ir::TraceContext start(const ir::Request&, const char* spanName) override;
    void set(const ir::TraceContext&, const char* key,
             const std::string& value) override;
    void end(const ir::TraceContext&, ir::FailureClass,
             uint64_t latencyMs) override;

private:
    struct SpanAcc {
        ir::Request req;
        std::string name;
        uint64_t startMs = 0;
        std::unordered_map<std::string, std::string> attrs;
    };

    void enqueue(std::string json);
    void drain();
    void postOne(const std::string& json);

    std::string endpoint_, service_;
    std::size_t queueMax_;
    netlib::ThreadPool* pool_;
    Counters* counters_;
    std::mutex mu_;
    std::deque<std::string> q_;
    bool draining_ = false;
    bool closed_ = false;
    std::unordered_map<std::string, SpanAcc> live_;
};

} // namespace perfacet
