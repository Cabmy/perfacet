#pragma once
// ToolIndex ⨯ Policy::visibilityFilter。可见性不读 Health / Circuit。
#include "perfacet/catalog/ToolIndex.h"
#include "perfacet/ir/Request.h"
#include "perfacet/policy/Policy.h"

namespace perfacet {

class FacetView {
public:
    FacetView(const ToolIndex& index, Policy& policy);
    ir::Json listTools(const ir::Principal& who) const;

private:
    const ToolIndex* index_;
    Policy* policy_;
};

} // namespace perfacet
