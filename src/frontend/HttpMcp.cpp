#include "perfacet/frontend/HttpMcp.h"
#include "detail/Random.h"
#include "detail/Time.h"
#include "perfacet/catalog/Catalog.h"
#include "perfacet/govern/LocalGovernor.h"
#include "perfacet/health/ProbeHealth.h"
#include "perfacet/ir/ClientCaps.h"
#include "perfacet/observe/Counters.h"

#include "netlib/Endpoint.h"

#include <llhttp.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>

namespace perfacet {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string header(const std::map<std::string, std::string>& h, const char* k) {
    auto it = h.find(k);
    return it == h.end() ? std::string() : it->second;
}

void sendHttp(const netlib::TcpConnectionPtr& conn, int status, const char* reason,
              const std::string& body, const char* contentType = "application/json") {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "MCP-Protocol-Version: " << ir::kProtocolVersion << "\r\n"
        << "Connection: keep-alive\r\n\r\n"
        << body;
    conn->send(oss.str());
}

ir::Json wrapRpc(const ir::Json& id, const ir::Response& r) {
    ir::Json out{{"jsonrpc", "2.0"}, {"id", id}};
    if (r.isError) out["error"] = r.body;
    else out["result"] = r.body;
    return out;
}

int httpStatusFor(const ir::Response& r) {
    if (r.httpStatus != 200) return r.httpStatus;
    if (r.klass == ir::FailureClass::Capability) return 400;
    if (r.klass == ir::FailureClass::Protocol && r.isError) {
        if (r.body.is_object() && r.body.value("code", 0) == -32601) return 404;
        return 400;
    }
    if (r.klass == ir::FailureClass::Unavailable && r.isError && r.httpStatus == 503) return 503;
    return 200;
}

bool originOk(const YamlConfig& cfg, const std::string& origin) {
    if (origin.empty()) return true;
    for (const auto& a : cfg.originAllowlist) {
        if (a == "*" || a == origin) return true;
    }
    return false;
}

struct Session {
    HttpMcp* srv = nullptr;
    netlib::TcpConnection* conn = nullptr;
    llhttp_t parser{};
    llhttp_settings_t settings{};
    std::string url, method, field, value, body;
    std::map<std::string, std::string> headers;
    bool complete = false;
    bool busy = false;
};

int onUrl(llhttp_t* p, const char* at, size_t n) {
    auto* s = static_cast<Session*>(p->data);
    s->url.append(at, n);
    return 0;
}
int onMethod(llhttp_t* p, const char* at, size_t n) {
    auto* s = static_cast<Session*>(p->data);
    s->method.append(at, n);
    return 0;
}
int onHdrField(llhttp_t* p, const char* at, size_t n) {
    auto* s = static_cast<Session*>(p->data);
    s->field.append(at, n);
    return 0;
}
int onHdrValue(llhttp_t* p, const char* at, size_t n) {
    auto* s = static_cast<Session*>(p->data);
    s->value.append(at, n);
    return 0;
}
int onHdrValueComplete(llhttp_t* p) {
    auto* s = static_cast<Session*>(p->data);
    s->headers[lower(s->field)] = s->value;
    s->field.clear();
    s->value.clear();
    return 0;
}
int onBody(llhttp_t* p, const char* at, size_t n) {
    auto* s = static_cast<Session*>(p->data);
    s->body.append(at, n);
    return 0;
}
int onMsgComplete(llhttp_t* p) {
    auto* s = static_cast<Session*>(p->data);
    s->complete = true;
    return 0;
}

std::shared_ptr<Session> makeSession(HttpMcp* srv, netlib::TcpConnection* conn) {
    auto s = std::make_shared<Session>();
    s->srv = srv;
    s->conn = conn;
    llhttp_settings_init(&s->settings);
    s->settings.on_url = onUrl;
    s->settings.on_method = onMethod;
    s->settings.on_header_field = onHdrField;
    s->settings.on_header_value = onHdrValue;
    s->settings.on_header_value_complete = onHdrValueComplete;
    s->settings.on_body = onBody;
    s->settings.on_message_complete = onMsgComplete;
    llhttp_init(&s->parser, HTTP_REQUEST, &s->settings);
    s->parser.data = s.get();
    return s;
}

void resetSession(Session& s) {
    s.url.clear();
    s.method.clear();
    s.field.clear();
    s.value.clear();
    s.body.clear();
    s.headers.clear();
    s.complete = false;
    llhttp_reset(&s.parser);
    s.parser.data = &s;
}

} // namespace

