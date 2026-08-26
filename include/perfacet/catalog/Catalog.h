#pragma once
// name → Backend + BackendMeta。不带 Principal。
#include "perfacet/backend/Backend.h"
#include "perfacet/ir/Request.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace perfacet {

struct CatalogEntry {
    std::string name;
    std::string url;
    ir::BackendMeta meta;
    std::unique_ptr<Backend> backend;
};

class Catalog {
public:
    void add(CatalogEntry entry);
    CatalogEntry* find(std::string_view name);
    const CatalogEntry* find(std::string_view name) const;
    std::vector<std::string> names() const;
    Backend* backend(std::string_view name);

private:
    std::unordered_map<std::string, CatalogEntry> byName_;
};

} // namespace perfacet
