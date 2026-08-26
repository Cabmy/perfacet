#pragma once
// Rank 比较 + 内置工具分流。出厂一档仍走本类，没有旁路。
#include "perfacet/catalog/Catalog.h"
#include "perfacet/policy/Policy.h"

namespace perfacet {

class RankPolicy : public Policy {
public:
    explicit RankPolicy(const Catalog& catalog);
    Decision authorizeCall(const ir::Principal&, const ir::ToolKey&) override;
    std::function<bool(const ir::ToolKey&)>
    visibilityFilter(const ir::Principal&) override;

    static Decision builtinDecision(const ir::Principal&, const ir::ToolKey&);

private:
    const Catalog* catalog_;
};

} // namespace perfacet
