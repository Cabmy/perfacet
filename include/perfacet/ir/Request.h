#pragma once
// IR 是跨层唯一货币：不认 HTTP，不带 agent token。
#include "perfacet/ir/ClientCaps.h"
#include "perfacet/ir/ToolKey.h"
#include "perfacet/ir/classify.h"

#include <cstdint>
#include <string>
#include <vector>

namespace perfacet::ir {

using Rank = uint16_t;

struct Principal {
    std::string agentId;
    Rank        level = 0;        // 仅 hasLevel 时有意义
    Rank        grantBump = 0;    // 读时求值
    bool        hasLevel = false; // 默认 false；仅认证成功才为 true
    bool        admin = false;    // 控制面开关，不是一档
    std::string levelName;        // 仅 span / 审计
};

inline Rank effectiveRank(const Principal& w) {
    return w.level > w.grantBump ? w.level : w.grantBump;
}

inline bool visible(const Principal& who, Rank mcpLevel) {
    return who.hasLevel && effectiveRank(who) >= mcpLevel;
}

struct BackendMeta {
    Rank level = 0;
    bool secret = false;
    std::vector<std::string> idempotentTools;
};

struct TraceContext {
    std::string traceId, spanId, parentSpanId;
};

struct Request {
    std::string  method;      // Mcp-Method
    std::string  name;        // tools/call: ToolKey::str(); tasks/*: taskId
    std::string  upstreamId;  // JSON-RPC id 的字符串化（回显用原文存在 idJson）
    Json         idJson = nullptr;
    Json         params;
    Json         meta;        // 可含 perfacet/confirm
    Principal    who;
    TraceContext trace;
    uint64_t     deadlineMs = 0; // 绝对 Unix ms
    ClientCaps   caps;
};

// Backend 只吃这个，不含 Principal。
struct BackendCall {
    std::string  method, name; // name 是上游工具名（不含 backend__ 前缀）
    Json         params, meta; // 已剥 perfacet/confirm
    uint64_t     deadlineMs = 0;
    TraceContext trace;
};

struct Response {
    std::string   upstreamId;
    Json          body;       // JSON-RPC result 或 error 对象
    bool          isError = false;
    FailureClass  klass = FailureClass::Ok;
    uint64_t      gatewayMs = 0;
    uint64_t      upstreamMs = 0;
    int           httpStatus = 200; // Frontend 写 HTTP 时用
};

Json callToolText(std::string_view text, bool isError = false);
Json jsonRpcError(int code, std::string_view msg, Json data = Json::object());

} // namespace perfacet::ir
