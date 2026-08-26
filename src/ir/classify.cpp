#include "perfacet/ir/Request.h"
#include "perfacet/ir/classify.h"

#include <array>

namespace perfacet::ir {

namespace {

struct Row {
    FailureClass k;
    const char* name;
};

constexpr Row kRows[] = {
    {FailureClass::Ok, "Ok"},
    {FailureClass::Cancelled, "Cancelled"},
    {FailureClass::Timeout, "Timeout"},
    {FailureClass::Unavailable, "Unavailable"},
    {FailureClass::Throttled, "Throttled"},
    {FailureClass::Protocol, "Protocol"},
    {FailureClass::Capability, "Capability"},
    {FailureClass::Upstream, "Upstream"},
    {FailureClass::Authz, "Authz"},
    {FailureClass::Internal, "Internal"},
};

} // namespace

const char* failureClassName(FailureClass k) {
    for (const auto& r : kRows) {
        if (r.k == k) return r.name;
    }
    return "Internal";
}

std::optional<FailureClass> failureClassFromName(std::string_view name) {
    for (const auto& r : kRows) {
        if (name == r.name) return r.k;
    }
    return std::nullopt;
}

FailureClass classify(const Response& r, std::error_code ec) {
    if (ec) {
        if (ec == std::errc::timed_out) return FailureClass::Timeout;
        if (ec == std::errc::connection_refused ||
            ec == std::errc::host_unreachable ||
            ec == std::errc::network_unreachable) {
            return FailureClass::Unavailable;
        }
        if (ec == std::errc::operation_canceled) return FailureClass::Cancelled;
    }
    if (r.klass != FailureClass::Ok) return r.klass;
    if (!r.isError) return FailureClass::Ok;
    if (r.body.is_object() && r.body.contains("code") && r.body["code"].is_number_integer()) {
        const int code = r.body["code"].get<int>();
        if (code == -32021) return FailureClass::Capability;
        if (code == -32600 || code == -32601 || code == -32602 || code == -32700) {
            return FailureClass::Protocol;
        }
    }
    return FailureClass::Upstream;
}

Json callToolText(std::string_view text, bool isError) {
    return Json{
        {"content", Json::array({Json{{"type", "text"}, {"text", std::string(text)}}})},
        {"isError", isError},
    };
}

Json jsonRpcError(int code, std::string_view msg, Json data) {
    Json err{{"code", code}, {"message", std::string(msg)}};
    if (!data.is_null() && !data.empty()) err["data"] = std::move(data);
    return err;
}

} // namespace perfacet::ir
