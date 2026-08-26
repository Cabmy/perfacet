#include "perfacet/pipeline/InFlight.h"
#include "detail/Random.h"

namespace perfacet {

InFlight::InFlight(Counters* counters) : counters_(counters) {}

std::string InFlight::mapKey(std::string_view agent, std::string_view tool,
                             std::string_view hash) const {
    std::string k;
    k.reserve(agent.size() + tool.size() + hash.size() + 2);
    k.append(agent);
    k.push_back('\n');
    k.append(tool);
    k.push_back('\n');
    k.append(hash);
    return k;
}

std::optional<InFlight::Entry> InFlight::lookup(std::string_view agentId,
                                                const ir::ToolKey& key,
                                                std::string_view paramsHash) const {
    auto it = keyToId_.find(mapKey(agentId, key.str(), paramsHash));
    if (it == keyToId_.end()) return std::nullopt;
    auto e = byId_.find(it->second);
    if (e == byId_.end()) return std::nullopt;
    return e->second;
}

std::string InFlight::insert(std::string agentId, ir::ToolKey key, std::string paramsHash,
                             std::string summary, uint64_t nowMs) {
    Entry e;
    e.inflightId = randomHex(16);
    e.startedAtMs = nowMs;
    e.paramsSummary = std::move(summary);
    e.key = std::move(key);
    e.agentId = std::move(agentId);
    e.paramsHash = std::move(paramsHash);
    const std::string mk = mapKey(e.agentId, e.key.str(), e.paramsHash);
    keyToId_[mk] = e.inflightId;
    std::string id = e.inflightId;
    byId_[id] = std::move(e);
    if (counters_) counters_->inflightHeld.fetch_add(1);
    return id;
}

void InFlight::setTaskId(const std::string& inflightId, std::string taskId) {
    auto it = byId_.find(inflightId);
    if (it == byId_.end()) return;
    it->second.taskId = std::move(taskId);
}

void InFlight::erase(const std::string& inflightId) {
    auto it = byId_.find(inflightId);
    if (it == byId_.end()) return;
    keyToId_.erase(mapKey(it->second.agentId, it->second.key.str(), it->second.paramsHash));
    byId_.erase(it);
    if (counters_) counters_->inflightHeld.fetch_sub(1);
}

} // namespace perfacet
