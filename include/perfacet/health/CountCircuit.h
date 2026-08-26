#pragma once
// 连续失败计数熔断。OPEN 不改切面。抽 Circuit 时替换本类。
#include "perfacet/ir/Request.h"

#include "netlib/EventLoop.h"

#include <string>
#include <unordered_map>

namespace perfacet {

class CountCircuit {
public:
    enum class CState { Closed, Open, HalfOpen };

    CountCircuit(int openAfter, uint64_t cooldownMs, int halfOpenProbes,
                 netlib::EventLoop* loop);

    bool isOpen(const std::string& server, uint64_t nowMs);
    CState state(const std::string& server, uint64_t nowMs);
    void onSuccess(const std::string& server);
    // 返回是否刚刚转入 OPEN
    bool onFailure(const std::string& server, uint64_t nowMs);
    void onProbeSuccess(const std::string& server);

private:
    struct Slot {
        CState st = CState::Closed;
        int fails = 0;
        int halfOk = 0;
        uint64_t openUntilMs = 0;
    };
    Slot& slot(const std::string& server);
    void maybeHalfOpen(Slot& s, uint64_t nowMs);

    int openAfter_;
    uint64_t cooldownMs_;
    int halfOpenProbes_;
    netlib::EventLoop* loop_;
    std::unordered_map<std::string, Slot> slots_;
};

} // namespace perfacet
