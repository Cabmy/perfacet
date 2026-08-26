#pragma once
// twigrpc::stats::Collector —— 指标与就绪状态的唯一数据面。
// 热路径按分片 atomic 累加（调用方给 shard key，key % N）；抓取时再聚合。
// AdminEndpoint 只读本对象：不感知 RpcServer / 注册中心 / Grafana。
// 慢调用日志阈值由 P99 导出（仅统计已完成、带耗时的样本；BUSY 不进直方图）。
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace config {
// 指数桶 2^0 .. 2^16 us，覆盖注释中的 100ms 上沿（2^16=65536，末桶 +Inf）
inline constexpr size_t kLatencyBuckets = 18;
inline constexpr double kLatencyBucketMaxUs = 100000.0; // 无样本时慢日志默认阈值
inline constexpr double kSlowCallP99Factor = 1.0; // 慢日志阈值 = P99 × factor
} // namespace config

namespace twigrpc {
namespace stats {

class Collector {
public:
    // shards：分片数（建议 = max(IO 线程, worker)）；0 视为 1
    explicit Collector(size_t shards);

    // serve 前为每个已注册方法预创建分片条目：此后 record 只读查找 map 结构
    // （结构不修改即并发安全），仅原子更新 value——避免首次并发插入的 data race。
    void preRegister(const std::string& method);

    // 已完成请求：计数 + 延迟直方图（P99 / 慢日志只看这里）
    void record(size_t tid, const std::string& method, int status, double us);

    // 未进 worker 的拒绝（BUSY 背压 / 排空）：只加计数，不进直方图，避免 0µs 污染 P99
    void recordReject(size_t tid, const std::string& method, int status);

    // 在途 gauge：RpcServer 受理 +1、回包或断连 -1；排空停机也可读 inFlight()
    void addInFlight(int delta);
    int64_t inFlight() const;

    // 就绪：serve 后为 true，stop/排空起为 false。/healthz 与 ready gauge 同源
    void setReady(bool ready);
    bool ready() const;

    // 聚合输出 Prometheus 文本格式
    std::string renderPrometheus() const;

    // 人类可读摘要
    std::string renderStats() const;

    // P99（微秒，仅已完成样本）
    double p99() const;

    // 慢调用判定阈值（当前 P99；无样本时返回默认）
    double slowThresholdUs() const;

private:
    static constexpr size_t kBuckets = config::kLatencyBuckets;
    // 状态分桶数：覆盖 Protocol.h Status 全部枚举值（OK..REJECTED，共 10）。
    // CANCELLED 是线上状态（服务端取消 handler 后回包），归独立桶而非并入 INTERNAL。
    static constexpr size_t kStatusBuckets = 10;

    struct Shard {
        // requests_total[method][status]
        std::unordered_map<std::string, std::array<std::atomic<uint64_t>, kStatusBuckets>> totals;
        std::array<std::atomic<uint64_t>, kBuckets> buckets{};
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> sumUs{0};
    };

    Shard& shardOf(size_t tid);
    void addTotal(Shard& s, const std::string& method, int status);
    static size_t bucketOf(double us); // us → 桶下标
    static const char* statusLabel(int status);

    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic<int64_t> inFlight_{0};
    std::atomic<bool> ready_{false};
    mutable std::string cached_; // render 期间临时
};

} // namespace stats
} // namespace twigrpc
