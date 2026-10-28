// Sampler —— 时间片抽帧器（任务分解 T1.5，详设 §3.4）
// 通道内对象，解码回调线程调用；等价令牌桶（桶容 1）：
//   now >= next_allowed → 放行，next_allowed += period
//   落后超过 2×period → 重置对齐当前时刻（防降级恢复后连发追帧）
// 未放行的帧由调用方 meter.sampled++ 后立即归还 FramePool（fd 生命周期纪律）
//
// 测试性：take() 显式收 now_us（虚拟时钟），单测不依赖真实时间
#pragma once

#include <atomic>
#include <cstdint>

namespace aibox {

class Sampler {
public:
    static constexpr int64_t kNeverTake = INT64_MIN;

    explicit Sampler(uint8_t init_fps = 3) { configure(init_fps); }

    // set_algos/degrade 调整（原子写，另一线程调用安全）
    // 对齐规则：next_allowed = max(next_allowed, last_take + 新 period)
    //   → 降档(3→1fps)后下一秒起放行率立即 ≈ 新值，不突刺也不长等
    void configure(uint8_t fps);

    // 返回 true = 该帧被选中送推理
    bool take(int64_t now_us);

    // RECONFIG 用：清空令牌状态，下一帧立即放行（详设 §3.1 迁移表"drain sampler 令牌"）
    void reset() {
        next_allowed_us_.store(0, std::memory_order_relaxed);
        last_take_us_.store(kNeverTake, std::memory_order_relaxed);
    }

    uint8_t fps() const { return target_fps_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint8_t> target_fps_{3};
    std::atomic<int64_t> next_allowed_us_{0};
    std::atomic<int64_t> last_take_us_{kNeverTake};
};

}  // namespace aibox
