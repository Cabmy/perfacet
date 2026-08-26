#pragma once
// 按 interval 对上游 tools/list 探活。不写 ToolIndex。探测不占 Governor 配额。
#include "perfacet/backend/Backend.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/health/Health.h"
#include "perfacet/ir/Request.h"

#include "netlib/EventLoop.h"
#include "netlib/ThreadPool.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace perfacet {

class ProbeHealth : public Health {
public:
    using ProbeCb = std::function<void(const std::string& server, State,
                                       std::optional<ir::Json> toolList)>;

    ProbeHealth(Catalog& catalog, netlib::EventLoop* loop, netlib::ThreadPool* pool,
                uint64_t intervalMs, uint64_t degradedMs, int downAfter);

    void setCallback(ProbeCb cb) { cb_ = std::move(cb); }

    // 阻塞：listen 前首轮。失败不抛，记 Down。
    void probeAllBlocking();

    void startTimer();

    State state(const std::string& server) const override;
    uint64_t latencyEwmaMs(const std::string& server) const override;

    struct Snapshot {
        std::string name;
        State state = State::Down;
        uint64_t latencyEwmaMs = 0;
    };
    std::vector<Snapshot> all() const;

private:
    struct Slot {
        State state = State::Down;
        uint64_t ewma = 0;
        int fails = 0;
        bool ewmaInit = false;
    };

    void applyResult(const std::string& name, bool ok, uint64_t latencyMs,
                     std::optional<ir::Json> tools);

    Catalog* catalog_;
    netlib::EventLoop* loop_;
    netlib::ThreadPool* pool_;
    uint64_t intervalMs_;
    uint64_t degradedMs_;
    int downAfter_;
    ProbeCb cb_;
    std::unordered_map<std::string, Slot> slots_;
    netlib::TimerId timerId_ = 0;
};

} // namespace perfacet
