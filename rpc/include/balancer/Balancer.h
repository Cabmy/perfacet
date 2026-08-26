#pragma once
// twigrpc::Balancer —— 负载均衡策略（策略模式：新增策略不改客户端代码）。
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "rpc.pb.h"

namespace config {
inline constexpr int kVnodes = 160; // 每物理节点的虚拟节点数
} // namespace config

namespace twigrpc {

struct Balancer {
    virtual ~Balancer() = default;
    // 从健康实例中选一个（返回调用方快照内的指针，不持有内部状态）；
    // key 用于哈希亲和（无亲和语义的策略忽略之）。线程安全。
    virtual const twigrpc::Instance* pick(const std::vector<twigrpc::Instance>& instances,
                                        std::string_view key = "") = 0;
};

struct RoundRobinBalancer : Balancer {
    std::atomic<uint64_t> seq_{0};
    const twigrpc::Instance* pick(const std::vector<twigrpc::Instance>& instances,
                                std::string_view key = "") override;
};

// 一致性哈希：160 虚拟节点/物理节点，std::map<uint32_t, index> 环上 lower_bound 找后继。
// 线程安全：pick 可能被多线程并发调用，ring_ 的重建/查找全程持锁。
struct ConsistentHashBalancer : Balancer {
    const twigrpc::Instance* pick(const std::vector<twigrpc::Instance>& instances,
                                std::string_view key = "") override;

private:
    void rebuildLocked(const std::vector<twigrpc::Instance>& instances);

    std::mutex mtx_;
    std::map<uint32_t, size_t> ring_; // vnode hash -> 实例下标（对齐当前调用方快照）
    std::string sig_;                 // 实例集签名（id/ip/port）：任一变化即重建
};

// FNV-1a 32bit
uint32_t fnv1a(std::string_view s);
// murmur3 finalizer 风格 bit-mix（打散 vnode 序号的局部性）
uint32_t mix32(uint32_t x);

} // namespace twigrpc
