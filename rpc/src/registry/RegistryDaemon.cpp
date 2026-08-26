#include "registry/RegistryDaemon.h"

#include <cstdio>

#include "registry.pb.h"

namespace twigrpc {

using steady = std::chrono::steady_clock;

RegistryDaemon::RegistryDaemon(netlib::EventLoop* mainLoop, netlib::Endpoint listenAddr)
    : server_(mainLoop, std::move(listenAddr)) {
    bindMethods();
}

void RegistryDaemon::serve() {
    // 扫描由外部（registryd main）用 runEvery 驱动：需要 mainLoop，server 内部线程不适用
    server_.serve();
}

void RegistryDaemon::stop() {
    server_.stop();
}

void RegistryDaemon::bindMethods() {
    server_.bind<twigrpc::RegisterRequest, twigrpc::RegisterResponse>(
        "twigrpc.Registry.Register", [this](const twigrpc::RegisterRequest& req,
                                           const CancelToken&) {
            std::lock_guard<std::mutex> lk(mtx_);
            const auto& inst = req.instance();
            Entry e;
            e.instance = inst;
            e.lastBeat = steady::now();
            auto& svc = table_[inst.service()];
            bool existed = svc.count(inst.instance_id()) > 0;
            svc[inst.instance_id()] = e;
            if (!existed) {
                ++version_;
                std::fprintf(stderr, "[Registry] REGISTER %s/%s -> %s:%u\n",
                             inst.service().c_str(), inst.instance_id().c_str(),
                             inst.ip().c_str(), inst.port());
            }
            twigrpc::RegisterResponse resp;
            resp.set_ok(true);
            resp.set_version(version_);
            return resp;
        });

    server_.bind<twigrpc::DiscoverRequest, twigrpc::DiscoverResponse>(
        "twigrpc.Registry.Discover", [this](const twigrpc::DiscoverRequest& req,
                                           const CancelToken&) {
            std::lock_guard<std::mutex> lk(mtx_);
            twigrpc::DiscoverResponse resp;
            auto it = table_.find(req.service());
            if (it != table_.end()) {
                for (auto& [id, entry] : it->second) {
                    // DOWN 实例保留在结果中并带 healthy=false，由客户端摘除
                    *resp.add_instances() = entry.instance;
                }
            }
            resp.set_version(version_);
            return resp;
        });

    server_.bind<twigrpc::HeartbeatRequest, twigrpc::HeartbeatResponse>(
        "twigrpc.Registry.Heartbeat", [this](const twigrpc::HeartbeatRequest& req,
                                            const CancelToken&) {
            std::lock_guard<std::mutex> lk(mtx_);
            bool ok = false;
            for (auto& [svc, insts] : table_) {
                auto it = insts.find(req.instance_id());
                if (it != insts.end()) {
                    it->second.lastBeat = steady::now();
                    if (!it->second.instance.healthy()) {
                        it->second.instance.set_healthy(true); // 恢复
                        ++version_;
                    }
                    ok = true;
                    break;
                }
            }
            twigrpc::HeartbeatResponse resp;
            resp.set_ok(ok);
            return resp;
        });
}

void RegistryDaemon::sweep() {
    // 由外部定时驱动（registryd main 的 EventLoop::runEvery）
    std::lock_guard<std::mutex> lk(mtx_);
    auto now = steady::now();
    for (auto& [svc, insts] : table_) {
        for (auto it = insts.begin(); it != insts.end();) {
            double idle = std::chrono::duration<double>(now - it->second.lastBeat).count();
            if (idle > config::kRegistryEvictAfterSec) {
                std::fprintf(stderr, "[Registry] EVICT %s/%s (idle %.0fs)\n",
                             svc.c_str(), it->first.c_str(), idle);
                it = insts.erase(it);
                ++version_;
            } else if (idle > config::kRegistryDownAfterSec &&
                       it->second.instance.healthy()) {
                std::fprintf(stderr, "[Registry] DOWN %s/%s (idle %.0fs)\n",
                             svc.c_str(), it->first.c_str(), idle);
                it->second.instance.set_healthy(false);
                ++version_;
                ++it;
            } else {
                ++it;
            }
        }
    }
}

} // namespace twigrpc
