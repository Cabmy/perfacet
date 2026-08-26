#pragma once
// 上游阻塞 HTTP client 跑在 worker；完成必须 queueInLoop。不读 Principal、不重试、不熔断。
#include "perfacet/backend/Backend.h"

#include "netlib/EventLoop.h"
#include "netlib/ThreadPool.h"

#include <string>

namespace perfacet {

class HttpMcpBackend : public Backend {
public:
    HttpMcpBackend(std::string url, netlib::EventLoop* loop,
                   netlib::ThreadPool* pool);

    void call(const ir::BackendCall&,
              std::function<void(ir::Response)>) override;

    ir::Response callBlocking(const ir::BackendCall& bc);

    const std::string& url() const { return url_; }

private:
    std::string url_, host_, path_;
    int port_ = 80;
    netlib::EventLoop* loop_;
    netlib::ThreadPool* pool_;
};

} // namespace perfacet
