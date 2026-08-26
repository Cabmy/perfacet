#include "perfacet/policy/RankPolicy.h"

namespace perfacet {

RankPolicy::RankPolicy(const Catalog& catalog) : catalog_(&catalog) {}

Decision RankPolicy::builtinDecision(const ir::Principal& who, const ir::ToolKey& key) {
    if (key.tool == "request_elevation") return Decision::Allow; // list 可见；call 另判 hasLevel
    if (key.tool == "upstream_status") {
        return who.admin ? Decision::Allow : Decision::Deny;
    }
    return who.admin ? Decision::Allow : Decision::Unknown;
}

Decision RankPolicy::authorizeCall(const ir::Principal& who, const ir::ToolKey& key) {
    if (key.backend == "perfacet") return builtinDecision(who, key);
    const CatalogEntry* e = catalog_->find(key.backend);
    if (!e) return Decision::Unknown;
    if (!who.hasLevel) return Decision::Deny;
    if (ir::effectiveRank(who) < e->meta.level) {
        return e->meta.secret ? Decision::Unknown : Decision::Deny;
    }
    return Decision::Allow;
}

std::function<bool(const ir::ToolKey&)> RankPolicy::visibilityFilter(const ir::Principal& who) {
    return [this, who](const ir::ToolKey& k) {
        return authorizeCall(who, k) == Decision::Allow;
    };
}

} // namespace perfacet
