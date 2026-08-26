#pragma once
// netlib::Epoll —— epoll 实例封装，只做事件收集，不含业务逻辑。
// 线程安全契约：epoll 资源只允许所属 IO 线程操作，其他线程一律 EventLoop::runInLoop 投递。
#include <sys/epoll.h>
#include <vector>

namespace netlib {

class Channel;

class Epoll {
public:
    Epoll();
    ~Epoll();

    Epoll(const Epoll&) = delete;
    Epoll& operator=(const Epoll&) = delete;
    Epoll(Epoll&&) = delete;
    Epoll& operator=(Epoll&&) = delete;

    void updateChannel(Channel* ch);
    void removeChannel(Channel* ch);

    // 阻塞/超时等待，把活跃 Channel 的 revents 设置好后返回
    std::vector<Channel*> poll(int timeoutMs);

private:
    int epfd_ = -1;
    std::vector<epoll_event> events_;
};

} // namespace netlib
