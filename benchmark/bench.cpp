// benchmark/bench.cpp —— 基于 RpcConn 多路复用的自研压测工具
// 直接走 ConnPool×RpcConn 层（真实多路复用：K 连接 × N 在途由 IO 线程承载，
// 无每请求线程开销）；RpcClient::asyncCall 是线程池式门面，不用于热路径压测。
// 用法：bench [ip] [port] [conns] [inflight] [durationSec] [method] [thinkUs] [serial] [payloadBytes]
//   method:   math.Add / math.Echo / kv.Set（自动选择对应 stub 请求）
//   thinkUs:  每请求完成后的思考时间（微秒，串行实验用），默认 0
//   serial:   非 0 时串行模式：每次仅 1 个在途，等完成再发下一个（多路复用对照）
//   payload:  math.Echo 载荷字节数（大报文场景），响应同大小回显，输出双向 MB/s
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "kv.pb.h"
#include "math.pb.h"
#include "client/ConnPool.h"
#include "codec/Protocol.h"
#include "common/EnvConfig.h"
#include "BenchUtil.h"

static std::atomic<bool> g_stop{false};

// 按方法模板化压测：serial=true 时每 worker 单在途（同步语义），否则批量 inflight 在途
// 每 worker 本地累积 latency，结束后合并（避免热路径全局锁）
using twigrpc::bench::Agg;
using twigrpc::bench::Latency;
using twigrpc::bench::readRssKb;

template <typename Req, typename Resp>
static void pump(twigrpc::ConnPool& pool, const std::string& method, int inflight,
                 const std::function<Req(int)>& makeReq, Agg& agg, bool serial, long thinkUs) {
    twigrpc::CallOpts copts{1000}; // timeout 1s
    Latency local;
    long ok = 0, bad = 0;
    int seq = 0;
    if (serial) {
        while (!g_stop.load()) {
            auto t0 = std::chrono::steady_clock::now();
            try {
                auto fut = pool.pick()->callAsync(method, makeReq(seq++).SerializeAsString(), copts);
                twigrpc::Frame f = fut.get();
                auto t1 = std::chrono::steady_clock::now();
                if (f.status == twigrpc::Status::OK) {
                    local.add(std::chrono::duration<double, std::micro>(t1 - t0).count());
                    ++ok;
                } else {
                    ++bad;
                }
            } catch (...) {
                ++bad;
            }
            if (thinkUs > 0) std::this_thread::sleep_for(std::chrono::microseconds(thinkUs));
        }
    } else {
        // 批量模式：t0 必须在发送时取（若等 get 时才取，同批早完成的请求延迟被
        // 系统性低估，p50/p99 口径与串行模式/gRPC 对照不一致）
        struct Inflight {
            std::chrono::steady_clock::time_point t0;
            std::future<twigrpc::Frame> fut;
        };
        std::vector<Inflight> batch;
        while (!g_stop.load()) {
            batch.clear();
            batch.reserve(inflight);
            for (int i = 0; i < inflight; ++i) {
                batch.push_back({std::chrono::steady_clock::now(),
                                 pool.pick()->callAsync(
                                     method, makeReq(seq++).SerializeAsString(), copts)});
            }
            for (auto& [t0, f] : batch) {
                try {
                    twigrpc::Frame fr = f.get();
                    auto t1 = std::chrono::steady_clock::now();
                    if (fr.status == twigrpc::Status::OK) {
                        local.add(std::chrono::duration<double, std::micro>(t1 - t0).count());
                        ++ok;
                    } else {
                        ++bad;
                    }
                } catch (...) {
                    ++bad;
                }
                if (thinkUs > 0) std::this_thread::sleep_for(std::chrono::microseconds(thinkUs));
            }
        }
    }
    agg.total += ok;
    agg.fails += bad;
    std::lock_guard<std::mutex> lk(agg.latMtx);
    agg.lat.samples.insert(agg.lat.samples.end(), local.samples.begin(), local.samples.end());
}

static math::AddRequest makeAdd(int i) {
    math::AddRequest req;
    req.set_a(1);
    req.set_b(2);
    (void)i;
    return req;
}

static kv::SetRequest makeSet(int i) {
    kv::SetRequest req;
    req.set_key("key-" + std::to_string(i & 0xFFFF));
    req.set_value("v");
    return req;
}

