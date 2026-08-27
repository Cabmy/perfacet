#include "backend/KeepAliveClient.h"

#include <httplib.h>

#include <memory>
#include <unordered_map>
#include <utility>

namespace perfacet {

namespace {

std::string slotKey(const std::string& host, int port) {
    return host + '\n' + std::to_string(port);
}

thread_local std::unordered_map<std::string, std::unique_ptr<httplib::Client>> g_clients;

httplib::Client& acquire(const std::string& host, int port) {
    auto& slot = g_clients[slotKey(host, port)];
    if (!slot) {
        slot = std::make_unique<httplib::Client>(host, port);
        slot->set_keep_alive(true);
        slot->set_tcp_nodelay(true);
        slot->set_connection_timeout(2, 0);
    }
    return *slot;
}

void drop(const std::string& host, int port) { g_clients.erase(slotKey(host, port)); }

} // namespace

KeepAlivePost keepAlivePost(const std::string& host, int port, const std::string& path,
                            const std::map<std::string, std::string>& headers,
                            const std::string& body, int readTimeoutSec,
                            int writeTimeoutSec) {
    auto& cli = acquire(host, port);
    cli.set_read_timeout(readTimeoutSec, 0);
    cli.set_write_timeout(writeTimeoutSec, 0);

    httplib::Headers hdr;
    for (const auto& kv : headers) hdr.emplace(kv.first, kv.second);

    auto res = cli.Post(path, hdr, body, "application/json");
    if (!res) {
        const auto err = res.error();
        drop(host, port);
        KeepAlivePost out;
        out.timeout = err == httplib::Error::Read || err == httplib::Error::Write ||
                      err == httplib::Error::ConnectionTimeout;
        return out;
    }
    KeepAlivePost out;
    out.transportOk = true;
    out.status = res->status;
    out.body = std::move(res->body);
    return out;
}

} // namespace perfacet
