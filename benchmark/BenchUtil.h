#pragma once
// 压测共享工具（readRssKb / Latency / Agg）。
// 只给 benchmark/ 用，不进协议头——Protocol.h 是热路径公共依赖。
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace twigrpc::bench {

// /proc/self/status VmRSS（KB）
inline long readRssKb() {
    std::ifstream f("/proc/self/status");
    std::string key;
    long val = 0;
    std::string unit;
    while (f >> key) {
        if (key == "VmRSS:") {
            f >> val >> unit;
            return val;
        }
        f.ignore(1 << 16, '\n');
    }
    return 0;
}

struct Latency {
    std::vector<double> samples; // us
    void add(double us) { samples.push_back(us); }
    double pct(double p) {
        if (samples.empty()) return 0;
        std::sort(samples.begin(), samples.end());
        size_t idx = static_cast<size_t>(p * (samples.size() - 1));
        return samples[idx];
    }
};

struct Agg {
    std::atomic<long> total{0};
    std::atomic<long> fails{0};
    Latency lat;
    std::mutex latMtx;
};

} // namespace twigrpc::bench
