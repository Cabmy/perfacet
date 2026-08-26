#pragma once
// agent::Runtime —— 唯一把 TaskTree、PolicyChain、IRetry 与 rpc 粘起来的地方。
// 出站：spawn → 准入（Reject 不占 inflight）→ callFrame（deadline/taskId TLV）
//       → 失败且 shouldRetry 且未取消 → 换新 requestId 重打（deadline 只减不增）
//       → 离开 callFrame 前结束叶子任务（成功/失败/异常同一出口）。
// 入站：attachServer 挂 requestHook（adopt 入站任务）/ cancelHook（取消本地
//       子树并对在途请求发 CANCEL）/ doneHook（响应发出后结束入站任务）
//       ——跨进程树靠 CANCEL 叶子传播，不加协议字段。
// 生命周期：谁创建、谁结束。叶子由 Runtime 结束；根与中间节点由创建方
// 显式 complete。client/server 必须活得比 Runtime 久（析构时自动摘钩）。
#include <memory>
#include <string>

#include "agent/IPolicy.h"
#include "agent/IRetry.h"
#include "agent/PolicyChain.h"
#include "agent/Task.h"
#include "client/RpcClient.h"
#include "client/RpcException.h"
#include "server/RpcServer.h"

namespace agent {

class Runtime {
public:
    // retry 传 nullptr = NoRetry 语义（rpc 层本就单次调用）
    explicit Runtime(twigrpc::RpcClient& client,
                     std::unique_ptr<IRetry> retry = nullptr);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // 入站钩子挂到 server（serve() 前调用）
    void attachServer(twigrpc::RpcServer& server);

    TaskId spawnRoot(uint64_t deadlineUnixMs) { return tree_.spawn(0, deadlineUnixMs); }

    // 结束任务（谁创建、谁结束）：无子无在途时从树上摘除；条件不满足
    // 返回 false，节点保留。叶子由 callFrame 自动结束，根/中间节点由
    // 创建方在收齐结果后调用。不发 CANCEL、不碰 client。
    bool complete(TaskId id) { return tree_.complete(id); }

    // 取消任务子树：本地树 DFS + 对仍在途的请求发 CANCEL
    void cancel(TaskId id);

    // 原始帧出站调用（单次 + 准入 + 可选重试），供需要绕过 proto 的调用方
    twigrpc::Frame callFrame(TaskId parent, const std::string& method,
                             const std::string& body, uint64_t deadlineUnixMs);

    // 带类型的出站调用（单次 + 准入 + 可选重试）。timeoutMs 只收紧任务 deadline。
    template <typename Resp, typename Req>
    Resp call(TaskId parent, const std::string& method, const Req& req,
              int timeoutMs = 500) {
        twigrpc::Frame f = callFrame(parent, method, req.SerializeAsString(),
                                     nowMs() + static_cast<uint64_t>(timeoutMs));
        if (f.status != twigrpc::Status::OK) {
            throw twigrpc::RpcException(f.status, f.body);
        }
        Resp resp;
        if (!resp.ParseFromString(f.body)) {
            throw twigrpc::RpcException(twigrpc::Status::DECODE_ERROR,
                                        "response parse failed");
        }
        return resp;
    }

    TaskTree& tree() { return tree_; }
    PolicyChain& policies() { return chain_; }

private:
    twigrpc::RpcClient* client_;
    twigrpc::RpcServer* server_ = nullptr;
    TaskTree tree_;
    PolicyChain chain_;
    std::unique_ptr<IRetry> retry_;
};

} // namespace agent
