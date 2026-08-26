#pragma once
#include "perfacet/ir/Request.h"

#include <functional>

namespace perfacet {

enum class Decision { Allow, Deny, Unknown }; // Unknown = secret 不够格的对外形状

class Policy {
public:
    virtual ~Policy() = default;
    virtual Decision authorizeCall(const ir::Principal&, const ir::ToolKey&) = 0;
    virtual std::function<bool(const ir::ToolKey&)>
    visibilityFilter(const ir::Principal&) = 0;
};

} // namespace perfacet
