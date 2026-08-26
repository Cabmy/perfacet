// math client：同步 + 并发 200 异步调用
// 目标地址：argv > 环境变量/.env（TWIGRPC_SERVER_IP/TWIGRPC_SERVER_PORT）> 默认
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include "math.pb.h"
#include "client/RpcClient.h"
#include "common/EnvConfig.h"

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    std::string ip = (argc > 1)
                         ? argv[1]
                         : twigrpc::env::get("TWIGRPC_SERVER_IP", "127.0.0.1");
    uint16_t port = (argc > 2)
                        ? static_cast<uint16_t>(std::atoi(argv[2]))
                        : static_cast<uint16_t>(
                              twigrpc::env::getInt("TWIGRPC_SERVER_PORT", 9000));

    twigrpc::RpcClient client(netlib::Endpoint(ip, port));
    client.waitConnected(2000);

    // 同步调用
    math::AddRequest req;
    req.set_a(1);
    req.set_b(2);
    math::AddResponse resp = client.call<math::AddResponse>("math.Add", req);
    std::printf("sync: %d\n", resp.result());
    if (resp.result() != 3) return 1;

    // 并发 200 异步调用
    std::vector<std::future<math::AddResponse>> futs;
    for (int i = 0; i < 200; ++i) {
        math::AddRequest r;
        r.set_a(i);
        r.set_b(1000);
        futs.push_back(
            client.asyncCall<math::AddResponse>("math.Add", r,
                                                twigrpc::CallOpts{1000}));
    }
    int ok = 0;
    for (size_t i = 0; i < futs.size(); ++i) {
        try {
            if (futs[i].get().result() == static_cast<int>(i) + 1000) ++ok;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "call %zu failed: %s\n", i, e.what());
        }
    }
    std::printf("async: %d/200 ok\n", ok);
    return ok == 200 ? 0 : 2;
}
