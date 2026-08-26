#include "balancer/Balancer.h"

namespace twigrpc {

uint32_t fnv1a(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

namespace {
// 实例集签名：id/ip/port 任一变化（含地址漂移）都触发环重建
std::string signature(const std::vector<twigrpc::Instance>& instances) {
    std::string sig;
    for (const auto& inst : instances) {
        sig += inst.instance_id();
        sig += '|';
        sig += inst.ip();
        sig += ':';
        sig += std::to_string(inst.port());
        sig += ';';
    }
    return sig;
}
} // namespace

const twigrpc::Instance* RoundRobinBalancer::pick(
    const std::vector<twigrpc::Instance>& instances, std::string_view) {
    if (instances.empty()) return nullptr;
    uint64_t i = seq_.fetch_add(1);
    return &instances[i % instances.size()];
}

void ConsistentHashBalancer::rebuildLocked(
    const std::vector<twigrpc::Instance>& instances) {
    ring_.clear();
    for (size_t idx = 0; idx < instances.size(); ++idx) {
        uint32_t base = fnv1a(instances[idx].instance_id());
        for (int v = 0; v < config::kVnodes; ++v) {
            // vnode key = mix(base ^ mix(v))——打散序号局部性，保证环上均匀
            uint32_t h = mix32(base ^ mix32(static_cast<uint32_t>(v) * 0x9e3779b9u));
            ring_.emplace(h, idx);
        }
    }
    sig_ = signature(instances);
}

const twigrpc::Instance* ConsistentHashBalancer::pick(
    const std::vector<twigrpc::Instance>& instances, std::string_view key) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (instances.empty()) return nullptr;
    std::string sig = signature(instances);
    if (sig_ != sig) rebuildLocked(instances); // 实例集变化（含地址漂移）
    if (ring_.empty()) return nullptr;
    uint32_t h = fnv1a(key);
    auto it = ring_.lower_bound(h);
    if (it == ring_.end()) it = ring_.begin(); // 环回绕
    return &instances[it->second]; // 从调用方快照取实例：杜绝过期数据
}

} // namespace twigrpc
