#include "media/Sampler.h"

namespace aibox {

void Sampler::configure(uint8_t fps) {
    if (fps == 0) {
        fps = 1;  // 降级阶梯下限 1fps；0 视为 1（详设 §3.9 阶梯无 0 档）
    }
    target_fps_.store(fps, std::memory_order_relaxed);
    const int64_t period = 1000000 / fps;
    const int64_t lt = last_take_us_.load(std::memory_order_relaxed);
    if (lt != kNeverTake) {
        const int64_t want = lt + period;
        const int64_t na = next_allowed_us_.load(std::memory_order_relaxed);
        if (want > na) {
            next_allowed_us_.store(want, std::memory_order_relaxed);
        }
    }
}

bool Sampler::take(int64_t now_us) {
    const int64_t period = 1000000 / target_fps_.load(std::memory_order_relaxed);
    const int64_t na = next_allowed_us_.load(std::memory_order_relaxed);
    if (now_us < na) {
        return false;
    }
    // 放行；落后过多（长时间无帧/降档恢复）→ 重置对齐，不连发追帧
    const int64_t next =
        (now_us - na > 2 * period) ? (now_us + period) : (na + period);
    next_allowed_us_.store(next, std::memory_order_relaxed);
    last_take_us_.store(now_us, std::memory_order_relaxed);
    return true;
}

}  // namespace aibox
