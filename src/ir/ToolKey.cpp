#include "perfacet/ir/ToolKey.h"

namespace perfacet::ir {

std::optional<ToolKey> ToolKey::parse(std::string_view s) {
    auto pos = s.find("__");
    if (pos == std::string_view::npos || pos == 0 || pos + 2 >= s.size()) {
        return std::nullopt;
    }
    ToolKey k;
    k.backend.assign(s.substr(0, pos));
    k.tool.assign(s.substr(pos + 2));
    return k;
}

std::string ToolKey::str() const {
    return backend + "__" + tool;
}

} // namespace perfacet::ir
