// local_mux_planner：单连接并行 N 路 callAsync。
// 一轮规划 = 同一 RpcConn 上 N 个在途请求（requestId 多路复用），
// 打印完成数 / P50 / P99。对照组：stdio 式串行（一问一答）同样的 N 路调用，
// 总耗时 ≈ N × sleep —— 多路复用收益一目了然，无需引入任何新协议。
// 用法：local_mux_planner <port> [--unix /path/to.sock] [N]
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>

#include "math.pb.h"
#include "client/RpcConn.h"

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <port|--unix path> [N]\n", argv[0]);
        return 1;
    }
    netlib::Endpoint addr("127.0.0.1", 9000);
    if (std::strcmp(argv[1], "--unix") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s --unix <path> [N]\n", argv[0]);
            return 1;
        }
        addr = netlib::Endpoint::unixPath(argv[2]);
    } else {
        addr = netlib::Endpoint("127.0.0.1",
                                static_cast<uint16_t>(std::atoi(argv[1])));
    }
    int n = (argc > 2 && std::strcmp(argv[1], "--unix") != 0)
                ? std::atoi(argv[2])
                : ((argc > 3) ? std::atoi(argv[3]) : 32);

    twigrpc::RpcConn conn(addr);
    if (!conn.waitConnected(2000)) {
        std::fprintf(stderr, "connect failed: %s\n", addr.toString().c_str());
        return 1;
    }

    std::vector<std::future<twigrpc::Frame>> futs;
    std::vector<std::chrono::steady_clock::time_point> t0s;
    auto batchStart = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        math::AddRequest req;
        req.set_a(i);
        req.set_b(1000);
        t0s.push_back(std::chrono::steady_clock::now());
        futs.push_back(conn.callAsync("mux.Sleep", req.SerializeAsString(),
                                      twigrpc::CallOpts{2000}));
    }

    int ok = 0;
    std::vector<double> latMs;
    for (int i = 0; i < n; ++i) {
        try {
            twigrpc::Frame f = futs[i].get();
            if (f.status != twigrpc::Status::OK) continue;
            math::AddResponse resp;
            if (!resp.ParseFromString(f.body)) continue;
            if (resp.result() != i + 1000) continue;
            double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0s[i])
                            .count();
            latMs.push_back(ms);
            ++ok;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "call %d failed: %s\n", i, e.what());
        }
    }
    double batchMs = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - batchStart)
                         .count();

    if (latMs.empty()) {
        std::fprintf(stderr, "no successful call\n");
        return 2;
    }
    std::sort(latMs.begin(), latMs.end());
    double p50 = latMs[latMs.size() / 2];
    double p99 = latMs[static_cast<size_t>(0.99 * (latMs.size() - 1))];
    std::printf("mux: %d/%d ok batch=%.1fms p50=%.1fms p99=%.1fms (%s)\n",
                ok, n, batchMs, p50, p99, addr.toString().c_str());
    return ok == n ? 0 : 3;
}
