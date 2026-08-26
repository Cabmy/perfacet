#pragma once
// agent::IPolicy —— 准入扩展点（只做准入，不做重试/定时/队列）。
// 框架只提供插件点；Runtime 只依赖 IPolicy/PolicyChain，不 include 具体插件。
#include <string_view>

#include "agent/Decision.h"
#include "agent/Task.h"
#include "codec/Protocol.h"

namespace agent {

// RPC 请求只读视图（不复制 proto）
struct RpcRequest {
    std::string_view method;
    std::string_view body;
};

class IPolicy {
public:
    virtual ~IPolicy() = default;

    // 调用前准入。Admit 放行（可能占槽），Reject 快速失败不占 inflight。
    virtual Decision evaluate(const TaskContext& ctx, const RpcRequest& req) = 0;

    // 调用结束后记账（Admission 释放槽）。每次 Admit 后必然恰有一次 onComplete。
    virtual void onComplete(const TaskContext& ctx, twigrpc::Status st) {
        (void)ctx;
        (void)st;
    }
};

} // namespace agent
