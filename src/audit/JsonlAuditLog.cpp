#include "perfacet/audit/JsonlAuditLog.h"
#include "detail/Time.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <fstream>

namespace perfacet {

JsonlAuditLog::JsonlAuditLog(std::string path, netlib::ThreadPool* pool)
    : path_(std::move(path)), pool_(pool) {}

void JsonlAuditLog::emit(AuditEvent ev) {
    if (ev.tsMs == 0) ev.tsMs = nowMs();
    pool_->add([path = path_, ev = std::move(ev)]() {
        nlohmann::json j{
            {"ts_ms", ev.tsMs},
            {"event", ev.event},
            {"trace_id", ev.traceId},
            {"principal", ev.principal},
            {"level", ev.level},
            {"tool", ev.tool},
            {"server", ev.server},
            {"status", ev.status},
        };
        std::ofstream out(path, std::ios::app);
        out << j.dump() << '\n';
    });
}

} // namespace perfacet
