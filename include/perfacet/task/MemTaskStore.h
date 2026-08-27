#pragma once
// 进程内任务表。持久化时抽 TaskStore + JSONL/SQLite 即满足 SEP-2663 MUST。
#include "perfacet/ir/Request.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace perfacet {

struct Task {
    std::string taskId;
    std::string agentId;
    ir::ToolKey key;
    std::string status; // working|completed|failed|cancelled
    ir::Json resultOrError;
    uint64_t createdAtMs = 0;
    uint64_t lastUpdatedAtMs = 0;
    uint64_t ttlMs = 0;
    ir::Principal owner;

    explicit Task(ir::Principal o) : owner(std::move(o)) {}
};

class MemTaskStore {
public:
    explicit MemTaskStore(std::size_t maxTasks = 10000);

    // 满则 false，不插入。
    bool insert(Task t);
    std::optional<Task> get(std::string_view id) const;
    void updateStatus(std::string_view id, std::string status, ir::Json body,
                      uint64_t nowMs);
    std::size_t size() const { return byId_.size(); }

private:
    void sweep(uint64_t nowMs) const;

    std::size_t maxTasks_;
    mutable std::unordered_map<std::string, Task> byId_;
};

} // namespace perfacet
