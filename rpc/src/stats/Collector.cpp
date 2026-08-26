#include "stats/Collector.h"

#include "codec/Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace twigrpc {
namespace stats {

Collector::Collector(size_t shards) {
    if (shards == 0) shards = 1;
    for (size_t i = 0; i < shards; ++i) {
        shards_.push_back(std::make_unique<Shard>());
    }
    preRegister("__unknown__"); // 未注册方法的兜底桶（避免 record 插入）
}

void Collector::preRegister(const std::string& method) {
    // 每个分片都预留条目；record 的 tid 可能碰撞到任意分片，必须全覆盖。
    for (auto& s : shards_) {
        s->totals[method]; // 触发插入，之后只读
    }
}

Collector::Shard& Collector::shardOf(size_t tid) {
    return *shards_[tid % shards_.size()];
}

const char* Collector::statusLabel(int status) {
    if (status < 0 || status >= static_cast<int>(kStatusBuckets)) return "UNKNOWN";
    return statusName(static_cast<Status>(status));
}

void Collector::addTotal(Shard& s, const std::string& method, int status) {
    auto it = s.totals.find(method);
    if (it == s.totals.end()) it = s.totals.find("__unknown__");
    auto& arr = it->second;
    size_t st = static_cast<size_t>(status < static_cast<int>(kStatusBuckets)
                                        ? status
                                        : static_cast<int>(kStatusBuckets - 1));
    arr[st].fetch_add(1, std::memory_order_relaxed);
}

size_t Collector::bucketOf(double us) {
    // 对数分桶：<1us → 0；其后 le = 2, 4, … 直到末桶。
    // us < 1 时 log2 为负（负浮点转 size_t 是 UB）：统一归入 0 号桶
    if (us < 1.0) return 0;
    size_t b = static_cast<size_t>(std::log2(us)) + 1;
    return std::min(b, config::kLatencyBuckets - 1);
}

void Collector::record(size_t tid, const std::string& method, int status, double us) {
    Shard& s = shardOf(tid);
    addTotal(s, method, status);
    s.buckets[bucketOf(us)].fetch_add(1, std::memory_order_relaxed);
    s.count.fetch_add(1, std::memory_order_relaxed);
    s.sumUs.fetch_add(static_cast<uint64_t>(us), std::memory_order_relaxed);
}

void Collector::recordReject(size_t tid, const std::string& method, int status) {
    addTotal(shardOf(tid), method, status);
}

void Collector::addInFlight(int delta) {
    inFlight_.fetch_add(static_cast<int64_t>(delta), std::memory_order_relaxed);
}

int64_t Collector::inFlight() const {
    return inFlight_.load(std::memory_order_relaxed);
}

void Collector::setReady(bool ready) {
    ready_.store(ready, std::memory_order_relaxed);
}

bool Collector::ready() const {
    return ready_.load(std::memory_order_relaxed);
}

std::string Collector::renderPrometheus() const {
    std::ostringstream out;
    std::map<std::string, std::array<uint64_t, kStatusBuckets>> agg;
    for (auto& s : shards_) {
        for (auto& [method, arr] : s->totals) {
            for (size_t st = 0; st < kStatusBuckets; ++st) {
                agg[method][st] += arr[st].load(std::memory_order_relaxed);
            }
        }
    }
    out << "# HELP twigrpc_server_requests_total Finished or rejected RPCs\n";
    out << "# TYPE twigrpc_server_requests_total counter\n";
    for (auto& [method, arr] : agg) {
        for (size_t st = 0; st < kStatusBuckets; ++st) {
            if (arr[st] == 0 && st != 0) continue;
            out << "twigrpc_server_requests_total{method=\"" << method
                << "\",status=\"" << statusLabel(static_cast<int>(st)) << "\"} "
                << arr[st] << "\n";
        }
    }

    uint64_t bucketCounts[config::kLatencyBuckets] = {};
    uint64_t total = 0, sum = 0;
    for (auto& s : shards_) {
        for (size_t b = 0; b < config::kLatencyBuckets; ++b) {
            bucketCounts[b] += s->buckets[b].load(std::memory_order_relaxed);
        }
        total += s->count.load(std::memory_order_relaxed);
        sum += s->sumUs.load(std::memory_order_relaxed);
    }
    out << "# HELP twigrpc_server_latency_us Completed RPC latency (microseconds)\n";
    out << "# TYPE twigrpc_server_latency_us histogram\n";
    uint64_t cum = 0;
    for (size_t b = 0; b < config::kLatencyBuckets; ++b) {
        cum += bucketCounts[b];
        double le = std::pow(2.0, static_cast<double>(b));
        out << "twigrpc_server_latency_us_bucket{le=\"" << le << "\"} " << cum << "\n";
    }
    out << "twigrpc_server_latency_us_bucket{le=\"+Inf\"} " << total << "\n";
    out << "twigrpc_server_latency_us_count " << total << "\n";
    out << "twigrpc_server_latency_us_sum " << static_cast<double>(sum) << "\n";

    out << "# HELP twigrpc_server_in_flight Accepted RPCs not yet completed\n";
    out << "# TYPE twigrpc_server_in_flight gauge\n";
    out << "twigrpc_server_in_flight " << inFlight() << "\n";

    out << "# HELP twigrpc_server_ready 1 if serving (0 while draining or stopped)\n";
    out << "# TYPE twigrpc_server_ready gauge\n";
    out << "twigrpc_server_ready " << (ready() ? 1 : 0) << "\n";
    return out.str();
}

double Collector::p99() const {
    uint64_t bucketCounts[config::kLatencyBuckets] = {};
    uint64_t total = 0;
    for (auto& s : shards_) {
        for (size_t b = 0; b < config::kLatencyBuckets; ++b) {
            bucketCounts[b] += s->buckets[b].load(std::memory_order_relaxed);
        }
        total += s->count.load(std::memory_order_relaxed);
    }
    if (total == 0) return 0;
    uint64_t target = static_cast<uint64_t>(total * 99 / 100);
    uint64_t cum = 0;
    for (size_t b = 0; b < config::kLatencyBuckets; ++b) {
        cum += bucketCounts[b];
        if (cum >= target) return std::pow(2.0, static_cast<double>(b));
    }
    return config::kLatencyBucketMaxUs;
}

double Collector::slowThresholdUs() const {
    double p = p99();
    return p > 0 ? p * config::kSlowCallP99Factor : config::kLatencyBucketMaxUs;
}

std::string Collector::renderStats() const {
    std::ostringstream out;
    uint64_t completed = 0, sum = 0, rejected = 0;
    for (auto& s : shards_) {
        completed += s->count.load(std::memory_order_relaxed);
        sum += s->sumUs.load(std::memory_order_relaxed);
        for (auto& [_, arr] : s->totals) {
            for (size_t st = 0; st < kStatusBuckets; ++st) {
                rejected += arr[st].load(std::memory_order_relaxed);
            }
        }
    }
    rejected = rejected > completed ? rejected - completed : 0;
    out << "ready: " << (ready() ? 1 : 0) << "\n";
    out << "in_flight: " << inFlight() << "\n";
    out << "completed: " << completed << "\n";
    out << "rejected: " << rejected << "\n";
    if (completed > 0) {
        out << "avg_us: " << (static_cast<double>(sum) / completed) << "\n";
    }
    out << "p99_us: " << p99() << "\n";
    out << "slow_threshold_us: " << slowThresholdUs() << "\n";
    return out.str();
}

} // namespace stats
} // namespace twigrpc
