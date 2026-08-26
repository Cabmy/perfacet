// agent_demo：Agent 画像注入 + Planner 取消演示。
// 画像 = 不同 Admission/Retry 组合，证明扩展点而非内置功能：
//   research: Admission(32) + Retry(3)    code: Admission(4) + NoRetry
//   llm:      Admission(2)  + NoRetry     summary: Admission(16) + NoRetry
// Planner：deadline=5s 根任务，两跳 fan-out（Research 4 → Search 2×）。
// 默认中途 cancel(root)：断言所有叶子 CANCELLED、取消后零重试（--no-cancel 跑全程）。
// 用法：agent_demo [--profile research|code|llm|summary] [--no-cancel]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agent/Runtime.h"
#include "agent/policies/AdmissionPolicy.h"
#include "agent/policies/NoRetry.h"
#include "agent/policies/RetryPolicy.h"
#include "math.pb.h"
#include "netlib/EventLoop.h"
#include "client/RpcClient.h"
#include "server/RpcServer.h"

using agent::AdmissionPolicy;
using agent::IRetry;
using agent::Runtime;
using agent::TaskId;
using twigrpc::RpcException;
using twigrpc::statusName;

namespace {

// RetryPolicy 包装：记录取消发生后是否还出现重试（Planner 断言用）
struct RetryTrace : IRetry {
    explicit RetryTrace(int maxAttempts) : inner_(maxAttempts) {}
    bool shouldRetry(const agent::TaskContext& ctx, twigrpc::Status st,
                     int attempt) override {
        if (cancelled_.load()) ++afterCancel_;
        return inner_.shouldRetry(ctx, st, attempt);
    }
    std::atomic<bool> cancelled_{false};
    std::atomic<int> afterCancel_{0};
    agent::RetryPolicy inner_;
};

struct Profile {
    const char* name;
    int maxConcurrent;
    int retryAttempts; // 0 = NoRetry
};

const Profile kProfiles[] = {
    {"research", 32, 3}, {"code", 4, 0}, {"llm", 2, 0}, {"summary", 16, 0},
};

// 慢工具服务：睡 250ms，每 5ms 轮询 token（协作取消），统计收到的请求数
struct DemoServer {
    netlib::EventLoop loop;
    std::thread th;
    std::unique_ptr<twigrpc::RpcServer> server;
    std::atomic<int> received{0};

    DemoServer() {
        server = std::make_unique<twigrpc::RpcServer>(&loop,
                                                      netlib::Endpoint("127.0.0.1", 0));
        auto handler = [this](const math::AddRequest& req,
                              const twigrpc::CancelToken& token) {
            received.fetch_add(1);
            for (int s = 0; s < 250 && !token.cancelled(); s += 5) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            math::AddResponse resp;
            resp.set_result(req.a() + req.b());
            return resp;
        };
        server->bind<math::AddRequest, math::AddResponse>("demo.Research", handler);
        server->bind<math::AddRequest, math::AddResponse>("demo.Search", handler);
        th = std::thread([this]() { loop.loop(); });
        server->serve();
    }
    ~DemoServer() {
        loop.quit();
        if (th.joinable()) th.join();
    }
    uint16_t port() const { return server->listenPort(); }
};

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    const Profile* prof = &kProfiles[0];
    bool doCancel = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            const char* name = argv[++i];
            for (const auto& p : kProfiles) {
                if (std::strcmp(p.name, name) == 0) prof = &p;
            }
        } else if (std::strcmp(argv[i], "--no-cancel") == 0) {
            doCancel = false;
        }
    }

    DemoServer srv;
    twigrpc::RpcClient client(netlib::Endpoint("127.0.0.1", srv.port()));
    client.waitConnected();

    auto admission = std::make_unique<AdmissionPolicy>(prof->maxConcurrent);
    AdmissionPolicy* adm = admission.get();
    std::unique_ptr<IRetry> retry =
        prof->retryAttempts > 0
            ? std::unique_ptr<IRetry>(new RetryTrace(prof->retryAttempts))
            : std::unique_ptr<IRetry>(new agent::NoRetry());
    RetryTrace* trace =
        prof->retryAttempts > 0 ? static_cast<RetryTrace*>(retry.get()) : nullptr;

    Runtime rt(client, std::move(retry));
    rt.policies().add(std::move(admission));

    std::fprintf(stderr, "[agent_demo] profile=%s admission(max=%d) retry(%s)\n",
                 prof->name, prof->maxConcurrent,
                 prof->retryAttempts > 0 ? "3 attempts" : "none");

    // Planner：deadline=5s 根任务，两跳 fan-out（Research 4 → Search 2×）
    TaskId root = rt.spawnRoot(agent::nowMs() + 5000);
    auto leafOutcome = [&](const char* method) -> std::string {
        math::AddRequest req;
        req.set_a(1);
        req.set_b(2);
        try {
            (void)rt.call<math::AddResponse>(root, method, req, 3000);
            return "OK";
        } catch (const RpcException& e) {
            return statusName(e.status());
        }
    };

    uint64_t t0 = agent::nowMs();
    std::vector<std::future<std::vector<std::string>>> futs;
    for (int r = 0; r < 4; ++r) {
        futs.push_back(std::async(std::launch::async, [&]() {
            std::vector<std::string> outcomes;
            outcomes.push_back(leafOutcome("demo.Research"));
            // 第一跳已取消则整棵树已死，不再发第二跳
            if (!rt.tree().cancelled(root)) {
                for (int s = 0; s < 2; ++s) {
                    outcomes.push_back(leafOutcome("demo.Search"));
                }
            }
            return outcomes;
        }));
    }

    if (doCancel) {
        // 等 min(4, max) 个请求在途再取消（Admission 限流的画像只有 max 个能到服务端）
        int inFlightTarget = std::min(4, prof->maxConcurrent);
        for (int i = 0; i < 400 && srv.received.load() < inFlightTarget; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (trace) trace->cancelled_.store(true);
        rt.cancel(root);
        std::fprintf(stderr, "[agent_demo] cancel(root) issued at +%llums\n",
                     (unsigned long long)(agent::nowMs() - t0));
    }

    std::vector<std::string> outcomes;
    for (auto& f : futs) {
        for (auto& o : f.get()) outcomes.push_back(o);
    }
    // 叶子已在各自 callFrame 出口结束；根任务由创建方（本 demo）结束
    (void)rt.complete(root);
    uint64_t elapsed = agent::nowMs() - t0;

    int ok = 0, cancelled = 0, rejected = 0, other = 0;
    for (const auto& o : outcomes) {
        if (o == "OK") ++ok;
        else if (o == "CANCELLED") ++cancelled;
        else if (o == "REJECTED") ++rejected; // llm/code 画像下 Admission 正常工作
        else ++other;
    }
    std::fprintf(stderr,
                 "[agent_demo] leaves=%zu ok=%d cancelled=%d rejected=%d other=%d "
                 "serverReceived=%d admissionInFlight=%d elapsed=%llums\n",
                 outcomes.size(), ok, cancelled, rejected, other, srv.received.load(),
                 adm->inFlight(), (unsigned long long)elapsed);

    // 槽全部归还、无意外状态、树清空（生命周期闭环）
    bool pass = adm->inFlight() == 0 && other == 0 && rt.tree().size() == 0;
    if (doCancel) {
        // 验收：所有叶子 CANCELLED（REJECTED 是准入生效，也合法）；取消后零重试
        pass = pass && ok == 0 && (!trace || trace->afterCancel_.load() == 0);
    } else {
        pass = pass && cancelled == 0;
    }
    std::fprintf(stderr, "[agent_demo] %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
