#pragma once
#include <atomic>
#include <cstdint>

namespace perfacet {

struct Counters {
    std::atomic<uint64_t> inflightHit{0};
    std::atomic<uint64_t> inflightConfirm{0};
    std::atomic<uint64_t> throttled{0};
    std::atomic<uint64_t> circuitOpen{0};
    std::atomic<uint64_t> otlpDropped{0};
    std::atomic<int> permitHeld{0};
    std::atomic<int> inflightHeld{0};

    struct Snap {
        uint64_t inflight_hit = 0, inflight_confirm = 0, throttled = 0,
                 circuit_open = 0, otlp_dropped = 0;
        int permit_held = 0, inflight_held = 0;
    };
    Snap snapshot() const {
        return Snap{inflightHit.load(), inflightConfirm.load(), throttled.load(),
                    circuitOpen.load(), otlpDropped.load(), permitHeld.load(),
                    inflightHeld.load()};
    }
};

} // namespace perfacet
