#pragma once
// 调用线程独占的 HTTP/1.1 keep-alive POST。不认识 MCP / Principal。
// httplib Client 非线程安全：按 thread_local × host:port 缓存，禁止跨线程共享。
#include <map>
#include <string>

namespace perfacet {

struct KeepAlivePost {
    bool transportOk = false;
    bool timeout = false;
    int status = 0;
    std::string body;
};

KeepAlivePost keepAlivePost(const std::string& host, int port, const std::string& path,
                            const std::map<std::string, std::string>& headers,
                            const std::string& body, int readTimeoutSec,
                            int writeTimeoutSec);

} // namespace perfacet
