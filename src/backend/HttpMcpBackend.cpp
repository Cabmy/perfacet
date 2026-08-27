#include "perfacet/backend/HttpMcpBackend.h"
#include "backend/KeepAliveClient.h"
#include "detail/Time.h"
#include "perfacet/ir/ClientCaps.h"

#include <map>
#include <stdexcept>

namespace perfacet {

namespace {

void parseUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    std::string rest = url;
    if (rest.compare(0, 7, "http://") == 0) rest = rest.substr(7);
    else if (rest.compare(0, 8, "https://") == 0) {
        throw std::runtime_error("M1 不支持 TLS 上游: " + url);
    }
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = hostport.rfind(':');
    if (colon == std::string::npos) {
        host = hostport;
        port = 80;
    } else {
        host = hostport.substr(0, colon);
        port = std::stoi(hostport.substr(colon + 1));
    }
}

std::string traceparent(const ir::TraceContext& t) {
    if (t.traceId.empty() || t.spanId.empty()) return {};
    return "00-" + t.traceId + "-" + t.spanId + "-01";
}

} // namespace

HttpMcpBackend::HttpMcpBackend(std::string url, netlib::EventLoop* loop,
                               netlib::ThreadPool* pool)
    : url_(std::move(url)), loop_(loop), pool_(pool) {
    parseUrl(url_, host_, port_, path_);
}

ir::Response HttpMcpBackend::callBlocking(const ir::BackendCall& bc) {
    ir::Response out;
    out.upstreamId = "1";
    const uint64_t t0 = nowMs();
    if (bc.deadlineMs > 0 && nowMs() >= bc.deadlineMs) {
        out.isError = true;
        out.klass = ir::FailureClass::Timeout;
        out.body = ir::jsonRpcError(-32000, "upstream deadline");
        out.upstreamMs = 0;
        return out;
    }

    int timeoutSec = 30;
    if (bc.deadlineMs > 0) {
        const uint64_t rem = bc.deadlineMs - nowMs();
        timeoutSec = static_cast<int>((rem + 999) / 1000);
        if (timeoutSec < 1) timeoutSec = 1;
    }

    ir::Json meta = bc.meta.is_object() ? bc.meta : ir::Json::object();
    meta[ir::kMetaProtocol] = ir::kProtocolVersion;
    if (!meta.contains(ir::kMetaCaps)) {
        meta[ir::kMetaCaps] = ir::Json::object();
    }
    // 不对上游声明 tasks（不 re-attach remoteTaskId）
    ir::Json params = bc.params.is_object() ? bc.params : ir::Json::object();
    params["_meta"] = meta;
    if (bc.method == "tools/call" && !bc.name.empty()) {
        params["name"] = bc.name;
    }
    ir::Json body{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", bc.method},
        {"params", params},
    };

    std::map<std::string, std::string> hdr{
        {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", ir::kProtocolVersion},
        {"Mcp-Method", bc.method},
    };
    if (bc.method == "tools/call" || bc.method.rfind("tasks/", 0) == 0) {
        hdr.emplace("Mcp-Name", bc.name);
    }
    auto tp = traceparent(bc.trace);
    if (!tp.empty()) hdr.emplace("traceparent", tp);

    const KeepAlivePost res =
        keepAlivePost(host_, port_, path_, hdr, body.dump(), timeoutSec, timeoutSec);
    out.upstreamMs = nowMs() - t0;
    if (!res.transportOk) {
        out.isError = true;
        out.klass = res.timeout ? ir::FailureClass::Timeout : ir::FailureClass::Unavailable;
        out.body = ir::jsonRpcError(-32000, "upstream unreachable");
        return out;
    }
    if (res.status >= 500) {
        out.isError = true;
        out.klass = ir::FailureClass::Unavailable;
        out.body = ir::jsonRpcError(-32000, "upstream 5xx");
        return out;
    }
    auto parsed = ir::Json::parse(res.body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        out.isError = true;
        out.klass = ir::FailureClass::Protocol;
        out.body = ir::jsonRpcError(-32700, "upstream not json-rpc");
        return out;
    }
    if (parsed.contains("error")) {
        out.isError = true;
        out.body = parsed["error"];
        out.klass = ir::classify(out, {});
        return out;
    }
    out.isError = false;
    out.klass = ir::FailureClass::Ok;
    out.body = parsed.contains("result") ? parsed["result"] : parsed;
    return out;
}

void HttpMcpBackend::call(const ir::BackendCall& bc,
                          std::function<void(ir::Response)> cb) {
    auto failQueue = [this, cb]() {
        ir::Response r;
        r.isError = true;
        r.klass = ir::FailureClass::Unavailable;
        r.body = ir::jsonRpcError(-32000, "worker queue full");
        loop_->queueInLoop([cb, r = std::move(r)]() mutable { cb(std::move(r)); });
    };
    if (pool_->full()) {
        failQueue();
        return;
    }
    try {
        pool_->add([this, bc, cb = std::move(cb)]() mutable {
            ir::Response r = callBlocking(bc);
            loop_->queueInLoop([cb = std::move(cb), r = std::move(r)]() mutable { cb(std::move(r)); });
        });
    } catch (...) {
        failQueue();
    }
}

} // namespace perfacet
