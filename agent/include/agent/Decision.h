#pragma once
// agent::Decision —— 准入裁决。只做 Admit/Reject：Delay/优先级队列是调度器，
// 不在本层（需要限流用 Reject 快速失败）。
#include <string>
#include <utility>

namespace agent {

struct Decision {
    bool admit = true;
    std::string reason; // Reject 时的原因（进异常消息/日志）

    static Decision Admit() { return Decision{true, {}}; }
    static Decision Reject(std::string why) {
        return Decision{false, std::move(why)};
    }
};

} // namespace agent
