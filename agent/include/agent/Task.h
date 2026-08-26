#pragma once
// agent::TaskTree —— 任务树（核心类型 + 树操作，一把 mutex）。
// 树是取消传播与生命周期记账的骨架：spawn 收紧 deadline，cancel DFS 置位
// 并收集在途 RPC，complete 在无子无在途时摘除节点。
// agent 不知 epoll / 不 include netlib：树操作纯内存，叶子传输由 Runtime 粘合。
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace agent {

using TaskId = uint64_t; // 0 = 无父（根）

struct Task {
    TaskId id = 0;
    TaskId parent = 0;
    uint64_t deadlineUnixMs = 0; // 只可收紧（子 = min(父, proposed)）
    std::atomic<bool> cancelled{false};
    // 由 adopt 建节点：生命周期归入站侧（doneHook 结束）。
    // 本地 spawn 的节点为 false，只能由创建方结束。
    bool adopted = false;
    // 以下三成员只在 TaskTree 持锁读写
    std::vector<TaskId> children;
    std::vector<uint64_t> inflightRpcIds; // 本任务直接发出的在途 requestId
};

// 当前任务只读快照（Policy 入参；禁止直接改树）
struct TaskContext {
    TaskId id = 0;
    uint64_t deadlineUnixMs = 0;
    bool cancelled = false;
    bool exists = false; // 任务不存在时快照为全零默认值，exists=false
};

class TaskTree {
public:
    // 新建任务：deadline = min(parent.deadline, proposed)；parent=0 为根。
    // parent 不存在返回 0（不建孤儿节点）——调用方应立即失败。
    TaskId spawn(TaskId parent, uint64_t proposedDeadlineUnixMs);

    // 入站任务落地：不存在则建根并推进 nextId 防撞号；存在则幂等返回。
    TaskId adopt(TaskId taskId, uint64_t deadlineUnixMs);

    // DFS 置位取消，收集并清空子树全部 inflightRpcIds（收集即清空：
    // 二次 cancel 返回空——CANCEL 回环的天然终止条件）。
    std::vector<uint64_t> cancel(TaskId id);

    TaskContext context(TaskId id) const;
    bool cancelled(TaskId id) const;

    // 挂叶成功返回 true；任务不存在或已取消返回 false——调用方应对该
    // requestId 立即补发 CANCEL（关闭 dispatch 与 cancel 的竞态窗口）。
    bool attachRpc(TaskId id, uint64_t requestId);
    // 摘除任务全部在途记账（单任务的尝试严格串行；重试换 requestId 前调）
    void detachRpc(TaskId id);

    // 结束任务：无子节点且无在途请求时从树上摘除自己（不递归摘父——
    // 每个节点的结束都由其创建方显式发起）。已摘除返回 true（幂等）；
    // 条件不满足返回 false，节点保留。
    bool complete(TaskId id);

    // 入站侧收尾：只结束 adopt 建立的节点。自连拓扑（同进程既是调用方又是
    // 被调方）下入站 taskId 可能就是本地 spawn 出来的叶子，其生命周期归
    // 调用方，入站侧不许摘——这种情况不动树并返回 true。
    bool completeAdopted(TaskId id);

    // 当前节点数（观测与测试用）
    size_t size() const;

private:
    using TaskMap = std::unordered_map<TaskId, std::unique_ptr<Task>>;

    void cancelLocked(TaskId id, std::vector<uint64_t>& rids);
    bool completeLocked(TaskMap::iterator it);

    mutable std::mutex mtx_;
    TaskMap tasks_;
    TaskId nextId_ = 1;
};

// 当前 Unix 毫秒（agent 层共享时间工具）
inline uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace agent
