// TimeUtil —— 稳态时钟工具（详设 §1.2 common/ 清单）
// Sampler/EdgeMeter 差分均基于 steady clock（单调，不受 NTP 跳变影响）
#pragma once

#include <chrono>

namespace aibox {

inline int64_t steady_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace aibox
