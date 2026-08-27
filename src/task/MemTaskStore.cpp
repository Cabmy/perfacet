#include "perfacet/task/MemTaskStore.h"
#include "detail/Time.h"

namespace perfacet {

MemTaskStore::MemTaskStore(std::size_t maxTasks) : maxTasks_(maxTasks == 0 ? 1 : maxTasks) {}

void MemTaskStore::sweep(uint64_t now) const {
    for (auto it = byId_.begin(); it != byId_.end();) {
        const auto& t = it->second;
        if (t.ttlMs > 0 && now >= t.createdAtMs + t.ttlMs) {
            it = byId_.erase(it);
        } else {
            ++it;
        }
    }
}

bool MemTaskStore::insert(Task t) {
    sweep(nowMs());
    if (byId_.size() >= maxTasks_ && byId_.find(t.taskId) == byId_.end()) return false;
    byId_.insert_or_assign(t.taskId, std::move(t));
    return true;
}

std::optional<Task> MemTaskStore::get(std::string_view id) const {
    sweep(nowMs());
    auto it = byId_.find(std::string(id));
    if (it == byId_.end()) return std::nullopt;
    return it->second;
}

void MemTaskStore::updateStatus(std::string_view id, std::string status, ir::Json body,
                                uint64_t now) {
    sweep(now);
    auto it = byId_.find(std::string(id));
    if (it == byId_.end()) return;
    it->second.status = std::move(status);
    it->second.resultOrError = std::move(body);
    it->second.lastUpdatedAtMs = now;
}

} // namespace perfacet
