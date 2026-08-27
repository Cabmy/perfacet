#pragma once
// 失败分类：重试只认 klass，Backend 禁止私自决定是否重试。
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace perfacet::ir {

using Json = nlohmann::json;

enum class FailureClass {
    Ok,
    Cancelled,    // 网关放弃等待；不保证远端终止
    Timeout,
    Unavailable,  // DOWN / OPEN / 连接失败
    Throttled,
    Protocol,
    Capability,   // -32021；不重试
    Upstream,
    Authz,
    Internal
};

const char* failureClassName(FailureClass k);
std::optional<FailureClass> failureClassFromName(std::string_view name);

struct Response;

// gatewayOwn：仅网关自身产生的 -32021 才是 Capability；上游同码是 Upstream。
FailureClass classify(const Response& r, std::error_code ec, bool gatewayOwn = false);

} // namespace perfacet::ir
