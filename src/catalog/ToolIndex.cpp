#include "perfacet/catalog/ToolIndex.h"

namespace perfacet {

void ToolIndex::replace(const std::string& backend, std::vector<IndexedTool> tools) {
    byBackend_[backend] = std::move(tools);
    anyWrite_ = true;
}

const std::vector<IndexedTool>* ToolIndex::toolsOf(const std::string& backend) const {
    auto it = byBackend_.find(backend);
    return it == byBackend_.end() ? nullptr : &it->second;
}

bool ToolIndex::everWritten(const std::string& backend) const {
    return byBackend_.count(backend) > 0;
}

bool ToolIndex::cold() const { return !anyWrite_; }

std::vector<std::string> ToolIndex::backends() const {
    std::vector<std::string> out;
    for (const auto& kv : byBackend_) out.push_back(kv.first);
    return out;
}

} // namespace perfacet
