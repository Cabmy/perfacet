#pragma once
// twigrpc::AtomicHook —— 可跨线程热插拔的回调槽位。
// 钩子的装配方（上层 Runtime）在主线程 set，读取方（IO 线程 / worker 线程）
// 在请求路径上 get 并调用：裸 std::function 成员的 set/read 是数据竞争，
// 且替换或摘钩时旧回调可能在别人调用途中被析构。
//
// 回调装进 shared_ptr<const Fn>，槽位读写走 shared_ptr 原子操作：
// - set 是 release，get 是 acquire —— 装配方在 set 之前写的一切
//   （任务树、策略链、客户端连接池）对读取方可见；
// - get 返回强引用，调用期间目标对象必然存活，摘钩只影响后续调用；
// - 读路径不持锁，回调里回调本槽位（如摘钩）也不会死锁。
#include <functional>
#include <memory>
#include <utility>

namespace twigrpc {

template <typename Sig>
class AtomicHook {
public:
    using Fn = std::function<Sig>;

    // 装配 / 摘钩（任意线程）。空 std::function 等价于摘钩。
    void set(Fn fn) {
        auto slot = fn ? std::make_shared<const Fn>(std::move(fn))
                       : std::shared_ptr<const Fn>{};
        std::atomic_store_explicit(&hook_, std::move(slot),
                                   std::memory_order_release);
    }

    // 取快照：非空则调用并返回 true，未装配返回 false。
    // 快照的强引用活到本次调用结束。
    template <typename... Args>
    bool invoke(Args&&... args) const {
        auto fn = std::atomic_load_explicit(&hook_, std::memory_order_acquire);
        if (!fn) return false;
        (*fn)(std::forward<Args>(args)...);
        return true;
    }

private:
    std::shared_ptr<const Fn> hook_;
};

} // namespace twigrpc
