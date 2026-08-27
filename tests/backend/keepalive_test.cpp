#include "perfacet/backend/HttpMcpBackend.h"
#include "detail/Time.h"

#include "netlib/EventLoop.h"
#include "netlib/ThreadPool.h"

#include <httplib.h>

#include <doctest/doctest.h>

#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

using perfacet::HttpMcpBackend;
using perfacet::ir::BackendCall;
using perfacet::ir::FailureClass;

TEST_CASE("同一线程两次 POST 复用一条 TCP") {
    httplib::Server svr;
    std::mutex mu;
    std::set<std::pair<std::string, int>> peers;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lk(mu);
        peers.emplace(req.remote_addr, req.remote_port);
        res.set_content(R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})",
                        "application/json");
    });
    const int port = svr.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&]() { svr.listen_after_bind(); });
    for (int i = 0; i < 200 && !svr.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(svr.is_running());

    netlib::EventLoop loop;
    netlib::ThreadPool pool(1);
    HttpMcpBackend be("http://127.0.0.1:" + std::to_string(port) + "/mcp", &loop, &pool);
    BackendCall bc;
    bc.method = "tools/list";
    bc.params = perfacet::ir::Json::object();
    bc.deadlineMs = perfacet::nowMs() + 5000;

    auto a = be.callBlocking(bc);
    auto b = be.callBlocking(bc);
    CHECK(a.klass == FailureClass::Ok);
    CHECK(b.klass == FailureClass::Ok);
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK(peers.size() == 1);
    }

    svr.stop();
    th.join();
}
