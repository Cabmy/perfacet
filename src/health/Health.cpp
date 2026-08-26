#include "perfacet/health/Health.h"

namespace perfacet {

const char* healthStateName(Health::State s) {
    switch (s) {
    case Health::State::Up:
        return "up";
    case Health::State::Degraded:
        return "degraded";
    case Health::State::Down:
        return "down";
    }
    return "down";
}

} // namespace perfacet
