// T1.5 单测（任务分解 §4-T1.5，虚拟时钟）
// 1) 25fps 流 3fps 采样 10s —— 放行数 ∈ [29,31]
// 2) 运行中 3→1fps —— 下一秒起放行率立即 ≈1
// 3) 人为落后 3 周期 —— 重置对齐当前时刻（不连发追帧）
#include "media/Sampler.h"
#include "test_util.h"

using namespace aibox;

namespace {

constexpr int64_t kFrameUs = 40000;  // 25fps

int run_stream(Sampler& s, int64_t from_us, int64_t to_us) {
    int pass = 0;
    for (int64_t t = from_us; t < to_us; t += kFrameUs) {
        if (s.take(t)) ++pass;
    }
    return pass;
}

void run() {
    // --- 25fps → 3fps，10s ---
    {
        Sampler s(3);
        const int pass = run_stream(s, 0, 10000000);
        CHECK(pass >= 29 && pass <= 31);
    }

    // --- 3→1fps 切换：下一秒起放行率立即 ≈1 ---
    {
        Sampler s(3);
        const int64_t switch_at = 3000000;
        run_stream(s, 0, switch_at);
        s.configure(1);
        // 切换后 4s：期望 4 帧（1/s 立即生效）；窗口边界帧可能 ±1
        const int pass = run_stream(s, switch_at, switch_at + 4000000);
        CHECK(pass >= 3 && pass <= 4);
        CHECK(s.fps() == 1);
    }

    // --- 落后 3 周期：只放行 1 帧，不连发追帧 ---
    {
        Sampler s(3);
        const int64_t period = 1000000 / 3;
        CHECK(s.take(0));                     // t=0 放行
        for (int i = 0; i < 5; ++i) {
            CHECK(!s.take(i * kFrameUs));     // 周期内不放行
        }
        const int64_t t_lag = 0 + 3 * period + 1000;  // 人为落后 3 周期
        CHECK(s.take(t_lag));                 // 重置对齐：放行 1 帧
        // 同一时刻紧邻帧不再放行（防连发）
        CHECK(!s.take(t_lag));
        CHECK(!s.take(t_lag + kFrameUs));
        // 恢复正常周期节奏
        CHECK(s.take(t_lag + period));
    }

    // --- configure(0) 钳位 ---
    {
        Sampler s(1);
        s.configure(0);
        CHECK(s.fps() == 1);
        CHECK(s.take(0));
        CHECK(!s.take(100000));   // 1s 内不放行
        CHECK(s.take(1000000));   // 满 1s 放行
    }
}

}  // namespace

TEST_MAIN("test_sampler")