HttpMcp::HttpMcp(netlib::EventLoop* loop, const YamlConfig& cfg, YamlIdentityStore& identity,
                 JsonlGrantStore& grants, Pipeline& pipeline, ProbeHealth* health,
                 Counters* counters, Catalog* catalog, LocalGovernor* governor,
                 JsonlAuditLog* audit)
    : loop_(loop), cfg_(&cfg), identity_(&identity), grants_(&grants), pipeline_(&pipeline),
      health_(health), counters_(counters), catalog_(catalog), governor_(governor),
      audit_(audit) {}

void HttpMcp::setStopping() { stopping_ = true; }

void HttpMcp::startListen() {
    if (listening_) return;
    netlib::Endpoint addr = netlib::parseHostPort(cfg_->listen);
    server_ = std::make_unique<netlib::TcpServer>(loop_, addr, 0);
    server_->setConnCb([this](const netlib::TcpConnectionPtr& c) { onConn(c); });
    server_->setMessageCb(
        [this](const netlib::TcpConnectionPtr& c, netlib::Buffer& b) { onMessage(c, b); });
    server_->start();
    listening_ = true;
    std::fprintf(stderr, "[perfacet] listen %s\n", cfg_->listen.c_str());
}

void HttpMcp::onConn(const netlib::TcpConnectionPtr& conn) {
    conn->setContext(makeSession(this, conn.get()));
}

