// math server：bind Add/Echo 后启动
// 可选：--registry ip:port 接入注册中心（自注册 + 心跳；host 支持 DNS 主机名）
// 配置优先级：命令行参数 > 环境变量/.env（见根目录 .env.example）> 编译默认
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "math.pb.h"
#include "common/EnvConfig.h"
#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "server/RpcServer.h"

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    uint16_t port = static_cast<uint16_t>(twigrpc::env::getInt("TWIGRPC_PORT", 9000));
    std::string listenIp = twigrpc::env::get("TWIGRPC_LISTEN_IP", "0.0.0.0");
    std::string registry = twigrpc::env::get("TWIGRPC_REGISTRY"); // ip:port，空 = 不接入
    std::string advertiseIp = twigrpc::env::get("TWIGRPC_ADVERTISE_IP"); // 空 = 自动推导
    uint16_t adminPort = static_cast<uint16_t>(
        twigrpc::env::getInt("TWIGRPC_ADMIN_PORT", 0)); // 管理端口，0 = 不启
    int ioThreads = twigrpc::env::getInt("TWIGRPC_IO_THREADS", config::kServerIoThreads);
    int workers = twigrpc::env::getInt("TWIGRPC_WORKERS", config::kWorkerThreads);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--registry") == 0 && i + 1 < argc) {
            registry = argv[++i];
        } else if (std::strcmp(argv[i], "--advertise") == 0 && i + 1 < argc) {
            advertiseIp = argv[++i];
        } else if (std::strcmp(argv[i], "--admin") == 0 && i + 1 < argc) {
            adminPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--io-threads") == 0 && i + 1 < argc) {
            ioThreads = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = std::atoi(argv[++i]);
        } else {
            port = static_cast<uint16_t>(std::atoi(argv[i]));
        }
    }

    netlib::EventLoop mainLoop;
    twigrpc::RpcServer server(&mainLoop, netlib::Endpoint(listenIp, port),
                              ioThreads, workers, adminPort);

    server.bind<math::AddRequest, math::AddResponse>(
        "math.Add", [](const math::AddRequest& req, const twigrpc::CancelToken&) {
            math::AddResponse resp;
            resp.set_result(req.a() + req.b());
            return resp;
        });

    server.bind<math::EchoRequest, math::EchoResponse>(
        "math.Echo", [](const math::EchoRequest& req, const twigrpc::CancelToken&) {
            math::EchoResponse resp;
            std::string out;
            for (int i = 0; i < std::max(1, req.repeat()); ++i) out += req.msg();
            resp.set_msg(out);
            return resp;
        });

    // 慢调用演示（超时/慢日志/压测 sleep 场景）：睡 100ms
    // handler 协作取消：睡前轮询 token，被取消则提前退出
    server.bind<math::AddRequest, math::AddResponse>(
        "math.Slow", [](const math::AddRequest& req, const twigrpc::CancelToken& token) {
            for (int slept = 0; slept < 100 && !token.cancelled(); slept += 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            math::AddResponse resp;
            resp.set_result(req.a() + req.b());
            return resp;
        });

    if (!registry.empty()) {
        try {
            server.withRegistry(netlib::parseHostPort(registry), "math", "",
                                advertiseIp);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "bad --registry: %s\n", e.what());
            return 1;
        }
    }

    server.serve();
    std::fprintf(stderr, "[math_server] listening on %u\n", server.listenPort());
    mainLoop.loop();
    return 0;
}
