// registryd —— 注册中心独立进程（基于 TwigRPC 框架自举）
// 端口：argv[1] > 环境变量/.env TWIGRPC_REGISTRY_PORT（见根目录 .env.example）> 默认 8500
// （独立于业务服务的 TWIGRPC_PORT：单 .env 驱动本机集群时互不冲突）
#include <csignal>
#include <cstdio>

#include "common/EnvConfig.h"
#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "registry/RegistryDaemon.h"

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    uint16_t port = (argc > 1)
                        ? static_cast<uint16_t>(std::atoi(argv[1]))
                        : static_cast<uint16_t>(
                              twigrpc::env::getInt("TWIGRPC_REGISTRY_PORT", 8500));

    netlib::EventLoop mainLoop;
    twigrpc::RegistryDaemon registry(&mainLoop, netlib::Endpoint("0.0.0.0", port));
    registry.serve();

    // 过期扫描：mainLoop 定时驱动
    mainLoop.runEvery(config::kRegistrySweepSec, [&registry]() { registry.sweep(); });

    std::fprintf(stderr, "[registryd] listening on %u\n", registry.listenPort());
    mainLoop.loop();
    return 0;
}
