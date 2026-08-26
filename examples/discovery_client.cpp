// discovery client：经注册中心调 math 服务
// 用法：discovery_client [registryIp:port] [N]
//   registry 地址缺省取 环境变量/.env TWIGRPC_REGISTRY（见根目录 .env.example）
#include <csignal>
#include <cstdio>
#include <map>
#include <string>

#include "math.pb.h"
#include "common/EnvConfig.h"
#include "netlib/Endpoint.h"
#include "client/RpcClient.h"

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    std::string reg = (argc > 1) ? argv[1] : twigrpc::env::get("TWIGRPC_REGISTRY");
    if (reg.empty()) {
        std::fprintf(stderr,
                     "usage: %s <registryIp:port> [N] (or set TWIGRPC_REGISTRY)\n",
                     argv[0]);
        return 1;
    }
    netlib::Endpoint addr;
    try {
        addr = netlib::parseHostPort(reg);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bad registry address: %s\n", e.what());
        return 1;
    }
    int N = (argc > 2) ? std::atoi(argv[2]) : 1000;

    twigrpc::RpcClient client(addr, "math",
                            twigrpc::RegistryWatcher::BalancerType::RoundRobin);
    if (!client.waitDiscovered(3000)) {
        std::fprintf(stderr, "no instances discovered\n");
        return 1;
    }

    int ok = 0;
    for (int i = 0; i < N; ++i) {
        math::AddRequest req;
        req.set_a(i);
        req.set_b(1000);
        try {
            auto resp = client.call<math::AddResponse>(
                "math.Add", req, twigrpc::CallOpts{1000});
            if (resp.result() == i + 1000) ++ok;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "call %d failed: %s\n", i, e.what());
        }
    }
    std::printf("ok=%d/%d\n", ok, N);
    return ok == N ? 0 : 2;
}
