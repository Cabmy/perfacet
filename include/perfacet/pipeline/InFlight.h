#pragma once
// 同身份同参数在途去重。不抽接口；与 Call 同一生命周期。
#include "perfacet/ir/Request.h"
#include "perfacet/observe/Counters.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace perfacet {

class InFlight {
public:
    struct Entry {
        std::string inflightId;
        uint64_t startedAtMs = 0;
        std::optional<std::string> taskId;
        std::string paramsSummary;
        ir::ToolKey key;
        std::string agentId;
        std::string paramsHash;
    };

    explicit InFlight(Counters* counters);

    std::optional<Entry> lookup(std::string_view agentId, const ir::ToolKey& key,
                                std::string_view paramsHash) const;

    std::string insert(std::string agentId, ir::ToolKey key, std::string paramsHash,
                       std::string summary, uint64_t nowMs);
    void setTaskId(const std::string& inflightId, std::string taskId);
    void erase(const std::string& inflightId);
    int held() const { return static_cast<int>(byId_.size()); }

private:
    std::string mapKey(std::string_view agent, std::string_view tool,
                       std::string_view hash) const;

    Counters* counters_;
    std::unordered_map<std::string, Entry> byId_;
    std::unordered_map<std::string, std::string> keyToId_;
};

} // namespace perfacet
