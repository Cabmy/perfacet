// echo_client.cpp —— gRPC async C++ client
// 与 TwigRPC bench 同参数模型：channels 个通道 × inflight 个在途 × durSec 秒，
// 每通道独立 CompletionQueue + 独立线程（对齐 bench 的 conns 个 worker）。
// 用法：echo_client [ip] [port] [channels] [inflight] [durationSec]
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "echo.grpc.pb.h"
#include "benchmark/BenchUtil.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using echo::Echo;
using echo::EchoReply;
using echo::EchoRequest;

static std::atomic<bool> g_stop{false};

using twigrpc::bench::Agg;
using twigrpc::bench::Latency;
using twigrpc::bench::readRssKb;

struct Pending {
    EchoRequest req;
    EchoReply reply;
    ClientContext ctx;
    Status status;
    std::unique_ptr<ClientAsyncResponseReader<EchoReply>> rpc;
    std::chrono::steady_clock::time_point t0;
};

static void worker(Echo::Stub* stub, CompletionQueue* cq, int inflight, Agg& agg) {
    Latency local;
    long ok = 0, bad = 0;
    auto issue = [&]() {
        auto* p = new Pending;
        p->req.set_msg("hello");
        p->t0 = std::chrono::steady_clock::now();
        p->rpc = stub->PrepareAsyncEcho(&p->ctx, p->req, cq);
        p->rpc->StartCall();
        p->rpc->Finish(&p->reply, &p->status, p);
    };
    for (int i = 0; i < inflight; ++i) issue();

    while (!g_stop.load()) {
        void* tag = nullptr;
        bool okc = false;
        if (!cq->Next(&tag, &okc)) break;
        auto* p = static_cast<Pending*>(tag);
        auto t1 = std::chrono::steady_clock::now();
        if (p->status.ok()) {
            local.add(std::chrono::duration<double, std::micro>(t1 - p->t0).count());
            ++ok;
        } else {
            ++bad;
        }
        delete p;
        if (!g_stop.load()) issue(); // 补发保持 inflight
    }

    agg.total += ok;
    agg.fails += bad;
    std::lock_guard<std::mutex> lk(agg.latMtx);
    agg.lat.samples.insert(agg.lat.samples.end(), local.samples.begin(), local.samples.end());
}

// 通道上下文：stub/cq 存活期覆盖 worker 线程
struct ChannelCtx {
    std::shared_ptr<Channel> ch;
    std::unique_ptr<Echo::Stub> stub;
    std::unique_ptr<CompletionQueue> cq;
};

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? std::atoi(argv[2]) : 50051;
    int channels = (argc > 3) ? std::atoi(argv[3]) : 16;
    int inflight = (argc > 4) ? std::atoi(argv[4]) : 128;
    int durSec = (argc > 5) ? std::atoi(argv[5]) : 10;

    std::fprintf(stderr,
                 "grpc_echo_client: %s:%d channels=%d inflight=%d dur=%ds\n",
                 ip, port, channels, inflight, durSec);

    Agg agg;
    std::vector<std::unique_ptr<ChannelCtx>> ctxs;
    std::vector<std::thread> ths;
    std::string addr = std::string(ip) + ":" + std::to_string(port);
    for (int i = 0; i < channels; ++i) {
        auto c = std::make_unique<ChannelCtx>();
        c->ch = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        c->stub = std::make_unique<Echo::Stub>(c->ch);
        c->cq = std::make_unique<CompletionQueue>();
        ths.emplace_back(worker, c->stub.get(), c->cq.get(), inflight, std::ref(agg));
        ctxs.push_back(std::move(c));
    }

    auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(durSec));
    g_stop = true;
    for (auto& c : ctxs) c->cq->Shutdown(); // 让 worker 的 Next 返回 false 退出
    for (auto& t : ths) t.join();
    double sec = std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - start)
                     .count();

    double qps = agg.total.load() / sec;
    std::fprintf(stderr,
                 "RESULT qps=%.0f total=%ld fails=%ld p50=%.1fus p99=%.1fus rss=%ldkB\n",
                 qps, agg.total.load(), agg.fails.load(), agg.lat.pct(0.5),
                 agg.lat.pct(0.99), readRssKb());
    return 0;
}
