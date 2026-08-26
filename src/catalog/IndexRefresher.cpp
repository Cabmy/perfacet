#include "perfacet/catalog/IndexRefresher.h"

namespace perfacet {

IndexRefresher::IndexRefresher(ToolIndex& index) : index_(&index) {}

void IndexRefresher::onProbeResult(const std::string& server, Health::State,
                                   const std::optional<ir::Json>& toolList) {
    if (!toolList || !toolList->is_object()) return;
    if (!toolList->contains("tools") || !(*toolList)["tools"].is_array()) return;
    std::vector<IndexedTool> tools;
    std::vector<std::string> names;
    for (const auto& t : (*toolList)["tools"]) {
        if (!t.is_object() || !t.contains("name")) continue;
        IndexedTool it;
        it.name = t["name"].get<std::string>();
        it.description = t.value("description", "");
        it.inputSchema = t.contains("inputSchema") ? t["inputSchema"] : ir::Json::object();
        names.push_back(it.name);
        tools.push_back(std::move(it));
    }
    index_->replace(server, std::move(tools));
    if (onListed_) onListed_(server, names);
}

} // namespace perfacet
