#pragma once
// Grant 快照：worker 刷新文件，IO loop 只读 shared_ptr。过期由 now < expiresAt 判定。
// 持久化抽 GrantStore 时替换本类即可。
#include "perfacet/ir/Request.h"
#include "perfacet/policy/Taxonomy.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace perfacet {

struct GrantRecord {
    std::string id;
    std::string agent;
    std::string bumpTo;
    ir::Rank rank = 0;
    std::string status; // pending|approved|denied
    uint64_t expiresAt = 0;
    uint64_t tsMs = 0;
};

struct GrantTable {
    std::unordered_map<std::string, GrantRecord> byId;
};

class JsonlGrantStore {
public:
    JsonlGrantStore(std::string path, const Taxonomy* tax, ir::Rank maxBump,
                    uint64_t ttlMs);

    // 仅 worker 调用：stat + 必要时解析，原子替换快照
    void refreshOnWorker();

    // loop 上无 syscall：读快照 + now 比较
    ir::Rank effectiveBump(std::string_view agentId, uint64_t nowMs) const;

    uint64_t shortestRemainingMs(std::string_view agentId, uint64_t nowMs) const;

    std::string appendPending(const std::string& agent, const std::string& bumpTo,
                              ir::Rank rank, uint64_t nowMs);
    bool approveById(const std::string& id, uint64_t nowMs);
    bool approveDirect(const std::string& agent, ir::Rank rank,
                       const std::string& bumpTo, uint64_t nowMs);

    using ExpireFn = std::function<void(const GrantRecord&)>;
    void setOnExpire(ExpireFn fn) { onExpire_ = std::move(fn); }

    std::shared_ptr<const GrantTable> snapshot() const;

private:
    void parseFileUnlocked();
    void appendLine(const GrantRecord& r);

    std::string path_;
    const Taxonomy* tax_;
    ir::Rank maxBump_;
    uint64_t ttlMs_;
    std::shared_ptr<const GrantTable> table_;
    mutable std::mutex mu_;
    int64_t lastMtimeNs_ = -1;
    std::unordered_map<std::string, bool> expireEmitted_;
    ExpireFn onExpire_;
};

} // namespace perfacet
