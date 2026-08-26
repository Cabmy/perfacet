#pragma once
// loop 投递 struct，worker append 一行。抽 AuditLog 时替换本类。
#include "perfacet/ir/Request.h"

#include "netlib/ThreadPool.h"

#include <cstdint>
#include <string>

namespace perfacet {

struct AuditEvent {
    std::string event; // 闭集：auth_fail/deny/throttled/inflight_hit/circuit_open/grant_approve/grant_expire
    uint64_t tsMs = 0;
    std::string traceId;
    std::string principal;
    std::string level;
    std::string tool;
    std::string server;
    std::string status;
};

class JsonlAuditLog {
public:
    JsonlAuditLog(std::string path, netlib::ThreadPool* pool);
    void emit(AuditEvent ev);

private:
    std::string path_;
    netlib::ThreadPool* pool_;
};

} // namespace perfacet
