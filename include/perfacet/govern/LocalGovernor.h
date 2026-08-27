#pragma once
// 单进程 FIFO 准入。Permit RAII 归还是进程内假设，不是分布式租约。
#include "perfacet/govern/Governor.h"
#include "perfacet/observe/Counters.h"
#include "perfacet/policy/YamlConfig.h"

#include "netlib/EventLoop.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace perfacet {

class LocalGovernor : public Governor {
public:
    LocalGovernor(const YamlConfig& cfg, netlib::EventLoop* loop, Counters* counters);

    void acquire(const ir::Principal&, const ir::ToolKey&, uint64_t waitDeadlineMs,
                 std::function<void(Admit, Permit)> onAdmit) override;

    void onToolsListed(const std::string& backend,
                       const std::vector<std::string>& tools);

    void rejectAllQueued();

    ir::Json statusJson() const;
    int permitHeld() const;

protected:
    void releaseSlot(const ir::ToolKey&, std::string_view agentId) override;

private:
    struct Waiter {
        ir::Principal who;
        ir::ToolKey key;
        uint64_t deadlineMs = 0;
        netlib::TimerId timer = 0;
        std::function<void(Admit, Permit)> cb;
        uint64_t id = 0;
    };

    bool canTake(const std::string& toolStr, const std::string& agent) const;
    void take(const std::string& toolStr, const std::string& agent);
    int toolLimit(const std::string& toolStr) const;
    uint64_t toolWaitMs(const std::string& toolStr) const;
    void tryDequeue(const std::string& toolStr);
    Permit issue(const ir::ToolKey& key, const std::string& agent);

    YamlConfig cfg_;
    netlib::EventLoop* loop_;
    Counters* counters_;
    std::unordered_map<std::string, int> toolInUse_;
    std::unordered_map<std::string, int> principalInUse_;
    std::unordered_map<std::string, std::deque<Waiter>> queues_;
    uint64_t nextWaitId_ = 1;
};

} // namespace perfacet
