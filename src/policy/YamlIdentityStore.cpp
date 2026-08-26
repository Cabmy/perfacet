#include "perfacet/policy/YamlIdentityStore.h"

namespace perfacet {

YamlIdentityStore::YamlIdentityStore(const YamlConfig& cfg) {
    for (const auto& a : cfg.agents) {
        byToken_[a.token] = a;
    }
    for (auto& kv : byToken_) {
        if (kv.second.admin) {
            admin_ = &kv.second;
            break;
        }
    }
}

std::optional<ir::Principal> YamlIdentityStore::authenticate(std::string_view bearer) const { // PERFACET_LAYER_ALLOW
    auto it = byToken_.find(std::string(bearer));
    if (it == byToken_.end()) return std::nullopt;
    const auto& a = it->second;
    ir::Principal p;
    p.agentId = a.id;
    p.level = a.level;
    p.hasLevel = a.hasLevel;
    p.admin = a.admin;
    p.levelName = a.levelName;
    p.grantBump = 0;
    return p;
}

const AgentCfg* YamlIdentityStore::adminAgent() const { return admin_; }

} // namespace perfacet
