#pragma once
// 权限档位全序链：数组下标即 Rank。偏序/格不是本产品模型。
#include "perfacet/ir/Request.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace perfacet {

class Taxonomy {
public:
    explicit Taxonomy(std::vector<std::string> levels);

    std::optional<ir::Rank> parse(std::string_view name) const;
    const std::string& nameOf(ir::Rank r) const;
    ir::Rank maxRank() const {
        return levels_.empty() ? 0 : static_cast<ir::Rank>(levels_.size() - 1);
    }
    std::size_t size() const { return levels_.size(); }
    const std::vector<std::string>& levels() const { return levels_; }

private:
    std::vector<std::string> levels_;
    std::unordered_map<std::string, ir::Rank> byName_;
};

} // namespace perfacet
