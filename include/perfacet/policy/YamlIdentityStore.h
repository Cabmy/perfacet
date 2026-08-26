#pragma once
// YAML agents → Principal。失败返回 nullopt；禁止把 token 写入日志。
#include "perfacet/ir/Request.h"
#include "perfacet/policy/YamlConfig.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace perfacet {

class YamlIdentityStore {
public:
    explicit YamlIdentityStore(const YamlConfig& cfg);
    std::optional<ir::Principal> authenticate(std::string_view bearer) const; // PERFACET_LAYER_ALLOW

    const AgentCfg* adminAgent() const;

private:
    std::unordered_map<std::string, AgentCfg> byToken_;
    const AgentCfg* admin_ = nullptr;
};

} // namespace perfacet
