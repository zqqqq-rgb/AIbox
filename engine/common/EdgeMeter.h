// EdgeMeter —— 每条数据边一个的帧计数器（任务分解 T1.1，详设 §2.3）
// 不变量: in == out + drop + sampled + 队列当前深度（短暂抖动允许）
//   in      = 到达本边的帧数（含被抽帧器拒绝/入队失败的尝试）
//   out     = 正常离开（消费成功）
//   drop    = 异常丢弃（队列满弹最旧/池空）
//   sampled = 设计内不送推理（Sampler 令牌不通过）
// Monitor 每 500ms 对所有边做 snapshot + diff，差分速率供降级决策（§3.9）
#pragma once

#include <atomic>
#include <cstdint>

namespace aibox {

struct EdgeMeter {
    std::atomic<uint64_t> in{0};
    std::atomic<uint64_t> out{0};
    std::atomic<uint64_t> drop{0};
    std::atomic<uint64_t> sampled{0};
    std::atomic<uint64_t> latency_us_sum{0};

    void add_latency_us(uint64_t us) { latency_us_sum.fetch_add(us, std::memory_order_relaxed); }
};

// 一次一致性-ish 快照（relaxed 读足够：差分只看趋势，不看绝对精确）
struct EdgeCounters {
    uint64_t in = 0, out = 0, drop = 0, sampled = 0, latency_us_sum = 0;
};

inline EdgeCounters snapshot(const EdgeMeter& m) {
    EdgeCounters c;
    c.in = m.in.load(std::memory_order_relaxed);
    c.out = m.out.load(std::memory_order_relaxed);
    c.drop = m.drop.load(std::memory_order_relaxed);
    c.sampled = m.sampled.load(std::memory_order_relaxed);
    c.latency_us_sum = m.latency_us_sum.load(std::memory_order_relaxed);
    return c;
}

// 两次快照差分 → 速率（详设 §2.3 的 diff；速率需要时间基准，故显式传 dt_ms）
struct EdgeSnapshot {
    double in_rate = 0, out_rate = 0, drop_rate = 0, sampled_rate = 0;  // 帧/秒
    double avg_latency_ms = 0;  // 窗口内平均出队延迟
};

EdgeSnapshot diff(const EdgeCounters& now, const EdgeCounters& base, double dt_ms);

}  // namespace aibox
