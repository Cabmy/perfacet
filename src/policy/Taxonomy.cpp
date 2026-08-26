#include "perfacet/policy/Taxonomy.h"

#include <stdexcept>

namespace perfacet {

Taxonomy::Taxonomy(std::vector<std::string> levels) : levels_(std::move(levels)) {
    if (levels_.empty()) {
        throw std::runtime_error("access.levels 不能为空");
    }
    for (ir::Rank i = 0; i < levels_.size(); ++i) {
        if (levels_[i].empty()) {
            throw std::runtime_error("access.levels 含空名字");
        }
        if (!byName_.emplace(levels_[i], i).second) {
            throw std::runtime_error("access.levels 重复: " + levels_[i]);
        }
    }
}

std::optional<ir::Rank> Taxonomy::parse(std::string_view name) const {
    auto it = byName_.find(std::string(name));
    if (it == byName_.end()) return std::nullopt;
    return it->second;
}

const std::string& Taxonomy::nameOf(ir::Rank r) const {
    return levels_.at(r);
}

} // namespace perfacet
