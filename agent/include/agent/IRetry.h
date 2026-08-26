#pragma once
// agent::IRetry —— 重试扩展点（独立于 IPolicy：重试在 Runtime 调用循环里，
// 不是 evaluate 里的 Admit，避免「假插件」与单次 evaluate 语义拧巴）。
// 默认 NoRetry（零行为）；重试只存在于这里，rpc 层无内置重试。
#include "agent/Task.h"
#include "codec/Protocol.h"

namespace agent {

class IRetry {
public:
    virtual ~IRetry() = default;

    // st：上一次尝试的失败状态；attempt：已失败次数（从 1 起）。
    // 返回 true 则换新 requestId 重打（deadline 不延长）。
    virtual bool shouldRetry(const TaskContext& ctx, twigrpc::Status st,
                             int attempt) = 0;
};

} // namespace agent
