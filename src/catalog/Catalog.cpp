#include "perfacet/catalog/Catalog.h"

namespace perfacet {

void Catalog::add(CatalogEntry entry) {
    std::string name = entry.name;
    byName_[name] = std::move(entry);
}

CatalogEntry* Catalog::find(std::string_view name) {
    auto it = byName_.find(std::string(name));
    return it == byName_.end() ? nullptr : &it->second;
}

const CatalogEntry* Catalog::find(std::string_view name) const {
    auto it = byName_.find(std::string(name));
    return it == byName_.end() ? nullptr : &it->second;
}

std::vector<std::string> Catalog::names() const {
    std::vector<std::string> out;
    out.reserve(byName_.size());
    for (const auto& kv : byName_) out.push_back(kv.first);
    return out;
}

Backend* Catalog::backend(std::string_view name) {
    auto* e = find(name);
    return e ? e->backend.get() : nullptr;
}

} // namespace perfacet
