#pragma once
// 客户端能力只看本请求 _meta，不记忆 session。
#include <string_view>

namespace perfacet::ir {

inline constexpr const char* kProtocolVersion = "2026-07-28";
inline constexpr const char* kTasksExt = "io.modelcontextprotocol/tasks";
inline constexpr const char* kMetaProtocol =
    "io.modelcontextprotocol/protocolVersion";
inline constexpr const char* kMetaCaps =
    "io.modelcontextprotocol/clientCapabilities";
inline constexpr const char* kConfirmKey = "perfacet/confirm";

struct ClientCaps {
    bool tasks = false;
    // M1 只认 tasks 扩展名
    bool has(std::string_view ext) const {
        return ext == kTasksExt && tasks;
    }
};

} // namespace perfacet::ir
