#pragma once
// 吃 onProbeResult：成功则替换 ToolIndex 该 backend 条目，并校验 governor.tools。
#include "perfacet/catalog/ToolIndex.h"
#include "perfacet/health/Health.h"
#include "perfacet/ir/Request.h"

#include <functional>
#include <optional>
#include <string>

namespace perfacet {

class IndexRefresher {
public:
    using ListedFn = std::function<void(const std::string& backend,
                                        const std::vector<std::string>& tools)>;

    explicit IndexRefresher(ToolIndex& index);

    void setOnListed(ListedFn fn) { onListed_ = std::move(fn); }

    void onProbeResult(const std::string& server, Health::State state,
                       const std::optional<ir::Json>& toolList);

private:
    ToolIndex* index_;
    ListedFn onListed_;
};

} // namespace perfacet
