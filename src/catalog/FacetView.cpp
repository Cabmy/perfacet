#include "perfacet/catalog/FacetView.h"

namespace perfacet {

FacetView::FacetView(const ToolIndex& index, Policy& policy)
    : index_(&index), policy_(&policy) {}

ir::Json FacetView::listTools(const ir::Principal& who) const {
    auto vis = policy_->visibilityFilter(who);
    ir::Json tools = ir::Json::array();
    for (const auto& backend : index_->backends()) {
        const auto* list = index_->toolsOf(backend);
        if (!list) continue;
        for (const auto& t : *list) {
            ir::ToolKey k{backend, t.name};
            if (!vis(k)) continue;
            ir::Json item{{"name", k.str()},
                          {"description", t.description},
                          {"inputSchema", t.inputSchema.is_null() ? ir::Json::object()
                                                                  : t.inputSchema}};
            tools.push_back(std::move(item));
        }
    }
    for (const char* tool : {"request_elevation", "upstream_status"}) {
        ir::ToolKey k{"perfacet", tool};
        if (!vis(k)) continue;
        if (k.tool == "request_elevation") {
            tools.push_back(ir::Json{
                {"name", k.str()},
                {"description", "申请临时提权（CLI 审批 + TTL）"},
                {"inputSchema",
                 ir::Json{{"type", "object"},
                          {"properties", ir::Json{{"bump_to", ir::Json{{"type", "string"}}}}},
                          {"required", ir::Json::array({"bump_to"})}}},
            });
        } else {
            tools.push_back(ir::Json{
                {"name", k.str()},
                {"description", "上游健康快照（控制面）"},
                {"inputSchema", ir::Json{{"type", "object"}}},
            });
        }
    }
    return tools;
}

} // namespace perfacet
