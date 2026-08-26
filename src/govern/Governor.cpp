#include "perfacet/govern/Governor.h"

namespace perfacet {

struct Governor::Permit::Impl {
    Governor* gov = nullptr;
    ir::ToolKey key;
    std::string agentId;
    bool held = false;
};

Governor::Permit::Permit() = default;
Governor::Permit::Permit(Permit&& o) noexcept = default;
Governor::Permit& Governor::Permit::operator=(Permit&& o) noexcept = default;

Governor::Permit::~Permit() {
    if (impl_ && impl_->held && impl_->gov) {
        impl_->held = false;
        impl_->gov->releaseSlot(impl_->key, impl_->agentId);
    }
}

bool Governor::Permit::held() const { return impl_ && impl_->held; }

Governor::Permit Governor::Permit::make(Governor* gov, ir::ToolKey key,
                                        std::string agentId) {
    Permit p;
    p.impl_ = std::make_unique<Impl>();
    p.impl_->gov = gov;
    p.impl_->key = std::move(key);
    p.impl_->agentId = std::move(agentId);
    p.impl_->held = true;
    return p;
}

} // namespace perfacet
