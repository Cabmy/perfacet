#pragma once
// twigrpc::stats::AdminEndpoint —— 管理端口：极简 HTTP GET 路由。
// 只读 Collector：/metrics 文本、/healthz 跟 ready()（未就绪 503）、/stats 摘要。
// 独立 TcpServer（单 reactor），监听 config::kAdminPort。
#include <memory>
#include <string>

#include "netlib/EventLoop.h"
#include "netlib/TcpServer.h"
#include "stats/Collector.h"

namespace config {
inline constexpr uint16_t kAdminPort = 9100; // 默认管理端口
// HTTP 请求头上限：超过即断连。防畸形/慢攻击请求在管理端口撑爆内存
// （抓取方 Prometheus 只发短 GET，8KB 足够）。
inline constexpr size_t kAdminMaxRequestBytes = 8 * 1024;
} // namespace config

namespace twigrpc {
namespace stats {

class AdminEndpoint {
public:
    // mainLoop 须为长驻循环；port=0 时内核分配
    AdminEndpoint(netlib::EventLoop* mainLoop, const Collector* collector,
                  uint16_t port = config::kAdminPort);
    ~AdminEndpoint();

    void start();
    void stop();
    uint16_t listenPort() const { return tcp_->listenPort(); }

private:
    void onMessage(const netlib::TcpConnectionPtr& conn, netlib::Buffer& buf);
    std::string handleGet(const std::string& path, int* code, const char** reason);

    netlib::TcpServer* tcp_ = nullptr;
    netlib::EventLoop* loop_;
    const Collector* collector_;
    bool started_ = false;
};

} // namespace stats
} // namespace twigrpc
