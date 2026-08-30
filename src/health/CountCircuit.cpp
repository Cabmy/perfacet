#include "perfacet/health/CountCircuit.h"

namespace perfacet {

CountCircuit::CountCircuit(int openAfter, uint64_t cooldownMs, int halfOpenProbes,
                           netlib::EventLoop* loop)
    : openAfter_(openAfter), cooldownMs_(cooldownMs), halfOpenProbes_(halfOpenProbes),
      loop_(loop) {}

CountCircuit::Slot& CountCircuit::slot(const std::string& server) {
    return slots_[server];
}

void CountCircuit::maybeHalfOpen(Slot& s, uint64_t nowMs) {
    if (s.st == CState::Open && nowMs >= s.openUntilMs) {
        s.st = CState::HalfOpen;
        s.halfOk = 0;
    }
}

bool CountCircuit::isOpen(const std::string& server, uint64_t now) {
    return state(server, now) == CState::Open;
}

CountCircuit::CState CountCircuit::state(const std::string& server, uint64_t now) {
    auto& s = slot(server);
    maybeHalfOpen(s, now);
    return s.st;
}

void CountCircuit::onSuccess(const std::string& server) {
    auto& s = slot(server);
    if (s.st == CState::HalfOpen) {
        s.halfOk++;
        if (s.halfOk >= halfOpenProbes_) {
            s.st = CState::Closed;
            s.fails = 0;
        }
        return;
    }
    s.fails = 0;
    s.st = CState::Closed;
}

bool CountCircuit::onFailure(const std::string& server, uint64_t now) {
    auto& s = slot(server);
    maybeHalfOpen(s, now);
    if (s.st == CState::HalfOpen) {
        s.st = CState::Open;
        s.openUntilMs = now + cooldownMs_;
        s.fails = 0;
        return true;
    }
    s.fails++;
    if (s.fails >= openAfter_ && s.st != CState::Open) {
        s.st = CState::Open;
        s.openUntilMs = now + cooldownMs_;
        return true;
    }
    return false;
}

void CountCircuit::onProbeSuccess(const std::string& server) {
    auto& s = slot(server);
    // OPEN 只走 cooldown → HALF_OPEN，探活不得直接合闸。
    if (s.st == CState::HalfOpen) {
        s.halfOk++;
        if (s.halfOk >= halfOpenProbes_) {
            s.st = CState::Closed;
            s.fails = 0;
            s.halfOk = 0;
        }
        return;
    }
    if (s.st == CState::Closed) s.fails = 0;
}

} // namespace perfacet
