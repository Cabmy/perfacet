#pragma once
#include <cstdint>
#include <chrono>

namespace perfacet {

inline uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace perfacet
