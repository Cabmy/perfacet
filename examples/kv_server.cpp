// kv server：内存 KV
// handler Set 可选 sleep --sleep-ms 毫秒模拟业务 IO（默认 0）
// 用法：kv_server [port] [--sleep-ms N] [--registry ip:port] [--advertise ip] [--admin port]
// 配置优先级：命令行参数 > 环境变量/.env（见根目录 .env.example）> 编译默认
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "kv.pb.h"
#include "common/EnvConfig.h"
#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "server/RpcServer.h"

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    uint16_t port = static_cast<uint16_t>(twigrpc::env::getInt("TWIGRPC_PORT", 9000));
    std::string listenIp = twigrpc::env::get("TWIGRPC_LISTEN_IP", "0.0.0.0");
    int sleepMs = twigrpc::env::getInt("TWIGRPC_SLEEP_MS", 0);
    std::string registry = twigrpc::env::get("TWIGRPC_REGISTRY");
    std::string advertiseIp = twigrpc::env::get("TWIGRPC_ADVERTISE_IP");
    uint16_t adminPort = static_cast<uint16_t>(
        twigrpc::env::getInt("TWIGRPC_ADMIN_PORT", 0));
    int ioThreads = twigrpc::env::getInt("TWIGRPC_IO_THREADS", config::kServerIoThreads);
    int workers = twigrpc::env::getInt("TWIGRPC_WORKERS", config::kWorkerThreads);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sleep-ms") == 0 && i + 1 < argc) {
            sleepMs = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--registry") == 0 && i + 1 < argc) {
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

    std::unordered_map<std::string, std::string> store;
    std::mutex mtx;

    netlib::EventLoop mainLoop;
    twigrpc::RpcServer server(&mainLoop, netlib::Endpoint(listenIp, port),
                              ioThreads, workers, adminPort);

    server.bind<kv::SetRequest, kv::SetResponse>(
        "kv.Set", [&](const kv::SetRequest& req, const twigrpc::CancelToken& token) {
            // 可选 sleep 模拟业务 IO，期间协作响应取消
            // 轮询粒度 1ms：压测常用 --sleep-ms 1，5ms 粒度会把 1ms IO 放大成 5 倍耗时
            for (int slept = 0; slept < sleepMs && !token.cancelled(); slept += 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            {
                std::lock_guard<std::mutex> lk(mtx);
                store[req.key()] = req.value();
            }
            kv::SetResponse resp;
            resp.set_ok(true);
            return resp;
        });

    server.bind<kv::GetRequest, kv::GetResponse>(
        "kv.Get", [&](const kv::GetRequest& req, const twigrpc::CancelToken&) {
            kv::GetResponse resp;
            std::lock_guard<std::mutex> lk(mtx);
            auto it = store.find(req.key());
            if (it != store.end()) {
                resp.set_found(true);
                resp.set_value(it->second);
            }
            return resp;
        });

    if (!registry.empty()) {
        try {
            server.withRegistry(netlib::parseHostPort(registry), "kv", "",
                                advertiseIp);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "bad --registry: %s\n", e.what());
            return 1;
        }
    }

    server.serve();
    // 用 listenPort（port=0 时由内核分配，命令行值会误导）
    std::fprintf(stderr, "[kv_server] listening on %u sleep-ms=%d\n",
                 server.listenPort(), sleepMs);
    mainLoop.loop();
    return 0;
}
