#include "netlib/TcpServer.h"
#include "netlib/Config.h"

#include <cstdio>
#include <future>

namespace netlib {

TcpServer::TcpServer(EventLoop* mainLoop, const Endpoint& listenAddr, int ioThreads)
    : mainLoop_(mainLoop), listenAddr_(listenAddr),
      acceptor_(mainLoop, listenAddr), connsByLoop_(static_cast<size_t>(ioThreads)) {
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

    for (size_t i = 0; i < connsByLoop_.size(); ++i) {
        auto loop = std::make_unique<EventLoop>();
        EventLoop* lp = loop.get();
        subLoops_.push_back(std::move(loop));
        subThreads_.emplace_back([lp]() { lp->loop(); });
    }
    std::fprintf(stderr, "[TcpServer] listening on %s with %zu io threads\n",
                 listenAddr_.toString().c_str(), connsByLoop_.size());
}

void TcpServer::stop() {
    if (!started_.exchange(false)) return;
    // 逐 sub loop 摘除连接并 quit。
    // 注意：forceClose 会在本线程同步触发 removeConnection（erase map），
    // 因此先把连接表 move 出来再遍历，避免迭代器失效。
    for (size_t i = 0; i < subLoops_.size(); ++i) {
        subLoops_[i]->runInLoop([this, i]() {
            auto conns = std::move(connsByLoop_[i]);
            connsByLoop_[i].clear();
            for (auto& [fd, conn] : conns) {
                conn->forceClose();
            }
        });
        subLoops_[i]->quit();
    }
    for (auto& t : subThreads_) {
        if (t.joinable()) t.join();
    }
    subThreads_.clear();
    subLoops_.clear();
}

void TcpServer::forEachConnection(const std::function<void(const TcpConnectionPtr&)>& cb) {
    // 逐分片投递到各自 IO 线程遍历（连接表只在所属线程读写），
    // 计数归零即全部分片完成，唤醒调用方。
    if (subLoops_.empty()) return; // 未 start：无连接可遍历
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
    // start() 前未建 sub loop：直接丢弃（否则下面取模除零）
    if (subLoops_.empty()) return;
    size_t idx = nextLoop_++ % subLoops_.size();
    EventLoop* ioLoop = subLoops_[idx].get();

    auto conn = std::make_shared<TcpConnection>(ioLoop, std::move(sock),
                                                listenAddr_, peer);
    conn->setMsgCb(msgCb_);
    conn->setConnCb(connCb_);
    // closeCb 由 TcpServer 注入：在所属 sub loop 里摘除连接（shared_ptr 释放）
    conn->setCloseCb([this, idx](const TcpConnectionPtr& c) { removeConnection(idx, c); });

    int fd = conn->fd();
    // 连接表插入与删除同侧：都投递到分片线程执行（免锁分片的前提）
    ioLoop->runInLoop([this, idx, conn, fd]() {
        connsByLoop_[idx].emplace(fd, conn);
        conn->attachToLoop();
    });
}

void TcpServer::removeConnection(size_t idx, const TcpConnectionPtr& conn) {
    // 必须投递（queueInLoop）而非内联执行：连接的析构延迟到本圈事件分发结束之后，
    // 避免同批 active 列表中后续 Channel 访问已析构对象（use-after-free）
    conn->getLoop()->queueInLoop([this, idx, conn]() {
        auto it = connsByLoop_[idx].find(conn->fd());
        if (it != connsByLoop_[idx].end() && it->second == conn) {
            connsByLoop_[idx].erase(it);
        }
    });
}

} // namespace netlib
