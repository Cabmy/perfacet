// netlib 独立性证明：仅用 netlib（无 rpc 层）实现 echo 服务器。
#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "netlib/TcpServer.h"

#include <csignal>
#include <cstdio>

int main(int argc, char* argv[]) {
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 8888;
    std::signal(SIGPIPE, SIG_IGN);

    netlib::EventLoop mainLoop;
    netlib::Endpoint addr("0.0.0.0", port);
    netlib::TcpServer server(&mainLoop, addr, 2);

    server.setMessageCb([](const netlib::TcpConnectionPtr& conn, netlib::Buffer& buf) {
        conn->send(std::string_view(buf.peek(), buf.readableBytes()));
        buf.retrieveAll();
    });

    server.start();
    mainLoop.loop();
    return 0;
}
