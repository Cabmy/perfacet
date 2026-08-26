#include "perfacet/task/MemTaskStore.h"

namespace perfacet {

void MemTaskStore::insert(Task t) { byId_.insert_or_assign(t.taskId, std::move(t)); }

std::optional<Task> MemTaskStore::get(std::string_view id) const {
    auto it = byId_.find(std::string(id));
    if (it == byId_.end()) return std::nullopt;
    return it->second;
}

void MemTaskStore::updateStatus(std::string_view id, std::string status, ir::Json body,
                                uint64_t nowMs) {
    auto it = byId_.find(std::string(id));
    if (it == byId_.end()) return;
    it->second.status = std::move(status);
    it->second.resultOrError = std::move(body);
    it->second.lastUpdatedAtMs = nowMs;
}

} // namespace perfacet
