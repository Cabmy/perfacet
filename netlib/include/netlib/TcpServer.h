#pragma once
// netlib::TcpServer —— 连接集合管理 + 多 Reactor 分发。
// 主 loop 只跑 Acceptor；每条连接归属一个 sub loop（连接 map 按 reactor 分片持有，免全局锁）。
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "netlib/Acceptor.h"
#include "netlib/Endpoint.h"
#include "netlib/EventLoop.h"
#include "netlib/TcpConnection.h"

namespace netlib {

class TcpServer {
public:
    // ioThreads: sub reactor 数量（不含主 loop）。
    // 字节流服务器：监听地址可为 TCP 或 UDS Endpoint（类名保持 Tcp*，避免无意义全库改名）
    TcpServer(EventLoop* mainLoop, const Endpoint& listenAddr, int ioThreads = 2);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void setMessageCb(TcpConnection::MsgCb cb) { msgCb_ = std::move(cb); }
    void setConnCb(TcpConnection::ConnCb cb) { connCb_ = std::move(cb); }

    void start(); // 启动 sub loop 线程 + 开始 accept（必须先于 mainLoop 运行调用）
    void stop();  // 优雅停机：quit 所有 loop + join
    // 遍历所有活跃连接：cb 在各连接所属 IO 线程执行（可安全 send），
    // 全部分片遍历完成后才返回（阻塞调用线程）。只做遍历，不 close。
    void forEachConnection(const std::function<void(const TcpConnectionPtr&)>& cb);
    uint16_t listenPort() const { return acceptor_.listenPort(); }

private:
    void newConnection(std::unique_ptr<Socket> sock, const Endpoint& peer);
    void removeConnection(size_t idx, const TcpConnectionPtr& conn);

    EventLoop* mainLoop_;
    Endpoint listenAddr_;
    Acceptor acceptor_;

    std::vector<std::unique_ptr<EventLoop>> subLoops_;
    std::vector<std::thread> subThreads_;
    // 连接表按 reactor 分片：connsByLoop_[i] 的插入与删除都只在 subLoops_[i] 线程执行
    std::vector<std::map<int, TcpConnectionPtr>> connsByLoop_;
    size_t nextLoop_ = 0;

    TcpConnection::MsgCb msgCb_;
    TcpConnection::ConnCb connCb_;
    std::atomic<bool> started_{false};
};

} // namespace netlib
