#pragma once
// last-known-good 工具快照。只由 IndexRefresher 在 probe 成功时写；失败不抹。
#include "perfacet/ir/Request.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace perfacet {

struct IndexedTool {
    std::string name; // 上游工具名，不含 backend__
    std::string description;
    ir::Json inputSchema = ir::Json::object();
};

class ToolIndex {
public:
    void replace(const std::string& backend, std::vector<IndexedTool> tools);
    const std::vector<IndexedTool>* toolsOf(const std::string& backend) const;
    bool everWritten(const std::string& backend) const;
    bool cold() const; // 从未成功写入过任何 backend
    std::vector<std::string> backends() const;

private:
    std::unordered_map<std::string, std::vector<IndexedTool>> byBackend_;
    bool anyWrite_ = false;
};

} // namespace perfacet
