// T1.1 单测（任务分解 §4-T1.1）
// 1) 并发 4 线程混写 in/out/drop/sampled —— 轮次并发 + 静息点不变量精确成立
//    （并发中采样不可行：5 个 relaxed load 的读窗偏差无界，只有静息点可精确断言）
// 2) 差分基准 —— 构造 10 帧/500ms，速率换算正确
#include <atomic>
#include <thread>
#include <vector>

#include "common/EdgeMeter.h"
#include "test_util.h"

using namespace aibox;

namespace {

// 模拟一条边：in 后按角色分流（sampled 拒绝 / depth 入队 / drop 弹最旧 / out 消费）
// 每种分支都保持 in - (out+drop+sampled+depth) 守恒
struct SimEdge {
    EdgeMeter m;
    std::atomic<uint64_t> depth{0};
};

void hammer_round(SimEdge& e, int iters, uint64_t seed) {
    uint64_t rng = seed;
    for (int i = 0; i < iters; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        e.m.in.fetch_add(1, std::memory_order_relaxed);
        switch (rng % 4) {
            case 0: e.m.sampled.fetch_add(1, std::memory_order_relaxed); break;  // 令牌不通过
            case 1: e.depth.fetch_add(1, std::memory_order_relaxed); break;      // 入队成功
            case 2: e.m.drop.fetch_add(1, std::memory_order_relaxed); break;     // 队满弹最旧（in+1, drop+1, 深度不变）
            default:  // 入队成功后又消费
                e.depth.fetch_add(1, std::memory_order_relaxed);
                e.m.out.fetch_add(1, std::memory_order_relaxed);
                e.m.add_latency_us(500);
                e.depth.fetch_sub(1, std::memory_order_relaxed);
                break;
        }
    }
}

void run() {
    // --- 并发混写：20 轮，每轮 4 线程 × 5000 次，静息点精确校验 ---
    {
        SimEdge e;
        for (int round = 0; round < 20; ++round) {
            std::vector<std::thread> workers;
            for (int t = 0; t < 4; ++t) {
                workers.emplace_back(hammer_round, std::ref(e), 5000,
                                     0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(round) * 131 + t));
            }
            for (auto& th : workers) th.join();

            const uint64_t in = e.m.in.load(), out = e.m.out.load(),
                           drop = e.m.drop.load(), sampled = e.m.sampled.load(),
                           depth = e.depth.load();
            CHECK(in == out + drop + sampled + depth);  // 静息点：精确成立（无泄漏）
        }
        CHECK(e.m.in.load() == 20ull * 4 * 5000);
    }

    // --- 差分基准：10 帧/500ms → 20 帧/s ---
    {
        EdgeMeter m;
        for (int i = 0; i < 10; ++i) {
            m.in.fetch_add(1);
            m.out.fetch_add(1);
            m.add_latency_us(2000);  // 每帧 2ms 出队延迟
        }
        m.drop.fetch_add(0);

        EdgeCounters base = snapshot(m);  // t0
        // 再加 10 帧（模拟下一个 500ms 窗口）
        for (int i = 0; i < 10; ++i) {
            m.in.fetch_add(1);
            m.out.fetch_add(1);
            m.add_latency_us(4000);
        }
        EdgeCounters now = snapshot(m);

        EdgeSnapshot s = diff(now, base, 500.0);
        CHECK_NEAR(s.in_rate, 20.0, 1e-9);
        CHECK_NEAR(s.out_rate, 20.0, 1e-9);
        CHECK_NEAR(s.drop_rate, 0.0, 1e-9);
        // 差分只统计窗口内：base 的 2000us 已计入 base 快照，窗口内 10 帧全为 4000us
        CHECK_NEAR(s.avg_latency_ms, 4.0, 1e-9);

        EdgeSnapshot z = diff(now, now, 500.0);  // 零差分
        CHECK_NEAR(z.in_rate, 0.0, 1e-9);
        CHECK_NEAR(z.avg_latency_ms, 0.0, 1e-9);
    }
}

}  // namespace

TEST_MAIN("test_edge_meter")
