#include "agent/Runtime.h"

#include <chrono>
#include <cstdio>
#include <utility>

namespace agent {

Runtime::Runtime(twigrpc::RpcClient& client, std::unique_ptr<IRetry> retry)
    : client_(&client), retry_(std::move(retry)) {
    // dispatchHook 在调用线程内、等待响应之前同步回调：把 requestId 挂到树上，
    // CANCEL 才有明确目标。
    client_->setDispatchHook([this](uint64_t requestId, uint64_t taskId) {
        if (taskId != 0 && !tree_.attachRpc(taskId, requestId)) {
            client_->cancelInflight(requestId); // dispatch 与 cancel 竞态：立即补刀
        }
    });
}

Runtime::~Runtime() {
    client_->setDispatchHook(nullptr);
    if (server_) {
        server_->setDoneHook(nullptr);
        server_->setCancelHook(nullptr);
        server_->setRequestHook(nullptr);
    }
}

void Runtime::attachServer(twigrpc::RpcServer& server) {
    server_ = &server;
    // 入站 REQUEST 带 taskId → adopt 为本进程根（幂等）
    server.setRequestHook([this](uint64_t taskId, uint64_t deadlineUnixMs) {
        tree_.adopt(taskId, deadlineUnixMs);
    });
    // 入站 CANCEL（token 已置位）→ 取消本地子树 + 对本进程发出的在途请求补 CANCEL。
    // hook 在 IO 线程执行：tree.cancel 持锁即返，cancelInflight 只投递帧，均非阻塞。
    server.setCancelHook([this](uint64_t requestId, uint64_t taskId) {
        (void)requestId; // 入站请求由 server 的 token 取消；这里只管本地子树
        if (taskId == 0) return;
        for (uint64_t rid : tree_.cancel(taskId)) client_->cancelInflight(rid);
    });
    // 入站请求的响应已发出（或连接已断无需回包）→ 结束入站任务（adopt 的对称出口）。
    // 若 handler 同步发过子调用，子叶子已在各自 callFrame 出口结束，此处可摘。
    server.setDoneHook([this](uint64_t taskId) {
        if (taskId == 0) return;
        if (!tree_.completeAdopted(taskId)) {
            std::fprintf(stderr, "[agent] task %llu done with residue left\n",
                         static_cast<unsigned long long>(taskId));
        }
    });
}

void Runtime::cancel(TaskId id) {
    for (uint64_t rid : tree_.cancel(id)) client_->cancelInflight(rid);
}

twigrpc::Frame Runtime::callFrame(TaskId parent, const std::string& method,
                                  const std::string& body,
                                  uint64_t deadlineUnixMs) {
    using twigrpc::CallOpts;
    using twigrpc::Frame;
    using twigrpc::RpcException;
    using twigrpc::Status;

    TaskId child = tree_.spawn(parent, deadlineUnixMs);
    if (child == 0) {
        throw RpcException(Status::INTERNAL, "spawn parent missing");
    }
    // 出口收尾：成功、失败、异常三条路都保证离开本函数前结束叶子任务。
    // 每次尝试结束已 detachRpc，此处叶子应无在途；complete 失败只忽略。
    struct LeafGuard {
        TaskTree& tree;
        TaskId id;
        ~LeafGuard() { (void)tree.complete(id); }
    } leaf{tree_, child};
    int attempt = 0;
    while (true) {
        TaskContext ctx = tree_.context(child);
        if (ctx.cancelled) throw RpcException(Status::CANCELLED, "task cancelled");

        Decision d = chain_.evaluate(ctx, RpcRequest{method, body});
        if (!d.admit) {
            throw RpcException(Status::REJECTED, "admission rejected: " + d.reason);
        }

        // deadline 不延长：每次尝试按剩余时间重打（deadline=0 无期限则用 rpc 缺省）
        int timeoutMs = 500;
        if (ctx.deadlineUnixMs != 0) {
            uint64_t now = nowMs();
            timeoutMs = ctx.deadlineUnixMs > now
                            ? static_cast<int>(ctx.deadlineUnixMs - now)
                            : 1; // 已过期仍发一次，由 sweeper 立即兜底
        }
        Frame f;
        try {
            f = client_->callFrame(method, body, CallOpts{timeoutMs, "", child});
        } catch (const RpcException& e) {
            tree_.detachRpc(child);
            chain_.onComplete(ctx, e.status());
            ++attempt;
            // 取消优先于重试：任务已死不再换 id 重打
            if (tree_.cancelled(child)) {
                throw RpcException(Status::CANCELLED, "task cancelled");
            }
            if (retry_ && retry_->shouldRetry(ctx, e.status(), attempt)) continue;
            throw;
        }
        // BUSY 等是响应帧状态，不是异常；必须在重试循环内决策，
        // 否则 RetryPolicy 默认 retryable 含 BUSY 是死代码。
        // 成功路径的 throw 必须在 try 外，避免被上面 catch 二次 onComplete。
        tree_.detachRpc(child);
        chain_.onComplete(ctx, f.status);
        if (f.status != Status::OK) {
            if (tree_.cancelled(child)) {
                throw RpcException(Status::CANCELLED, "task cancelled");
            }
            ++attempt;
            if (retry_ && retry_->shouldRetry(ctx, f.status, attempt)) continue;
        }
        return f;
    }
}

} // namespace agent