void HttpMcp::onMessage(const netlib::TcpConnectionPtr& conn, netlib::Buffer& buf) {
    auto any = conn->getContext();
    auto sess = std::any_cast<std::shared_ptr<Session>>(any);
    if (sess->busy) return; // 本请求尚未写回，剩余字节留在缓冲
    const size_t n = buf.readableBytes();
    enum llhttp_errno err = llhttp_execute(&sess->parser, buf.peek(), n);
    buf.retrieve(n);
    if (err != HPE_OK && err != HPE_PAUSED) {
        sendHttp(conn, 400, "Bad Request",
                 ir::Json{{"jsonrpc", "2.0"},
                          {"id", nullptr},
                          {"error", ir::jsonRpcError(-32700, "parse error")}}
                     .dump());
        resetSession(*sess);
        return;
    }
    if (!sess->complete) return;
    sess->busy = true;

    auto doneWrite = [conn, sess](int status, const char* reason, const std::string& body) {
        sendHttp(conn, status, reason, body);
        resetSession(*sess);
        sess->busy = false;
    };

    auto path = sess->url;
    auto qpos = path.find('?');
    if (qpos != std::string::npos) path = path.substr(0, qpos);

    if (sess->method == "GET" && path == "/healthz") {
        doneWrite(200, "OK", "{\"ok\":true}");
        return;
    }

    const std::string origin = header(sess->headers, "origin");
    if (!originOk(*cfg_, origin)) {
        doneWrite(403, "Forbidden",
                  ir::Json{{"jsonrpc", "2.0"},
                           {"id", nullptr},
                           {"error", ir::jsonRpcError(-32000, "origin denied")}}
                      .dump());
        return;
    }

    const std::string authz = header(sess->headers, "authorization");
    std::string bearer;
    if (authz.size() >= 7 && lower(authz.substr(0, 7)) == "bearer ") {
        bearer = authz.substr(7);
    }

    if (sess->method == "GET" && path == "/upstreams") {
        auto who = bearer.empty() ? std::nullopt : identity_->authenticate(bearer);
        if (!who || !who->admin) {
            if (audit_) {
                AuditEvent e;
                e.event = "auth_fail";
                e.status = bearer.empty() ? "missing" : "unknown";
                audit_->emit(std::move(e));
            }
            doneWrite(401, "Unauthorized",
                      ir::Json{{"jsonrpc", "2.0"},
                               {"id", nullptr},
                               {"error", ir::jsonRpcError(-32000, "unauthorized")}}
                          .dump());
            return;
        }
        ir::Json ups = ir::Json::array();
        for (const auto& name : catalog_->names()) {
            auto* e = catalog_->find(name);
            ups.push_back(ir::Json{
                {"name", name},
                {"state", healthStateName(health_->state(name))},
                {"latency_ewma_ms", health_->latencyEwmaMs(name)},
                {"secret", e ? e->meta.secret : false},
            });
        }
        auto snap = counters_->snapshot();
        ir::Json body{
            {"upstreams", ups},
            {"observe",
             ir::Json{{"inflight_hit", snap.inflight_hit},
                      {"inflight_confirm", snap.inflight_confirm},
                      {"throttled", snap.throttled},
                      {"circuit_open", snap.circuit_open},
                      {"otlp_dropped", snap.otlp_dropped},
                      {"permit_held", snap.permit_held},
                      {"inflight_held", snap.inflight_held}}},
            {"governor_tools", governor_->statusJson()},
        };
        doneWrite(200, "OK", body.dump());
        return;
    }

    if (sess->method != "POST" || path != "/mcp") {
        doneWrite(404, "Not Found",
                  ir::Json{{"jsonrpc", "2.0"},
                           {"id", nullptr},
                           {"error", ir::jsonRpcError(-32601, "not found")}}
                      .dump());
        return;
    }

    if (stopping_) {
        doneWrite(503, "Service Unavailable",
                  ir::Json{{"jsonrpc", "2.0"},
                           {"id", nullptr},
                           {"error", ir::jsonRpcError(-32000, "draining")}}
                      .dump());
        return;
    }

    const std::string accept = header(sess->headers, "accept");
    if (accept.find("application/json") == std::string::npos) {
        doneWrite(406, "Not Acceptable",
                  ir::Json{{"jsonrpc", "2.0"},
                           {"id", nullptr},
                           {"error", ir::jsonRpcError(-32600, "Accept must include application/json")}}
                      .dump());
        return;
    }

    auto rpcErr = [&](int http, int code, const char* msg, ir::Json data, ir::Json id) {
        ir::Json err = ir::jsonRpcError(code, msg, std::move(data));
        doneWrite(http, http == 401 ? "Unauthorized" : "Bad Request",
                  ir::Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"error", err}}.dump());
    };

    const std::string ver = header(sess->headers, "mcp-protocol-version");
    if (ver != ir::kProtocolVersion) {
        rpcErr(400, -32022, "Unsupported protocol version",
               ir::Json{{"supported", ir::Json::array({ir::kProtocolVersion})}}, nullptr);
        return;
    }

    auto body = ir::Json::parse(sess->body, nullptr, false);
    ir::Json id = nullptr;
    if (!body.is_discarded() && body.is_object() && body.contains("id")) id = body["id"];
    if (body.is_discarded() || !body.is_object()) {
        rpcErr(400, -32700, "Parse error", {}, nullptr);
        return;
    }
    if (!body.contains("method") || body.contains("result") ||
        (!body.contains("id") && !body.contains("params"))) {
        // JSON-RPC notification：无 id
        if (!body.contains("id")) {
            rpcErr(400, -32600, "notifications are not accepted", {}, nullptr);
            return;
        }
    }

    const std::string method = body.value("method", "");
    ir::Json params = body.contains("params") ? body["params"] : ir::Json::object();
    ir::Json meta = ir::Json::object();
    if (params.is_object() && params.contains("_meta")) meta = params["_meta"];

    const std::string bodyVer = meta.is_object() ? meta.value(ir::kMetaProtocol, "") : "";
    if (bodyVer != ir::kProtocolVersion) {
        if (bodyVer.empty() || !meta.contains(ir::kMetaCaps)) {
            rpcErr(400, -32602, "missing _meta required fields", {}, id);
            return;
        }
        rpcErr(400, -32020, "HeaderMismatch", {}, id);
        return;
    }
    if (!meta.contains(ir::kMetaCaps)) {
        rpcErr(400, -32602, "missing _meta required fields", {}, id);
        return;
    }
    if (ver != bodyVer) {
        rpcErr(400, -32020, "HeaderMismatch", {}, id);
        return;
    }

    const std::string mcpMethod = header(sess->headers, "mcp-method");
    if (mcpMethod.empty() || mcpMethod != method) {
        rpcErr(400, -32020, "HeaderMismatch", {}, id);
        return;
    }

    std::string name;
    if (method == "tools/call") {
        name = params.is_object() ? params.value("name", "") : "";
    } else if (method.rfind("tasks/", 0) == 0) {
        name = params.is_object() ? params.value("taskId", "") : "";
    }
    const bool needName = method == "tools/call" || method == "tasks/get" ||
                          method == "tasks/cancel";
    if (needName) {
        const std::string hn = header(sess->headers, "mcp-name");
        if (hn.empty() || hn != name) {
            rpcErr(400, -32020, "HeaderMismatch", {}, id);
            return;
        }
    }

    static const char* kKnown[] = {"server/discover", "tools/list", "tools/call",
                                   "tasks/get", "tasks/cancel"};
    bool known = false;
    for (auto* m : kKnown) {
        if (method == m) {
            known = true;
            break;
        }
    }
    if (!known) {
        rpcErr(404, -32601, "Method not found", {}, id);
        return;
    }

    if (bearer.empty()) {
        if (audit_) {
            AuditEvent e;
            e.event = "auth_fail";
            e.status = "missing";
            audit_->emit(std::move(e));
        }
        rpcErr(401, -32000, "unauthorized", {}, id);
        return;
    }
    auto who = identity_->authenticate(bearer);
    if (!who) {
        if (audit_) {
            AuditEvent e;
            e.event = "auth_fail";
            e.status = "unknown";
            audit_->emit(std::move(e));
        }
        rpcErr(401, -32000, "unauthorized", {}, id);
        return;
    }
    who->grantBump = grants_->effectiveBump(who->agentId, nowMs());

    ir::Request req;
    req.method = method;
    req.name = name;
    req.upstreamId = id.dump();
    req.idJson = id;
    req.params = params;
    req.meta = meta;
    req.who = *who;
    req.deadlineMs = nowMs() + 60000;
    if (meta.contains(ir::kMetaCaps) && meta[ir::kMetaCaps].is_object()) {
        auto caps = meta[ir::kMetaCaps];
        if (caps.contains("extensions") && caps["extensions"].is_object() &&
            caps["extensions"].contains(ir::kTasksExt)) {
            req.caps.tasks = true;
        }
    }
    const std::string tp = header(sess->headers, "traceparent");
    if (tp.size() >= 55 && tp[2] == '-') {
        req.trace.traceId = tp.substr(3, 32);
        req.trace.spanId = tp.substr(36, 16);
    }

    pipeline_->handle(std::move(req), [doneWrite, id](ir::Response r) {
        const int st = httpStatusFor(r);
        const char* reason = st == 200   ? "OK"
                             : st == 400 ? "Bad Request"
                             : st == 401 ? "Unauthorized"
                             : st == 404 ? "Not Found"
                             : st == 406 ? "Not Acceptable"
                             : st == 503 ? "Service Unavailable"
                                         : "OK";
        doneWrite(st, reason, wrapRpc(id, r).dump());
    });
}

} // namespace perfacet
