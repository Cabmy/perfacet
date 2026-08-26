#pragma once
// netlib 层全局配置：所有可调参数集中于此，禁止散落魔法数字。
// 可被 TWIGRPC_* 环境变量覆盖的逻辑在各使用点读取。

namespace config {

inline constexpr int kBacklog = 1024;                 // listen backlog
inline constexpr int kEpollInitEvents = 1024;          // epoll 事件数组初始大小
inline constexpr size_t kBufferPrepend = 8;            // Buffer prepend 区初始大小
inline constexpr size_t kBufferInit = 1024;            // Buffer 初始大小
inline constexpr size_t kBufferExtra = 65536;          // readFd 栈上 extrabuf 大小

} // namespace config
