#pragma once
#include "perfacet/ir/Request.h"

#include <functional>

namespace perfacet {

class Backend {
public:
    virtual ~Backend() = default;
    virtual void call(const ir::BackendCall&,
                      std::function<void(ir::Response)>) = 0;
};

} // namespace perfacet