static std::string g_echoPayload; // 大报文场景：构造一次，每请求拷贝

static math::EchoRequest makeEcho(int i) {
    math::EchoRequest req;
    req.set_msg(g_echoPayload);
    req.set_repeat(1);
    (void)i;
    return req;
}

// 多指标延迟统计：一次排序后取分位数（pct() 每次调用都会重排，这里只排一次）
struct LatStats {
    double p50, p90, p99, p999, avg, min, max;
};
static LatStats summarize(std::vector<double> samples) {
    LatStats s{0, 0, 0, 0, 0, 0, 0};
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    auto at = [&](double p) {
        return samples[static_cast<size_t>(p * (samples.size() - 1))];
    };
    double sum = 0;
    for (double v : samples) sum += v;
    s = {at(0.50), at(0.90), at(0.99), at(0.999), sum / samples.size(),
         samples.front(), samples.back()};
    return s;
}

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    // 目标地址缺省取 环境变量/.env（TWIGRPC_SERVER_IP/TWIGRPC_SERVER_PORT）
    std::string ip = (argc > 1) ? argv[1]
                                : twigrpc::env::get("TWIGRPC_SERVER_IP", "127.0.0.1");
    uint16_t port = (argc > 2)
                        ? static_cast<uint16_t>(std::atoi(argv[2]))
                        : static_cast<uint16_t>(
                              twigrpc::env::getInt("TWIGRPC_SERVER_PORT", 9000));
    int conns = (argc > 3) ? std::atoi(argv[3]) : 16;
    int inflight = (argc > 4) ? std::atoi(argv[4]) : 128;
    int durSec = (argc > 5) ? std::atoi(argv[5]) : 10;
    std::string method = (argc > 6) ? argv[6] : "math.Add";
    long thinkUs = (argc > 7) ? std::atol(argv[7]) : 0;
    bool serial = (argc > 8) && std::atoi(argv[8]) != 0;
    int payloadBytes = (argc > 9) ? std::atoi(argv[9]) : 0;
    if (payloadBytes > 0) g_echoPayload.assign(payloadBytes, 'x');

    std::fprintf(stderr,
                 "bench: %s:%u conns=%d inflight=%d dur=%ds method=%s thinkUs=%ld serial=%d payload=%dB\n",
                 ip.c_str(), port, conns, inflight, durSec, method.c_str(), thinkUs, serial, payloadBytes);

    twigrpc::ConnPool pool(netlib::Endpoint(ip, port), conns);
    pool.waitConnected(3000);

    Agg agg;
    auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> ths;
    for (int i = 0; i < conns; ++i) {
        if (method.rfind("kv.", 0) == 0)
            ths.emplace_back(pump<kv::SetRequest, kv::SetResponse>, std::ref(pool),
                             method, inflight, makeSet, std::ref(agg), serial, thinkUs);
        else if (method == "math.Echo")
            ths.emplace_back(pump<math::EchoRequest, math::EchoResponse>, std::ref(pool),
                             method, inflight, makeEcho, std::ref(agg), serial, thinkUs);
        else
            ths.emplace_back(pump<math::AddRequest, math::AddResponse>, std::ref(pool),
                             method, inflight, makeAdd, std::ref(agg), serial, thinkUs);
    }

    std::this_thread::sleep_for(std::chrono::seconds(durSec));
    g_stop = true;
    for (auto& t : ths) t.join();
    double sec = std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - start)
                     .count();

    double qps = agg.total.load() / sec;
    LatStats st = summarize(std::move(agg.lat.samples));
    double mbps = (method == "math.Echo" && payloadBytes > 0)
                      ? agg.total.load() * 2.0 * payloadBytes / sec / 1e6 // 请求+响应双向
                      : 0.0;
    std::fprintf(stderr,
                 "RESULT qps=%.0f total=%ld fails=%ld "
                 "p50=%.1fus p90=%.1fus p99=%.1fus p99.9=%.1fus avg=%.1fus "
                 "min=%.1fus max=%.1fus mbps=%.1f rss=%ldkB\n",
                 qps, agg.total.load(), agg.fails.load(), st.p50, st.p90, st.p99,
                 st.p999, st.avg, st.min, st.max, mbps, readRssKb());
    return 0;
}
