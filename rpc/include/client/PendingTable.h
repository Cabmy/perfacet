#pragma once
// twigrpc::PendingTable —— 在途请求表（挂在单条 RpcConn 上）。
// 核心不变量：每个 promise 恰好被 set 一次。
// 由「先从 map 原子 erase，erase 成功者才 set」保证：
//   - take(id) 原子摘除：谁 erase 到谁 set_value；
//   - erase 失败（迟到响应/超时已摘）→ 丢弃并 LOG_WARN；
//   - failAll 对表中剩余项逐一 set_exception 后清表。
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "codec/Protocol.h"

namespace twigrpc {

using Deadline = std::chrono::steady_clock::time_point;

struct PendingCall {
    std::promise<Frame> pr;
    Deadline dl;
};

class PendingTable {
public:
    // 插入在途请求，返回 future
    std::future<Frame> insert(uint64_t id, Deadline dl) {
        std::lock_guard<std::mutex> lk(mtx_);
        PendingCall pc;
        pc.dl = dl;
        auto fut = pc.pr.get_future();
        m_.emplace(id, std::move(pc));
        return fut;
    }

    // 原子摘除：摘到者 set promise（set_value 由调用方完成）
    std::optional<PendingCall> take(uint64_t id) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = m_.find(id);
        if (it == m_.end()) return std::nullopt;
        PendingCall pc = std::move(it->second);
        m_.erase(it);
        return pc;
    }

    // 连接断开：对剩余所有项 set_exception
    void failAll(std::exception_ptr ep) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [id, pc] : m_) {
            pc.pr.set_exception(ep);
        }
        m_.clear();
    }

    // 超时扫描：返回到期项供 sweeper 处理（先发 CANCEL 再 take 失败 promise）
    std::vector<uint64_t> expiredIds(Deadline now) {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<uint64_t> ids;
        for (auto& [id, pc] : m_) {
            if (pc.dl <= now) ids.push_back(id);
        }
        return ids;
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mtx_);
        return m_.size();
    }

private:
    std::unordered_map<uint64_t, PendingCall> m_;
    std::mutex mtx_;
};

} // namespace twigrpc
