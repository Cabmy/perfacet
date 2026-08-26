#include "perfacet/govern/LocalGovernor.h"
#include "detail/Time.h"

#include <cstdio>

namespace perfacet {

LocalGovernor::LocalGovernor(const YamlConfig& cfg, netlib::EventLoop* loop,
                             Counters* counters)
    : cfg_(cfg), loop_(loop), counters_(counters) {}

int LocalGovernor::toolLimit(const std::string& toolStr) const {
    auto it = cfg_.governorTools.find(toolStr);
    if (it != cfg_.governorTools.end() && it->second.status == GovernorToolCfg::Status::Active &&
        it->second.maxConcurrency > 0) {
        return it->second.maxConcurrency;
    }
    return cfg_.perToolConcurrency;
}

uint64_t LocalGovernor::toolWaitMs(const std::string& toolStr) const {
    auto it = cfg_.governorTools.find(toolStr);
    if (it != cfg_.governorTools.end() && it->second.status == GovernorToolCfg::Status::Active &&
        it->second.queueWaitMs > 0) {
        return it->second.queueWaitMs;
    }
    return cfg_.queueWaitMs;
}

bool LocalGovernor::canTake(const std::string& toolStr, const std::string& agent) const {
    int t = 0, p = 0;
    auto it = toolInUse_.find(toolStr);
    if (it != toolInUse_.end()) t = it->second;
    auto ip = principalInUse_.find(agent);
    if (ip != principalInUse_.end()) p = ip->second;
    return t < toolLimit(toolStr) && p < cfg_.perPrincipalConcurrency;
}

void LocalGovernor::take(const std::string& toolStr, const std::string& agent) {
    toolInUse_[toolStr]++;
    principalInUse_[agent]++;
    if (counters_) counters_->permitHeld.fetch_add(1);
}

Governor::Permit LocalGovernor::issue(const ir::ToolKey& key, const std::string& agent) {
    take(key.str(), agent);
    return Permit::make(this, key, agent);
}

void LocalGovernor::acquire(const ir::Principal& who, const ir::ToolKey& key,
                            uint64_t waitDeadlineMs,
                            std::function<void(Admit, Permit)> onAdmit) {
    const std::string ts = key.str();
    if (canTake(ts, who.agentId)) {
        onAdmit(Admit::Go, issue(key, who.agentId));
        return;
    }
    const uint64_t now = nowMs();
    uint64_t deadline = waitDeadlineMs;
    if (deadline == 0) deadline = now + toolWaitMs(ts);
    if (deadline <= now) {
        onAdmit(Admit::Reject, Permit{});
        return;
    }
    Waiter w;
    w.who = who;
    w.key = key;
    w.deadlineMs = deadline;
    w.cb = std::move(onAdmit);
    w.id = nextWaitId_++;
    const double waitSec = static_cast<double>(deadline - now) / 1000.0;
    const uint64_t id = w.id;
    w.timer = loop_->runAfter(waitSec, [this, ts, id]() {
        auto& q = queues_[ts];
        for (auto it = q.begin(); it != q.end(); ++it) {
            if (it->id != id) continue;
            auto cb = std::move(it->cb);
            q.erase(it);
            cb(Admit::Reject, Permit{});
            return;
        }
    });
    queues_[ts].push_back(std::move(w));
}

void LocalGovernor::tryDequeue(const std::string& toolStr) {
    auto& q = queues_[toolStr];
    while (!q.empty()) {
        Waiter& head = q.front();
        if (!canTake(toolStr, head.who.agentId)) break;
        Waiter w = std::move(head);
        q.pop_front();
        if (w.timer) loop_->cancel(w.timer);
        if (nowMs() >= w.deadlineMs) {
            w.cb(Admit::Reject, Permit{});
            continue;
        }
        w.cb(Admit::Go, issue(w.key, w.who.agentId));
        break;
    }
}

void LocalGovernor::releaseSlot(const ir::ToolKey& key, std::string_view agentId) {
    const std::string ts = key.str();
    auto it = toolInUse_.find(ts);
    if (it != toolInUse_.end() && it->second > 0) it->second--;
    auto ip = principalInUse_.find(std::string(agentId));
    if (ip != principalInUse_.end() && ip->second > 0) ip->second--;
    if (counters_) counters_->permitHeld.fetch_sub(1);
    tryDequeue(ts);
}

void LocalGovernor::onToolsListed(const std::string& backend,
                                  const std::vector<std::string>& tools) {
    std::unordered_set<std::string> set(tools.begin(), tools.end());
    for (auto& kv : cfg_.governorTools) {
        auto parsed = ir::ToolKey::parse(kv.first);
        if (!parsed || parsed->backend != backend) continue;
        if (set.count(parsed->tool)) {
            kv.second.status = GovernorToolCfg::Status::Active;
        } else {
            kv.second.status = GovernorToolCfg::Status::Invalid;
            std::fprintf(stderr,
                         "[perfacet] WARN governor.tools %s 未命中已 list 的工具，限制不生效\n",
                         kv.first.c_str());
        }
    }
}

void LocalGovernor::rejectAllQueued() {
    for (auto& kv : queues_) {
        while (!kv.second.empty()) {
            auto w = std::move(kv.second.front());
            kv.second.pop_front();
            if (w.timer) loop_->cancel(w.timer);
            w.cb(Admit::Reject, Permit{});
        }
    }
}

ir::Json LocalGovernor::statusJson() const {
    ir::Json j = ir::Json::object();
    for (const auto& kv : cfg_.governorTools) {
        const char* st = "unvalidated";
        if (kv.second.status == GovernorToolCfg::Status::Active) st = "active";
        if (kv.second.status == GovernorToolCfg::Status::Invalid) st = "invalid";
        j[kv.first] = st;
    }
    return j;
}

int LocalGovernor::permitHeld() const {
    int n = 0;
    for (const auto& kv : toolInUse_) n += kv.second;
    return n;
}

} // namespace perfacet
