#pragma once
// agent::NoRetry —— 零行为重试（默认语义；rpc 层本就单次调用）。
#include "agent/IRetry.h"

namespace agent {

class NoRetry : public IRetry {
public:
    bool shouldRetry(const TaskContext&, twigrpc::Status, int) override {
        return false;
    }
};

} // namespace agent
