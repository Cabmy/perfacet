#pragma once
#include "perfacet/ir/Request.h"

#include <cstdint>
#include <string>

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

} // namespace perfacet
