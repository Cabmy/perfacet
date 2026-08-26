// echo_server.cpp —— gRPC async C++ server（官方 helloworld async 示例改造）
// 用法：echo_server [addr] [cqThreads]  默认 0.0.0.0:50051, 8 CQ 线程
// 语义与 TwigRPC math.Echo 对齐：纯 echo，无 sleep（纯网络基线）。
#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "echo.grpc.pb.h"

using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerCompletionQueue;
using grpc::ServerContext;
using grpc::Status;
using echo::Echo;
using echo::EchoReply;
using echo::EchoRequest;

// 每请求一个 CallData（gRPC async 官方标准模型：状态机推进 + 自续订）
class CallData {
public:
    CallData(Echo::AsyncService* service, ServerCompletionQueue* cq)
        : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE) {
        Proceed();
    }

    void Proceed() {
        if (status_ == CREATE) {
            status_ = PROCESS;
            service_->RequestEcho(&ctx_, &request_, &responder_, cq_, cq_, this);
        } else if (status_ == PROCESS) {
            new CallData(service_, cq_); // 为下一个请求预注册
            reply_.set_msg(request_.msg());
            status_ = FINISH;
            responder_.Finish(reply_, Status::OK, this);
        } else {
            delete this; // FINISH 完成回调后释放
        }
    }

private:
    Echo::AsyncService* service_;
    ServerCompletionQueue* cq_;
    ServerContext ctx_;
    EchoRequest request_;
    EchoReply reply_;
    ServerAsyncResponseWriter<EchoReply> responder_;
    enum CallStatus { CREATE, PROCESS, FINISH };
    CallStatus status_;
};

class ServerImpl {
public:
    ~ServerImpl() {
        server_->Shutdown();
        for (auto& cq : cqs_) cq->Shutdown();
        for (auto& t : threads_) t.join();
    }

    void Run(const std::string& addr, int cqThreads) {
        ServerBuilder builder;
        builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
        builder.RegisterService(&service_);
        for (int i = 0; i < cqThreads; ++i)
            cqs_.push_back(builder.AddCompletionQueue());
        server_ = builder.BuildAndStart();
        if (!server_) {
            std::fprintf(stderr, "[grpc_echo_server] failed to start on %s\n", addr.c_str());
            std::exit(1);
        }
        std::fprintf(stderr, "[grpc_echo_server] listening on %s with %d cq threads\n",
                     addr.c_str(), cqThreads);
        for (auto& cq : cqs_)
            threads_.emplace_back([this, &cq]() { HandleRpcs(cq.get()); });
    }

private:
    void HandleRpcs(ServerCompletionQueue* cq) {
        new CallData(&service_, cq);
        void* tag = nullptr;
        bool ok = false;
        while (cq->Next(&tag, &ok)) {
            static_cast<CallData*>(tag)->Proceed();
        }
    }

    Echo::AsyncService service_;
    std::unique_ptr<Server> server_;
    std::vector<std::unique_ptr<ServerCompletionQueue>> cqs_;
    std::vector<std::thread> threads_;
};

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    std::string addr = (argc > 1) ? argv[1] : "0.0.0.0:50051";
    int cqThreads = (argc > 2) ? std::atoi(argv[2]) : 8;
    ServerImpl server;
    server.Run(addr, cqThreads);
    std::this_thread::sleep_for(std::chrono::hours(24)); // 常驻，Ctrl-C 结束
    return 0;
}
