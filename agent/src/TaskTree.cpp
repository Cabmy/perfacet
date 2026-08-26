#include "agent/Task.h"

#include <algorithm>
#include <utility>

namespace agent {

TaskId TaskTree::spawn(TaskId parent, uint64_t proposedDeadlineUnixMs) {
    std::lock_guard<std::mutex> lk(mtx_);
    uint64_t deadline = proposedDeadlineUnixMs;
    if (parent != 0) {
        auto pit = tasks_.find(parent);
        if (pit == tasks_.end()) return 0; // 不建孤儿：父必须先存在
        // deadline 只可收紧：子 = min(父, proposed)；0 = 无期限（min 的单位元）
        const uint64_t pd = pit->second->deadlineUnixMs;
        if (pd != 0) {
            deadline = (proposedDeadlineUnixMs != 0)
                           ? std::min(pd, proposedDeadlineUnixMs)
                           : pd;
        }
        TaskId id = nextId_++;
        pit->second->children.push_back(id);
        auto t = std::make_unique<Task>();
        t->id = id;
        t->parent = parent;
        t->deadlineUnixMs = deadline;
        tasks_[id] = std::move(t);
        return id;
    }
    TaskId id = nextId_++;
    auto t = std::make_unique<Task>();
    t->id = id;
    t->deadlineUnixMs = deadline;
    tasks_[id] = std::move(t);
    return id;
}

TaskId TaskTree::adopt(TaskId taskId, uint64_t deadlineUnixMs) {
    std::lock_guard<std::mutex> lk(mtx_);
    // 入站 taskId 可能落在本地 nextId_ 区间：推进防撞号
    nextId_ = std::max(nextId_, taskId + 1);
    auto it = tasks_.find(taskId);
    if (it != tasks_.end()) return taskId; // 幂等
    auto t = std::make_unique<Task>();
    t->id = taskId;
    t->deadlineUnixMs = deadlineUnixMs;
    t->adopted = true;
    tasks_[taskId] = std::move(t);
    return taskId;
}

std::vector<uint64_t> TaskTree::cancel(TaskId id) {
    std::vector<uint64_t> rids;
    std::lock_guard<std::mutex> lk(mtx_);
    cancelLocked(id, rids);
    return rids;
}

void TaskTree::cancelLocked(TaskId id, std::vector<uint64_t>& rids) {
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return;
    Task& t = *it->second;
    if (t.cancelled.exchange(true)) return; // 已取消子树：inflight 已被首次 cancel 清空
    for (uint64_t rid : t.inflightRpcIds) rids.push_back(rid);
    t.inflightRpcIds.clear(); // 收集即清空：二次 cancel 返回空（CANCEL 回环终止条件）
    for (TaskId c : t.children) cancelLocked(c, rids);
}

TaskContext TaskTree::context(TaskId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    TaskContext ctx;
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return ctx; // 不存在：全零快照 + exists=false
    ctx.exists = true;
    ctx.id = it->second->id;
    ctx.deadlineUnixMs = it->second->deadlineUnixMs;
    ctx.cancelled = it->second->cancelled.load();
    return ctx;
}

bool TaskTree::cancelled(TaskId id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    return it != tasks_.end() && it->second->cancelled.load();
}

bool TaskTree::attachRpc(TaskId id, uint64_t requestId) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end() || it->second->cancelled.load()) return false;
    it->second->inflightRpcIds.push_back(requestId);
    return true;
}

void TaskTree::detachRpc(TaskId id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return;
    it->second->inflightRpcIds.clear(); // 单任务的尝试严格串行：清空即摘除本次叶
}

bool TaskTree::complete(TaskId id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return true; // 幂等：已经摘除
    return completeLocked(it);
}

bool TaskTree::completeAdopted(TaskId id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return true;        // 幂等：已经摘除
    if (!it->second->adopted) return true;      // 本地 spawn：归创建方结束
    return completeLocked(it);
}

bool TaskTree::completeLocked(TaskMap::iterator it) {
    const TaskId id = it->first;
    Task& t = *it->second;
    if (!t.inflightRpcIds.empty() || !t.children.empty()) return false;
    if (t.parent != 0) {
        // 摘除自己与父的父子关系（父可能已先被摘除，忽略即可）
        auto pit = tasks_.find(t.parent);
        if (pit != tasks_.end()) {
            auto& cs = pit->second->children;
            cs.erase(std::remove(cs.begin(), cs.end(), id), cs.end());
        }
    }
    tasks_.erase(it);
    return true;
}

size_t TaskTree::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return tasks_.size();
}

} // namespace agent
