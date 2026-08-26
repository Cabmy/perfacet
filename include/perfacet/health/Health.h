#pragma once
#include <cstdint>
#include <string>

namespace perfacet {

class Health {
public:
    enum class State { Up, Degraded, Down };
    virtual ~Health() = default;
    virtual State state(const std::string& server) const = 0;
    virtual uint64_t latencyEwmaMs(const std::string& server) const = 0;
};

const char* healthStateName(Health::State s);

} // namespace perfacet
