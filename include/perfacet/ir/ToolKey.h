#pragma once
// ToolKey：对 agent 暴露的工具名永远是 backend__tool。
// 第一个 "__" 切开；backend 名加载期禁止含 "__"。
#include <optional>
#include <string>
#include <string_view>

namespace perfacet::ir {

struct ToolKey {
    std::string backend;
    std::string tool;

    static std::optional<ToolKey> parse(std::string_view s);
    std::string str() const; // backend + "__" + tool

    bool operator==(const ToolKey& o) const {
        return backend == o.backend && tool == o.tool;
    }
};

} // namespace perfacet::ir
