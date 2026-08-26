#pragma once
#include "perfacet/ir/Request.h"

namespace perfacet {

class Tracer {
public:
    virtual ~Tracer() = default;
    virtual ir::TraceContext start(const ir::Request&, const char* spanName) = 0;
    virtual void set(const ir::TraceContext&, const char* key,
                     const std::string& value) = 0;
    virtual void end(const ir::TraceContext&, ir::FailureClass,
                     uint64_t latencyMs) = 0;
};

} // namespace perfacet
