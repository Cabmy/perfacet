#pragma once
// 进程内任务表。持久化时抽 TaskStore + JSONL/SQLite 即满足 SEP-2663 MUST。
#include "perfacet/ir/Request.h"

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
    void insert(Task t);
    std::optional<Task> get(std::string_view id) const;
    void updateStatus(std::string_view id, std::string status, ir::Json body,
                      uint64_t nowMs);

private:
    std::unordered_map<std::string, Task> byId_;
};

} // namespace perfacet
