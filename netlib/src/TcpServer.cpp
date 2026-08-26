#include "netlib/TcpServer.h"
#include "netlib/Config.h"

#include <cstdio>
#include <future>
#include <string>

namespace netlib {

TcpServer::TcpServer(EventLoop* mainLoop, const Endpoint& listenAddr, int ioThreads)
    : mainLoop_(mainLoop), listenAddr_(listenAddr),
      acceptor_(mainLoop, listenAddr), ioThreads_(ioThreads < 0 ? 0 : ioThreads),
      connsByLoop_(ioThreads_ == 0 ? 1 : static_cast<size_t>(ioThreads_)) {
    acceptor_.setNewConnCb([this](std::unique_ptr<Socket> sock, const Endpoint& peer) {
        newConnection(std::move(sock), peer);
    });
}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::start() {
    bool expect = false;
    if (!started_.compare_exchange_strong(expect, true)) return;

    for (size_t i = 0; i < static_cast<size_t>(ioThreads_); ++i) {
        auto loop = std::make_unique<EventLoop>();
        EventLoop* lp = loop.get();
        subLoops_.push_back(std::move(loop));
        subThreads_.emplace_back([lp]() { lp->loop(); });
    }
    std::fprintf(stderr, "[TcpServer] listening on %s with %s\n",
                 listenAddr_.toString().c_str(),
                 ioThreads_ == 0 ? "main loop only"
                                 : (std::to_string(ioThreads_) + " io threads").c_str());
}

void TcpServer::stop() {
    if (!started_.exchange(false)) return;

    auto closeShard = [this](size_t i) {
        auto conns = std::move(connsByLoop_[i]);
        connsByLoop_[i].clear();
        for (auto& [fd, conn] : conns) {
            conn->forceClose();
        }
    };

    if (ioThreads_ == 0) {
        if (mainLoop_->inLoopThread()) {
            closeShard(0);
        } else {
            std::promise<void> done;
            auto fut = done.get_future();
            mainLoop_->runInLoop([this, closeShard, &done]() {
                closeShard(0);
                done.set_value();
            });
            fut.get();
        }
        return;
    }

    for (size_t i = 0; i < subLoops_.size(); ++i) {
        subLoops_[i]->runInLoop([this, i, closeShard]() { closeShard(i); });
        subLoops_[i]->quit();
    }
    for (auto& t : subThreads_) {
        if (t.joinable()) t.join();
    }
    subThreads_.clear();
    subLoops_.clear();
}

void TcpServer::forEachConnection(const std::function<void(const TcpConnectionPtr&)>& cb) {
    if (!started_.load()) return;

    if (ioThreads_ == 0) {
        auto run = [this, &cb]() {
            for (const auto& kv : connsByLoop_[0]) cb(kv.second);
        };
        if (mainLoop_->inLoopThread()) {
            run();
            return;
        }
        std::promise<void> done;
        auto fut = done.get_future();
        mainLoop_->runInLoop([&run, &done]() {
            run();
            done.set_value();
        });
        fut.get();
        return;
    }

    if (subLoops_.empty()) return;
    std::atomic<size_t> pending{subLoops_.size()};
    std::promise<void> done;
    auto fut = done.get_future();
    for (size_t i = 0; i < subLoops_.size(); ++i) {
        subLoops_[i]->runInLoop([this, i, &cb, &pending, &done]() {
            for (const auto& kv : connsByLoop_[i]) cb(kv.second);
            if (pending.fetch_sub(1) == 1) done.set_value();
        });
    }
    fut.get();
}

void TcpServer::newConnection(std::unique_ptr<Socket> sock, const Endpoint& peer) {
    mainLoop_->assertInLoopThread();
    if (!started_.load()) return;

    size_t idx = 0;
    EventLoop* ioLoop = mainLoop_;
    if (ioThreads_ > 0) {
        if (subLoops_.empty()) return;
        idx = nextLoop_++ % subLoops_.size();
        ioLoop = subLoops_[idx].get();
    }

    auto conn = std::make_shared<TcpConnection>(ioLoop, std::move(sock),
                                                listenAddr_, peer);
    conn->setMsgCb(msgCb_);
    conn->setConnCb(connCb_);
    conn->setCloseCb([this, idx](const TcpConnectionPtr& c) { removeConnection(idx, c); });

    int fd = conn->fd();
    ioLoop->runInLoop([this, idx, conn, fd]() {
        connsByLoop_[idx].emplace(fd, conn);
        conn->attachToLoop();
    });
}

void TcpServer::removeConnection(size_t idx, const TcpConnectionPtr& conn) {
    conn->getLoop()->queueInLoop([this, idx, conn]() {
        auto it = connsByLoop_[idx].find(conn->fd());
        if (it != connsByLoop_[idx].end() && it->second == conn) {
            connsByLoop_[idx].erase(it);
        }
    });
}

} // namespace netlib
