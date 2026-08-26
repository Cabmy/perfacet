#include "stats/AdminEndpoint.h"

#include <cstdio>
#include <string>

namespace twigrpc {
namespace stats {

using netlib::Buffer;
using netlib::TcpConnectionPtr;

AdminEndpoint::AdminEndpoint(netlib::EventLoop* mainLoop, const Collector* collector,
                             uint16_t port)
    : loop_(mainLoop), collector_(collector) {
    tcp_ = new netlib::TcpServer(mainLoop, netlib::Endpoint("0.0.0.0", port), 1);
    tcp_->setMessageCb(
        [this](const TcpConnectionPtr& c, Buffer& b) { onMessage(c, b); });
    tcp_->setConnCb([](const TcpConnectionPtr& c) {
        c->setContext(std::make_shared<Buffer>()); // HTTP 请求累积缓冲
    });
}

AdminEndpoint::~AdminEndpoint() {
    stop();
    delete tcp_;
}

void AdminEndpoint::start() {
    if (started_) return;
    started_ = true;
    tcp_->start();
    std::fprintf(stderr, "[AdminEndpoint] listening on %u\n", tcp_->listenPort());
}

void AdminEndpoint::stop() {
    if (!started_) return;
    started_ = false;
    tcp_->stop();
}

void AdminEndpoint::onMessage(const TcpConnectionPtr& conn, Buffer& buf) {
    // 极简 HTTP：读到 \r\n\r\n 即认为请求完整（GET 无 body）
    auto ctx = std::any_cast<std::shared_ptr<Buffer>>(conn->getContext());
    if (!ctx) return;
    ctx->append(buf.peek(), buf.readableBytes());
    buf.retrieveAll();

    // 上限保护：合法 Prometheus 抓取请求远小于 8KB，超过即视为恶意/畸形，断连
    if (ctx->readableBytes() > config::kAdminMaxRequestBytes) {
        ctx->retrieveAll();
        conn->forceClose();
        return;
    }

    const char* end = nullptr;
    const char* begin = ctx->peek();
    size_t len = ctx->readableBytes();
    for (size_t i = 0; i + 3 < len; ++i) {
        if (begin[i] == '\r' && begin[i + 1] == '\n' && begin[i + 2] == '\r' &&
            begin[i + 3] == '\n') {
            end = begin + i;
            break;
        }
    }
    if (!end) return; // 头未读完

    // 取请求行：GET /path HTTP/1.1
    std::string line(begin, end - begin);
    ctx->retrieveAll();

    std::string path;
    if (line.rfind("GET ", 0) == 0) {
        size_t sp = line.find(' ', 4);
        path = (sp == std::string::npos) ? line.substr(4) : line.substr(4, sp - 4);
    }

    int code = 200;
    const char* reason = "OK";
    std::string body = handleGet(path, &code, &reason);
    std::string resp = "HTTP/1.1 " + std::to_string(code) + " " + reason +
                       "\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
                       body;
    conn->send(resp);
    // Connection: close——发完即关
    conn->shutdown();
}

std::string AdminEndpoint::handleGet(const std::string& path, int* code,
                                     const char** reason) {
    if (path == "/metrics") {
        return collector_ ? collector_->renderPrometheus() : "";
    }
    if (path == "/healthz") {
        if (collector_ && collector_->ready()) return "ok\n";
        *code = 503;
        *reason = "Service Unavailable";
        return "not ready\n";
    }
    if (path == "/stats") {
        return collector_ ? collector_->renderStats() : "";
    }
    *code = 404;
    *reason = "Not Found";
    return "not found\n";
}

} // namespace stats
} // namespace twigrpc
