// local_mux_server：本机多路复用压测载体。
// 监听 tcp://127.0.0.1:0（随机端口）或 unix:///tmp/twigrpc-mux.sock（--unix），
// bind 慢工具（sleep 协作取消）。无注册中心、无业务逻辑。
// 用法：local_mux_server [port] [--unix /path/to.sock] [--sleep-ms N]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>

#include "math.pb.h"
#include "netlib/EventLoop.h"
#include "server/RpcServer.h"

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    uint16_t port = 0; // 0 = 内核随机分配
    const char* unixPath = nullptr;
    int sleepMs = 20;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--unix") == 0 && i + 1 < argc) {
            unixPath = argv[++i];
        } else if (std::strcmp(argv[i], "--sleep-ms") == 0 && i + 1 < argc) {
            sleepMs = std::atoi(argv[++i]);
        } else {
            port = static_cast<uint16_t>(std::atoi(argv[i]));
        }
    }

    netlib::Endpoint listenAddr =
        unixPath ? netlib::Endpoint::unixPath(unixPath)
                 : netlib::Endpoint("127.0.0.1", port);

    netlib::EventLoop mainLoop;
    twigrpc::RpcServer server(&mainLoop, listenAddr, 2, 8);

    // 慢工具：睡 sleepMs，每 5ms 轮询 token 协作取消（CANCEL 帧到达即提前退出）
    std::atomic<int> cancelledCount{0};
    server.bind<math::AddRequest, math::AddResponse>(
        "mux.Sleep", [&](const math::AddRequest& req,
                         const twigrpc::CancelToken& token) {
            for (int slept = 0; slept < sleepMs && !token.cancelled(); slept += 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (token.cancelled()) cancelledCount.fetch_add(1);
            math::AddResponse resp;
            resp.set_result(req.a() + req.b());
            return resp;
        });

    server.serve();
    std::fprintf(stderr, "[local_mux_server] listening on %s sleep-ms=%d\n",
                 server.listenAddr().toString().c_str(), sleepMs);
    // 供 planner 读取：TCP 打印端口，UDS 打印路径
    if (unixPath) {
        std::printf("%s\n", unixPath);
    } else {
        std::printf("%u\n", server.listenPort());
    }
    std::fflush(stdout);
    mainLoop.loop();
    return 0;
}
